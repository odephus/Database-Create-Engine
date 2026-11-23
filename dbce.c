#define _FILE_OFFSET_BITS 64
#define _POSIX_C_SOURCE 200809L
#include "storage_format.h"
#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


#define DEFAULT_QUEUE_SIZE 1024
_Static_assert(sizeof(dbce_record_header_t) == 16, "legacy header layout");

typedef struct {
    char key[DBCE_MAX_KEY + 1];
    int64_t file_offset;
    uint32_t val_len;
    bool present;
} index_entry_t;

typedef struct {
    char key[DBCE_MAX_KEY + 1];
    void* val;
    uint32_t val_len;
    unsigned char op;
    uint64_t sequence;
    int* result;  /* Synchronous submitter's stack, protected by q_mutex. */
} write_task_t;

struct dbce_engine {
    FILE* fp;
    bool v2;
    index_entry_t index[DBCE_INDEX_CAPACITY];
    size_t index_count;
    pthread_mutex_t storage_mutex;
    int append_error, durable_error;

    write_task_t* queue;
    size_t q_capacity, q_head, q_tail, q_count;
    bool running;
    uint64_t submitted, completed, first_failure;
    int first_error, writer_error;
    pthread_t writer_thread;
    pthread_mutex_t q_mutex;
    pthread_cond_t q_not_empty, completion;
};

/* Tests supply deterministic worker and I/O fault points in their own build. */
#ifdef DBCE_TESTING
static int before_worker_create(void);
static int sync_failure;
static void after_dequeue(void);
static int after_header_write(void);
static void before_barrier_wait(void);
static void admission_closed(void);
#endif

static bool fail(int error) {
    errno = error;
    return false;
}


static bool valid_key(const char* key) {
    if (!key || !*key) return fail(EINVAL);
    if (strlen(key) > DBCE_MAX_KEY) return fail(ENAMETOOLONG);
    return true;
}

static bool value_fits(uint64_t len) {
    return len <= UINT32_MAX && len < SIZE_MAX;  /* Extra byte on get. */
}

static index_entry_t* find_index_internal(dbce_t* db, const char* key) {
    for (size_t i = 0; i < db->index_count; ++i) {
        if (strcmp(db->index[i].key, key) == 0) return &db->index[i];
    }
    return NULL;
}

static void publish(dbce_t* db, index_entry_t* entry, const char* key,
                    int64_t offset, uint32_t len, bool present) {
    if (!entry) {
        entry = &db->index[db->index_count++];
        strcpy(entry->key, key);
    }
    entry->file_offset = offset;
    entry->val_len = len;
    entry->present = present;
}

/* Called only under storage_mutex. A failed append seals the file for this handle. */
static int internal_append(dbce_t* db, const write_task_t* task) {
    if (db->append_error) return db->append_error;
    index_entry_t* entry = find_index_internal(db, task->key);
    if (!entry && db->index_count == DBCE_INDEX_CAPACITY) return ENOSPC;
    dbce_record_header_t h = {DBCE_MAGIC, 0, (uint32_t)strlen(task->key), task->val_len};
    unsigned char wire[V2_RECORD_BYTES];
    const void* header = &h;
    size_t header_size = sizeof(h);
    if (db->v2) {
        uint32_t crc = dbce_v2_prefix(wire, task->op, task->key, h.val_len);
        dbce_v2_finish_header(wire, dbce_crc32_update(crc, task->val, h.val_len));
        header = wire;
        header_size = sizeof(wire);
    }
    errno = 0;
    if (file_seek(db->fp, 0, SEEK_END) != 0) goto io_failure;
    int64_t start = file_tell(db->fp);
    if (start < 0) goto io_failure;
    int64_t prefix = (int64_t)header_size + h.key_len;
    if (start > INT64_MAX - prefix - h.val_len) return EFBIG;
    if (dbce_write_exact(db->fp, header, header_size)) goto io_failure;
#ifdef DBCE_TESTING
    if ((errno = after_header_write()) != 0) goto io_failure;
#endif
    if (fwrite(task->key, 1, h.key_len, db->fp) != h.key_len ||
        (h.val_len && fwrite(task->val, 1, h.val_len, db->fp) != h.val_len) ||
        fflush(db->fp) != 0 || ferror(db->fp)) goto io_failure;
    publish(db, entry, task->key, start + prefix, h.val_len, task->op == OP_SET);
    return 0;

io_failure:
    db->append_error = dbce_io_error();
    clearerr(db->fp);  /* Existing committed records remain readable. */
    return db->append_error;
}

static void* writer_worker(void* arg) {
    dbce_t* db = arg;
    pthread_mutex_lock(&db->q_mutex);
    while (db->running || db->q_count) {
        if (!db->q_count) {
            pthread_cond_wait(&db->q_not_empty, &db->q_mutex);
            continue;
        }
        write_task_t task = db->queue[db->q_head];
        db->q_head = (db->q_head + 1) % db->q_capacity;
        db->q_count--;
        pthread_mutex_unlock(&db->q_mutex);
#ifdef DBCE_TESTING
        after_dequeue();
#endif
        pthread_mutex_lock(&db->storage_mutex);
        int error = internal_append(db, &task);
        int append_error = db->append_error;
        pthread_mutex_unlock(&db->storage_mutex);
        free(task.val);

        pthread_mutex_lock(&db->q_mutex);
        db->writer_error = append_error;
        if (error && !db->first_failure) {
            db->first_failure = task.sequence;
            db->first_error = error;
        }
        if (task.result) *task.result = error;
        db->completed = task.sequence;
        pthread_cond_broadcast(&db->completion);
    }
    pthread_mutex_unlock(&db->q_mutex);
    return NULL;
}

static int index_record(const log_record* rec, void* arg) {
    dbce_t* db = arg;
    index_entry_t* entry = find_index_internal(db, rec->key);
    if (!entry && db->index_count == DBCE_INDEX_CAPACITY) return ENOSPC;
    publish(db, entry, rec->key, rec->offset, rec->length, rec->op == OP_SET);
    return 0;
}

int dbce_validate_log(FILE* fp, log_info* info) {
    dbce_t* scratch = calloc(1, sizeof(*scratch));
    if (!scratch) return ENOMEM;
    int error = dbce_walk_log(fp, info, index_record, scratch);
    free(scratch);
    return error;
}

/* Publish a fully initialized, locked header. A pre-existing empty file is V1. */
static FILE* open_database(const char* path) {
    FILE* fp = dbce_locked_existing(path, true);
    if (fp || errno != ENOENT) return fp;
    stage_t stage = {0};
    int error = dbce_stage_create(&stage, path);
    if (!error) error = dbce_write_v2_file_header(stage.fp);
    if (!error) error = dbce_stage_publish(&stage, path);
    if (!error) return stage.fp;
    dbce_stage_finish(&stage, error);
    if (error == EEXIST) return dbce_locked_existing(path, true);
    errno = error;
    return NULL;
}

static dbce_t* dbce_init_engine(const char* filepath, size_t queue_size) {
    if (!filepath || !*filepath || !queue_size) { errno = EINVAL; return NULL; }
    if (queue_size > SIZE_MAX / sizeof(write_task_t)) { errno = EOVERFLOW; return NULL; }
    dbce_t* db = calloc(1, sizeof(*db));
    if (!db) return NULL;
    int error = ENOMEM;
    db->q_capacity = queue_size;
    db->queue = calloc(queue_size, sizeof(*db->queue));
    if (!db->queue) goto free_engine;
    db->fp = open_database(filepath);
    if (!db->fp) { error = dbce_io_error(); goto free_queue; }
    log_info info;
    error = dbce_walk_log(db->fp, &info, index_record, db);
    db->v2 = info.v2;
    if (error) goto close_file;
    error = pthread_mutex_init(&db->storage_mutex, NULL);
    if (error) goto close_file;
    error = pthread_mutex_init(&db->q_mutex, NULL);
    if (error) goto destroy_storage;
    error = pthread_cond_init(&db->q_not_empty, NULL);
    if (error) goto destroy_queue_lock;
    error = pthread_cond_init(&db->completion, NULL);
    if (error) goto destroy_not_empty;
    db->running = true;
#ifdef DBCE_TESTING
    error = before_worker_create();
    if (!error)
#endif
        error = pthread_create(&db->writer_thread, NULL, writer_worker, db);
    if (!error) return db;

    pthread_cond_destroy(&db->completion);
destroy_not_empty:
    pthread_cond_destroy(&db->q_not_empty);
destroy_queue_lock:
    pthread_mutex_destroy(&db->q_mutex);
destroy_storage:
    pthread_mutex_destroy(&db->storage_mutex);
close_file:
    fclose(db->fp);  /* Preserve the original initialization failure. */
free_queue:
    free(db->queue);
free_engine:
    free(db);
    errno = error;
    return NULL;
}

dbce_t* dbce_open(const char* filepath) {
    return dbce_init_engine(filepath, DEFAULT_QUEUE_SIZE);
}

/* The legacy configuration uses one field per line. Missing config uses defaults. */
dbce_t* dbce_open_config(const char* config_path) {
    dbce_config_t cfg = {.db_path = "data.dbce", .queue_size = DEFAULT_QUEUE_SIZE};
    if (!config_path) { errno = EINVAL; return NULL; }
    FILE* fp = fopen(config_path, "r");
    if (!fp) {
        if (errno == ENOENT) return dbce_init_engine(cfg.db_path, cfg.queue_size);
        return NULL;
    }
    char line[1024];
    int error = 0;
    while (!error && fgets(line, sizeof(line), fp)) {
        if (!strchr(line, '\n') && !feof(fp)) { error = EOVERFLOW; break; }
        char* field = strstr(line, "\"db_path\"");
        if (field) {
            char* colon = strchr(field, ':');
            char* start = colon ? strchr(colon + 1, '"') : NULL;
            char* end = start ? strchr(start + 1, '"') : NULL;
            if (!end || end == start + 1) { error = EINVAL; break; }
            size_t n = (size_t)(end - (start + 1));
            if (n >= sizeof(cfg.db_path)) { error = ENAMETOOLONG; break; }
            memcpy(cfg.db_path, start + 1, n);
            cfg.db_path[n] = '\0';
        }
        field = strstr(line, "\"queue_size\"");
        if (field) {
            char* p = strchr(field, ':');
            if (!p) { error = EINVAL; break; }
            do { ++p; } while (isspace((unsigned char)*p));
            if (!isdigit((unsigned char)*p)) { error = EINVAL; break; }
            errno = 0;
            uintmax_t n = strtoumax(p, &p, 10);
            if (errno == ERANGE || n > SIZE_MAX) { error = EOVERFLOW; break; }
            while (isspace((unsigned char)*p)) ++p;
            if (*p && *p != ',' && *p != '}') { error = EINVAL; break; }
            cfg.queue_size = n ? (size_t)n : DEFAULT_QUEUE_SIZE;
        }
    }
    if (ferror(fp) && !error) error = dbce_io_error();
    if (fclose(fp) != 0 && !error) error = dbce_io_error();
    if (error) { errno = error; return NULL; }
    return dbce_init_engine(cfg.db_path, cfg.queue_size);
}

/* q_mutex is held from admission through publication of the queue slot. */
static int enqueue(dbce_t* db, const char* key, const void* val, size_t len, unsigned char op, int* result) {
    if (!db->running) return ECANCELED;
    if (db->writer_error) return db->writer_error;
    if (db->q_count == db->q_capacity) return EAGAIN;
    if (db->submitted == UINT64_MAX) return EOVERFLOW;

    write_task_t* task = &db->queue[db->q_tail];
    task->val = len ? malloc(len) : NULL;
    if (len && !task->val) return ENOMEM;
    if (len) memcpy(task->val, val, len);
    strcpy(task->key, key);
    task->val_len = (uint32_t)len;
    task->op = op;
    task->result = result;
    task->sequence = ++db->submitted;
    db->q_tail = (db->q_tail + 1) % db->q_capacity;
    db->q_count++;
    pthread_cond_signal(&db->q_not_empty);
    return 0;
}

static bool submit(dbce_t* db, const char* key, const void* val, size_t len, unsigned char op, bool wait) {
    if (!db) return fail(EINVAL);
    if (!valid_key(key)) return false;
    if (op == OP_SET && ((!val && len) || (!len && !db->v2))) return fail(EINVAL);
    if (!value_fits(len)) return fail(EOVERFLOW);
    int result = 0;
    pthread_mutex_lock(&db->q_mutex);
    int error = enqueue(db, key, val, len, op, wait ? &result : NULL);
    uint64_t seq = db->submitted;
    while (!error && wait && db->completed < seq)
        pthread_cond_wait(&db->completion, &db->q_mutex);
    pthread_mutex_unlock(&db->q_mutex);
    if (error) return fail(error);
    return result ? fail(result) : true;
}

bool dbce_set_async(dbce_t* db, const char* key, const void* val, size_t len) {
    return submit(db, key, val, len, OP_SET, false);
}

bool dbce_set(dbce_t* db, const char* key, const void* val, size_t len) {
    return submit(db, key, val, len, OP_SET, true);
}

bool dbce_del(dbce_t* db, const char* key) {
    return submit(db, key, NULL, 0, OP_DELETE, true);
}

bool dbce_sync_wait_checked(dbce_t* db) {
    if (!db) return fail(EINVAL);
    pthread_mutex_lock(&db->q_mutex);
    uint64_t target = db->submitted;
    while (db->completed < target) {
#ifdef DBCE_TESTING
        before_barrier_wait();
#endif
        pthread_cond_wait(&db->completion, &db->q_mutex);
    }
    int error = db->first_failure && db->first_failure <= target ? db->first_error : 0;
    pthread_mutex_unlock(&db->q_mutex);
    return error ? fail(error) : true;
}

void dbce_sync_wait(dbce_t* db) {
    (void)dbce_sync_wait_checked(db);
}

bool dbce_sync_durable(dbce_t* db) {
    if (!dbce_sync_wait_checked(db)) return false;
    pthread_mutex_lock(&db->storage_mutex);
    int error = db->durable_error ? db->durable_error : db->append_error;
#ifdef DBCE_TESTING
    if (!error) error = sync_failure;
#endif
    if (!error) error = dbce_disk_sync(db->fp);
    db->durable_error = error;
    pthread_mutex_unlock(&db->storage_mutex);
    return error ? fail(error) : true;
}

static int read_value(dbce_t* db, const index_entry_t* entry, void** out) {
    size_t n = entry->val_len;
    char* buf = malloc(n + 1);
    if (!buf) return ENOMEM;
    int error = 0;
    if (file_seek(db->fp, entry->file_offset, SEEK_SET) != 0) error = dbce_io_error();
    else error = dbce_read_exact(db->fp, buf, n);
    if (error) { free(buf); return error; }
    buf[n] = '\0';
    *out = buf;
    return 0;
}

bool dbce_get(dbce_t* db, const char* key, void** out_val, size_t* out_len) {
    if (!db || !out_val || !out_len) return fail(EINVAL);
    *out_val = NULL;
    *out_len = 0;
    if (!valid_key(key)) return false;
    pthread_mutex_lock(&db->storage_mutex);
    index_entry_t* entry = find_index_internal(db, key);
    int error = ENOENT;
    if (entry && entry->present) {
        error = read_value(db, entry, out_val);
        if (!error) *out_len = entry->val_len;
    }
    pthread_mutex_unlock(&db->storage_mutex);
    return error ? fail(error) : true;
}

bool dbce_scan_all_checked(dbce_t* db, dbce_scan_cb cb, void* udata) {
    if (!db || !cb) return fail(EINVAL);
    if (!dbce_sync_wait_checked(db)) return false;
    /* Old offsets remain valid for the lifetime of the append-only log. */
    pthread_mutex_lock(&db->storage_mutex);
    size_t count = db->index_count;
    index_entry_t* snapshot = count ? malloc(count * sizeof(*snapshot)) : NULL;
    if (snapshot) memcpy(snapshot, db->index, count * sizeof(*snapshot));
    pthread_mutex_unlock(&db->storage_mutex);
    if (count && !snapshot) return fail(ENOMEM);
    int error = 0;
    for (size_t i = 0; i < count; ++i) {
        if (!snapshot[i].present) continue;
        void* buf = NULL;
        pthread_mutex_lock(&db->storage_mutex);
        error = read_value(db, &snapshot[i], &buf);
        pthread_mutex_unlock(&db->storage_mutex);
        if (error) break;
        cb(snapshot[i].key, buf, snapshot[i].val_len, udata);
        free(buf);
    }
    free(snapshot);
    return error ? fail(error) : true;
}

void dbce_scan_all(dbce_t* db, dbce_scan_cb cb, void* udata) {
    (void)dbce_scan_all_checked(db, cb, udata);
}

bool dbce_close_checked(dbce_t* db) {
    if (!db) return fail(EINVAL);
    pthread_mutex_lock(&db->q_mutex);
    db->running = false;
#ifdef DBCE_TESTING
    admission_closed();
#endif
    pthread_cond_signal(&db->q_not_empty);
    pthread_mutex_unlock(&db->q_mutex);
    int error = pthread_join(db->writer_thread, NULL);
    if (error) return fail(error);  /* Cannot release a possibly live worker. */
    error = db->first_error ? db->first_error : db->durable_error;
    if (fclose(db->fp) != 0 && !error) error = dbce_io_error();
    pthread_cond_destroy(&db->completion);
    pthread_cond_destroy(&db->q_not_empty);
    pthread_mutex_destroy(&db->q_mutex);
    pthread_mutex_destroy(&db->storage_mutex);
    free(db->queue);
    free(db);
    return error ? fail(error) : true;
}

void dbce_close(dbce_t* db) {
    if (db) (void)dbce_close_checked(db);
}
