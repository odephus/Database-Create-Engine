#include "dbce.h"
#include "stdio.h"
#include "stdlib.h"
#include "string.h"
#include "pthread.h"

#define MAX_INDEX_ENTRIES 4096
#define DEFAULT_QUEUE_SIZE 1024

typedef struct {
    char key[64];
    long file_offset;
    uint32_t val_len;
} index_entry_t;

typedef struct {
    char key[64];
    void* val;
    size_t val_len;
    bool is_delete;
} write_task_t;

struct dbce_engine {
    FILE* fp;
    index_entry_t index[MAX_INDEX_ENTRIES];
    size_t index_count;
    pthread_rwlock_t index_lock;

    write_task_t* queue;
    size_t q_capacity;
    size_t q_head;
    size_t q_tail;
    size_t q_count;
    bool running;

    pthread_t writer_thread;
    pthread_mutex_t q_mutex;
    pthread_cond_t q_not_empty;
    pthread_cond_t q_empty;
};

static index_entry_t* find_index_internal(dbce_t* db, const char* key) {
    for (size_t i = 0; i < db->index_count; ++i) {
        if (strcmp(db->index[i].key, key) == 0) {
            return &db->index[i];
        }
    }
    return NULL;
}

static bool internal_append(dbce_t* db, const char* key, const void* val, size_t val_len) {
    size_t key_len = strlen(key);

    dbce_record_header_t header = {
        .magic = DBCE_MAGIC,
        .crc32 = 0,
        .key_len = (uint32_t)key_len,
        .val_len = (uint32_t)val_len
    };

    fseek(db->fp, 0, SEEK_END);
    if (fwrite(&header, sizeof(dbce_record_header_t), 1, db->fp) != 1) return false;
    if (fwrite(key, 1, key_len, db->fp) != key_len) return false;

    long val_offset = ftell(db->fp);
    if (val_len > 0) {
        if (fwrite(val, 1, val_len, db->fp) != val_len) return false;
    }
    fflush(db->fp);

    pthread_rwlock_wrlock(&db->index_lock);
    index_entry_t* entry = find_index_internal(db, key);
    if (!entry && db->index_count < MAX_INDEX_ENTRIES) {
        entry = &db->index[db->index_count++];
        memset(entry->key, 0, sizeof(entry->key));
        memcpy(entry->key, key, key_len < sizeof(entry->key) ? key_len : sizeof(entry->key) - 1);
    }
    if (entry) {
        entry->file_offset = val_offset;
        entry->val_len = (uint32_t)val_len;
    }
    pthread_rwlock_unlock(&db->index_lock);

    return true;
}

static void* writer_worker(void* arg) {
    dbce_t* db = (dbce_t*)arg;

    while (1) {
        pthread_mutex_lock(&db->q_mutex);
        while (db->q_count == 0 && db->running) {
            pthread_cond_wait(&db->q_not_empty, &db->q_mutex);
        }

        if (!db->running && db->q_count == 0) {
            pthread_mutex_unlock(&db->q_mutex);
            break;
        }

        write_task_t task = db->queue[db->q_head];
        db->q_head = (db->q_head + 1) % db->q_capacity;
        db->q_count--;

        if (db->q_count == 0) {
            pthread_cond_broadcast(&db->q_empty);
        }
        pthread_mutex_unlock(&db->q_mutex);

        if (task.is_delete) {
            internal_append(db, task.key, NULL, 0);
        } else {
            internal_append(db, task.key, task.val, task.val_len);
            free(task.val);
        }
    }
    return NULL;
}

static bool parse_config(const char* path, dbce_config_t* cfg) {
    FILE* f = fopen(path, "r");
    if (!f) return false;

    char line[256];
    while (fgets(line, sizeof(line), f)) {
        char* k = strstr(line, "\"db_path\"");
        if (k) {
            char* val_start = strchr(k, ':');
            if (val_start) {
                char* q1 = strchr(val_start, '\"');
                if (q1) {
                    char* q2 = strchr(q1 + 1, '\"');
                    if (q2) {
                        size_t len = (size_t)(q2 - q1 - 1);
                        strncpy(cfg->db_path, q1 + 1, len);
                        cfg->db_path[len] = '\0';
                    }
                }
            }
        }

        k = strstr(line, "\"queue_size\"");
        if (k) {
            char* val_start = strchr(k, ':');
            if (val_start) {
                cfg->queue_size = (size_t)strtoul(val_start + 1, NULL, 10);
            }
        }

        k = strstr(line, "\"sync_interval_ms\"");
        if (k) {
            char* val_start = strchr(k, ':');
            if (val_start) {
                cfg->sync_interval_ms = (uint32_t)strtoul(val_start + 1, NULL, 10);
            }
        }
    }

    fclose(f);
    return true;
}

static dbce_t* dbce_init_engine(const char* filepath, size_t queue_size) {
    if (!filepath) return NULL;

    dbce_t* db = (dbce_t*)malloc(sizeof(dbce_t));
    if (!db) return NULL;

    memset(db, 0, sizeof(dbce_t));
    db->q_capacity = (queue_size > 0) ? queue_size : DEFAULT_QUEUE_SIZE;
    db->queue = (write_task_t*)malloc(sizeof(write_task_t) * db->q_capacity);
    if (!db->queue) {
        free(db);
        return NULL;
    }

    pthread_rwlock_init(&db->index_lock, NULL);
    pthread_mutex_init(&db->q_mutex, NULL);
    pthread_cond_init(&db->q_not_empty, NULL);
    pthread_cond_init(&db->q_empty, NULL);

    db->fp = fopen(filepath, "a+b");
    if (!db->fp) {
        free(db->queue);
        free(db);
        return NULL;
    }

    fseek(db->fp, 0, SEEK_SET);
    dbce_record_header_t header;

    while (fread(&header, sizeof(dbce_record_header_t), 1, db->fp) == 1) {
        if (header.magic != DBCE_MAGIC) break;

        char key_buf[64] = {0};
        if (header.key_len < sizeof(key_buf)) {
            if (fread(key_buf, 1, header.key_len, db->fp) != header.key_len) break;
        } else {
            fseek(db->fp, header.key_len, SEEK_CUR);
            continue;
        }

        long val_offset = ftell(db->fp);
        fseek(db->fp, header.val_len, SEEK_CUR);

        index_entry_t* entry = find_index_internal(db, key_buf);
        if (!entry && db->index_count < MAX_INDEX_ENTRIES) {
            entry = &db->index[db->index_count++];
            memset(entry->key, 0, sizeof(entry->key));
            memcpy(entry->key, key_buf, header.key_len < sizeof(entry->key) ? header.key_len : sizeof(entry->key) - 1);
        }
        if (entry) {
            entry->file_offset = val_offset;
            entry->val_len = header.val_len;
        }
    }

    db->running = true;
    pthread_create(&db->writer_thread, NULL, writer_worker, db);

    return db;
}

dbce_t* dbce_open(const char* filepath) {
    return dbce_init_engine(filepath, DEFAULT_QUEUE_SIZE);
}

dbce_t* dbce_open_config(const char* config_path) {
    dbce_config_t cfg;
    memset(&cfg, 0, sizeof(dbce_config_t));
    cfg.queue_size = DEFAULT_QUEUE_SIZE;
    strncpy(cfg.db_path, "data.dbce", sizeof(cfg.db_path) - 1);

    if (parse_config(config_path, &cfg)) {
        return dbce_init_engine(cfg.db_path, cfg.queue_size);
    }
    return dbce_init_engine("data.dbce", DEFAULT_QUEUE_SIZE);
}

bool dbce_set_async(dbce_t* db, const char* key, const void* val, size_t val_len) {
    if (!db || !key || !val || !db->running) return false;

    void* val_copy = malloc(val_len);
    if (!val_copy) return false;
    memcpy(val_copy, val, val_len);

    pthread_mutex_lock(&db->q_mutex);
    if (db->q_count >= db->q_capacity) {
        pthread_mutex_unlock(&db->q_mutex);
        free(val_copy);
        return false;
    }

    write_task_t* task = &db->queue[db->q_tail];
    memset(task->key, 0, sizeof(task->key));
    strncpy(task->key, key, sizeof(task->key) - 1);
    task->val = val_copy;
    task->val_len = val_len;
    task->is_delete = false;

    db->q_tail = (db->q_tail + 1) % db->q_capacity;
    db->q_count++;

    pthread_cond_signal(&db->q_not_empty);
    pthread_mutex_unlock(&db->q_mutex);

    return true;
}

void dbce_sync_wait(dbce_t* db) {
    if (!db) return;
    pthread_mutex_lock(&db->q_mutex);
    while (db->q_count > 0) {
        pthread_cond_wait(&db->q_empty, &db->q_mutex);
    }
    pthread_mutex_unlock(&db->q_mutex);
}

bool dbce_set(dbce_t* db, const char* key, const void* val, size_t val_len) {
    if (!dbce_set_async(db, key, val, val_len)) return false;
    dbce_sync_wait(db);
    return true;
}

bool dbce_get(dbce_t* db, const char* key, void** out_val, size_t* out_len) {
    if (!db || !key || !out_val || !out_len || !db->fp) return false;

    pthread_rwlock_rdlock(&db->index_lock);
    index_entry_t* entry = find_index_internal(db, key);

    if (!entry || entry->val_len == 0) {
        pthread_rwlock_unlock(&db->index_lock);
        return false;
    }

    long offset = entry->file_offset;
    uint32_t len = entry->val_len;
    pthread_rwlock_unlock(&db->index_lock);

    void* buf = malloc(len + 1);
    if (!buf) return false;

    fseek(db->fp, offset, SEEK_SET);
    size_t read_bytes = fread(buf, 1, len, db->fp);
    if (read_bytes != len) {
        free(buf);
        return false;
    }

    ((char*)buf)[len] = '\0';
    *out_val = buf;
    *out_len = len;
    return true;
}

bool dbce_del(dbce_t* db, const char* key) {
    if (!db || !key) return false;
    pthread_mutex_lock(&db->q_mutex);
    if (db->q_count >= db->q_capacity) {
        pthread_mutex_unlock(&db->q_mutex);
        return false;
    }

    write_task_t* task = &db->queue[db->q_tail];
    memset(task->key, 0, sizeof(task->key));
    strncpy(task->key, key, sizeof(task->key) - 1);
    task->val = NULL;
    task->val_len = 0;
    task->is_delete = true;

    db->q_tail = (db->q_tail + 1) % db->q_capacity;
    db->q_count++;

    pthread_cond_signal(&db->q_not_empty);
    pthread_mutex_unlock(&db->q_mutex);

    dbce_sync_wait(db);
    return true;
}

void dbce_scan_all(dbce_t* db, dbce_scan_cb cb, void* udata) {
    if (!db || !cb || !db->fp) return;

    dbce_sync_wait(db);

    pthread_rwlock_rdlock(&db->index_lock);
    for (size_t i = 0; i < db->index_count; ++i) {
        if (db->index[i].val_len > 0) {
            void* buf = malloc(db->index[i].val_len + 1);
            if (buf) {
                fseek(db->fp, db->index[i].file_offset, SEEK_SET);
                fread(buf, 1, db->index[i].val_len, db->fp);
                ((char*)buf)[db->index[i].val_len] = '\0';
                cb(db->index[i].key, buf, db->index[i].val_len, udata);
                free(buf);
            }
        }
    }
    pthread_rwlock_unlock(&db->index_lock);
}

void dbce_close(dbce_t* db) {
    if (!db) return;

    pthread_mutex_lock(&db->q_mutex);
    db->running = false;
    pthread_cond_signal(&db->q_not_empty);
    pthread_mutex_unlock(&db->q_mutex);

    pthread_join(db->writer_thread, NULL);

    pthread_rwlock_destroy(&db->index_lock);
    pthread_mutex_destroy(&db->q_mutex);
    pthread_cond_destroy(&db->q_not_empty);
    pthread_cond_destroy(&db->q_empty);

    if (db->fp) fclose(db->fp);
    if (db->queue) free(db->queue);
    free(db);
}