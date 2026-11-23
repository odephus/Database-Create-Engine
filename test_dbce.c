/* Compile the engine into this test translation unit to inject private faults. */
#include "dbce.c"
#ifdef _WIN32
#include <windows.h>
#include <process.h>
#else
#include <unistd.h>
#include <sys/wait.h>
#include <dirent.h>
#include <signal.h>
#include <sys/resource.h>
#endif

#define CHECK(expr) do { if (!(expr)) { \
    fprintf(stderr, "%s:%d: %s (errno=%d)\n", __FILE__, __LINE__, #expr, errno); \
    exit(1); \
} } while (0)
#define FAILS(expr, code) do { CHECK(!(expr)); CHECK(errno == (code)); } while (0)

static char path[1024];
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed = PTHREAD_COND_INITIALIZER;
static bool gated, barrier_entered, closing_started;
static unsigned arrivals, permits;
static int worker_error, header_error;

static int before_worker_create(void) { return worker_error; }
static int after_header_write(void) { return header_error; }

static void after_dequeue(void) {
    pthread_mutex_lock(&lock);
    if (gated) {
        unsigned ticket = ++arrivals;
        pthread_cond_broadcast(&changed);
        while (permits < ticket) pthread_cond_wait(&changed, &lock);
    }
    pthread_mutex_unlock(&lock);
}

static void before_barrier_wait(void) {
    pthread_mutex_lock(&lock);
    barrier_entered = true;
    pthread_cond_broadcast(&changed);
    pthread_mutex_unlock(&lock);
}

static void admission_closed(void) {
    pthread_mutex_lock(&lock);
    closing_started = true;
    pthread_cond_broadcast(&changed);
    pthread_mutex_unlock(&lock);
}

static void gate_start(void) {
    closing_started = false;
    gated = true;
    arrivals = permits = 0;
    barrier_entered = false;
}

static void await_dequeue(unsigned n) {
    pthread_mutex_lock(&lock);
    while (arrivals < n) pthread_cond_wait(&changed, &lock);
    pthread_mutex_unlock(&lock);
}

static void release_writes(unsigned n) {
    pthread_mutex_lock(&lock);
    permits = n;
    pthread_cond_broadcast(&changed);
    pthread_mutex_unlock(&lock);
}

static void temp_file(char* dest, size_t cap) {
#ifdef _WIN32
    char dir[MAX_PATH], name[MAX_PATH];
    DWORD len = GetTempPathA(MAX_PATH, dir);
    CHECK(len && len < MAX_PATH);
    CHECK(GetTempFileNameA(dir, "dbc", 0, name));
    CHECK(strlen(name) < cap);
    strcpy(dest, name);
#else
    const char* dir = getenv("TMPDIR");
    if (!dir || !*dir) dir = "/tmp";
    int n = snprintf(dest, cap, "%s/dbce-XXXXXX", dir);
    CHECK(n > 0 && (size_t)n < cap);
    int fd = mkstemp(dest);
    CHECK(fd >= 0 && close(fd) == 0);
#endif
}

static dbce_t* open_temp(void) {
    dbce_t* db = dbce_open(path);
    CHECK(db);
    return db;
}

static void expect_value(dbce_t* db, const char* key, const void* expected, size_t n) {
    void* value = NULL;
    size_t len = 0;
    CHECK(dbce_get(db, key, &value, &len));
    CHECK(len == n && memcmp(value, expected, n) == 0);
    CHECK(((char*)value)[len] == '\0');
    free(value);
}

static void missing(dbce_t* db, const char* key) {
    void* p;
    size_t n;
    FAILS(dbce_get(db, key, &p, &n), ENOENT);
    CHECK(p == NULL && n == 0);
}

static void round_trip(void) {
    dbce_t* db = open_temp();
    CHECK(dbce_set(db, "key", "old", 3));
    CHECK(dbce_set(db, "key", "new", 3));
    expect_value(db, "key", "new", 3);
    CHECK(dbce_close_checked(db));
    db = open_temp();
    expect_value(db, "key", "new", 3);
    CHECK(dbce_del(db, "key"));
    missing(db, "key");
    dbce_close(db);
    db = open_temp();
    missing(db, "key");
    CHECK(dbce_close_checked(db));
}

static void binary(void) {
    unsigned char bytes[] = {0, 255, 'x', 0};
    dbce_t* db = open_temp();
    CHECK(dbce_set_async(db, "binary", bytes, sizeof(bytes)));
    bytes[2] = 'y';
    CHECK(dbce_sync_wait_checked(db));
    bytes[2] = 'x';
    expect_value(db, "binary", bytes, sizeof(bytes));
    dbce_close(db);
    db = open_temp();
    expect_value(db, "binary", bytes, sizeof(bytes));
    dbce_close(db);
}

static void validation(void) {
    char key[65];
    memset(key, 'k', sizeof(key));
    key[63] = '\0';
    dbce_t* db = open_temp();
    CHECK(dbce_set(db, key, "x", 1));
    key[63] = 'z'; key[64] = '\0';
    FAILS(dbce_set_async(db, key, "y", 1), ENAMETOOLONG);
    FAILS(dbce_del(db, key), ENAMETOOLONG);
    key[63] = '\0';
    FAILS(dbce_set(db, key, "z", 0), EINVAL);
    FAILS(dbce_set(db, key, NULL, 1), EINVAL);
    FAILS(dbce_set_async(db, "", "z", 1), EINVAL);
    FAILS(dbce_set(db, key, "z", SIZE_MAX), EOVERFLOW);
    expect_value(db, key, "x", 1);
    CHECK(dbce_close_checked(db));
}

typedef struct { dbce_t* db; bool success; } call_t;
static void* sync_set(void* arg) {
    call_t* c = arg;
    c->success = dbce_set(c->db, "in-flight", "v", 1);
    if (c->success) expect_value(c->db, "in-flight", "v", 1);
    return NULL;
}
static void* barrier(void* arg) {
    call_t* c = arg;
    c->success = dbce_sync_wait_checked(c->db);
    return NULL;
}

static void completion(void) {
    gate_start();
    dbce_t* db = dbce_init_engine(path, 1);
    CHECK(db);
    call_t write = {db, false}, sync = {db, false};
    pthread_t a, b;
    CHECK(!pthread_create(&a, NULL, sync_set, &write));
    await_dequeue(1);
    missing(db, "in-flight");
    CHECK(!pthread_create(&b, NULL, barrier, &sync));
    pthread_mutex_lock(&lock);
    while (!barrier_entered) pthread_cond_wait(&changed, &lock);
    pthread_mutex_unlock(&lock);
    CHECK(dbce_set_async(db, "later", "y", 1));
    FAILS(dbce_set_async(db, "excess", "z", 1), EAGAIN);
    release_writes(1);
    await_dequeue(2);
    /* The later sequence is in flight while both earlier calls return. */
    CHECK(!pthread_join(a, NULL) && write.success);
    CHECK(!pthread_join(b, NULL) && sync.success);
    expect_value(db, "in-flight", "v", 1);
    missing(db, "later");
    release_writes(2);
    CHECK(dbce_sync_wait_checked(db));
    expect_value(db, "later", "y", 1);
    CHECK(dbce_close_checked(db));
    gated = false;
}

typedef struct { dbce_t* db; int id; pthread_barrier_t* start; } thread_arg_t;

static void* produce(void* arg) {
    thread_arg_t* t = arg;
    char key[24], value[32];
    (void)snprintf(key, sizeof(key), "producer-%d", t->id);
    pthread_barrier_wait(t->start);
    for (int i = 0; i < 100; ++i) {
        int n = snprintf(value, sizeof(value), "%d/%d", t->id, i);
        CHECK(n > 0 && (size_t)n < sizeof(value));
        CHECK(dbce_set(t->db, key, value, (size_t)n));
        expect_value(t->db, key, value, (size_t)n);
    }
    return NULL;
}

static void* contend(void* arg) {
    thread_arg_t* t = arg;
    unsigned char blob[256];
    pthread_barrier_wait(t->start);
    for (unsigned i = 0; i < 300; ++i) {
        if (!t->id) {
            memset(blob, (int)(i % 256), sizeof(blob));
            CHECK(dbce_set(t->db, "shared", blob, sizeof(blob)));
        } else {
            void* p;
            size_t n;
            CHECK(dbce_get(t->db, "shared", &p, &n) && n == sizeof(blob));
            memset(blob, *(unsigned char*)p, n);
            CHECK(memcmp(blob, p, n) == 0);
            free(p);
        }
    }
    return NULL;
}

static void run_threads(void* (*fn)(void*)) {
    dbce_t* db = open_temp();
    unsigned char blob[256] = {0};
    CHECK(dbce_set(db, "shared", blob, sizeof(blob)));
    thread_arg_t args[6];
    pthread_t threads[6];
    pthread_barrier_t start;
    CHECK(!pthread_barrier_init(&start, NULL, 6));
    for (int i = 0; i < 6; ++i) {
        args[i] = (thread_arg_t){db, i, &start};
        CHECK(!pthread_create(&threads[i], NULL, fn, &args[i]));
    }
    for (int i = 0; i < 6; ++i) CHECK(!pthread_join(threads[i], NULL));
    CHECK(!pthread_barrier_destroy(&start));
    CHECK(dbce_close_checked(db));
}

static void producers(void) { run_threads(produce); }
static void readers(void) { run_threads(contend); }

static void init_failure(void) {
    worker_error = EAGAIN;
    for (int i = 0; i < 10; ++i) FAILS(dbce_open(path), EAGAIN);
    worker_error = 0;
    FAILS(dbce_init_engine(path, SIZE_MAX), EOVERFLOW);
    dbce_t* db = open_temp();
    CHECK(dbce_close_checked(db));
}

static void partial_write(void) {
    dbce_t* db = open_temp();
    CHECK(dbce_set(db, "keep", "old", 3));
    header_error = ENOSPC;
    FAILS(dbce_del(db, "keep"), ENOSPC);
    expect_value(db, "keep", "old", 3);
    FAILS(dbce_set_async(db, "x", "b", 1), ENOSPC);
    FAILS(dbce_sync_wait_checked(db), ENOSPC);
    FAILS(dbce_close_checked(db), ENOSPC);
    header_error = 0;
    FAILS(dbce_open(path), EILSEQ);
    FILE* f = fopen(path, "rb");
    CHECK(f && !fseek(f, 0, SEEK_END));
    CHECK(ftell(f) == 2 * 16 + 4 + 3);
    CHECK(!fclose(f));
}

#ifndef _WIN32
/* Force a real short write from the filesystem. */
static void file_limit(void) {
    struct rlimit saved, limit;
    CHECK(!getrlimit(RLIMIT_FSIZE, &saved));
    limit = saved;
    limit.rlim_cur = 18;
    dbce_t* db = open_temp();
    void (*old_handler)(int) = signal(SIGXFSZ, SIG_IGN);
    CHECK(old_handler != SIG_ERR);
    CHECK(!setrlimit(RLIMIT_FSIZE, &limit));
    CHECK(!dbce_set(db, "long-key", "value", 5));
    int error = errno;
    CHECK(error == EFBIG || error == EIO);
    CHECK(!setrlimit(RLIMIT_FSIZE, &saved));
    CHECK(signal(SIGXFSZ, old_handler) != SIG_ERR);
    missing(db, "long-key");
    FAILS(dbce_close_checked(db), error);
    FAILS(dbce_open(path), EILSEQ);
}
#endif

#if !defined(_WIN32) && SIZE_MAX > UINT32_MAX
static void sparse_offsets(void) {
    FILE* f = fopen(path, "wb");
    CHECK(f);
    uint32_t h[] = {DBCE_MAGIC, 0, 1, UINT32_MAX};
    CHECK(fwrite(h, sizeof(h), 1, f) == 1);
    CHECK(fputc('a', f) != EOF);
    CHECK(!file_seek(f, (int64_t)UINT32_MAX - 1, SEEK_CUR));
    CHECK(fputc(0, f) != EOF && !fclose(f));
    dbce_t* db = open_temp();
    CHECK(dbce_set(db, "b", "beyond 4 GiB", 12));
    expect_value(db, "b", "beyond 4 GiB", 12);
    CHECK(dbce_close_checked(db));
    db = open_temp();
    expect_value(db, "b", "beyond 4 GiB", 12);
    CHECK(dbce_close_checked(db));
}
#endif

static void put_bytes(FILE* f, const void* bytes, size_t n) {
    CHECK(fwrite(bytes, 1, n, f) == n);
}

static void malformed(uint32_t magic, uint32_t k, uint32_t v, size_t header_bytes,
                      const char* payload, size_t actual) {
    uint32_t header[] = {magic, 0, k, v};
    uint32_t valid[] = {DBCE_MAGIC, 0, 1, 1};
    unsigned char expected[128], copy[128];
    size_t prefix = sizeof(valid) + 2;
    size_t n = prefix + header_bytes + actual;
    CHECK(n <= sizeof(expected));
    memcpy(expected, valid, sizeof(valid));
    memcpy(expected + sizeof(valid), "ab", 2);
    memcpy(expected + prefix, header, header_bytes);
    memcpy(expected + prefix + header_bytes, payload, actual);
    FILE* f = fopen(path, "wb");
    CHECK(f);
    put_bytes(f, expected, n);
    CHECK(!fclose(f));
    FAILS(dbce_open(path), EILSEQ);
    f = fopen(path, "rb");
    CHECK(f && fread(copy, 1, sizeof(copy), f) == n && !ferror(f));
    CHECK(!memcmp(copy, expected, n));
    CHECK(!fclose(f));
}

static void short_header(void) { malformed(DBCE_MAGIC, 1, 1, 15, "", 0); }
static void short_key(void) { malformed(DBCE_MAGIC, 3, 0, 16, "ab", 2); }
static void short_value(void) { malformed(DBCE_MAGIC, 1, 3, 16, "ab", 2); }
static void bad_magic(void) { malformed(1, 1, 1, 16, "kv", 2); }
static void bad_lengths(void) {
    malformed(DBCE_MAGIC, 0, 1, 16, "x", 1);
    malformed(DBCE_MAGIC, 64, 0, 16, "", 0);
    malformed(DBCE_MAGIC, 3, 0, 16, "a\0b", 3);
    malformed(DBCE_MAGIC, 1, UINT32_MAX, 16, "x", 1);
    malformed(DBCE_MAGIC, UINT32_MAX, UINT32_MAX, 16, "x", 1);
}

static void record(FILE* f, const char* key, const char* val) {
    uint32_t header[] = {DBCE_MAGIC, 0, (uint32_t)strlen(key), (uint32_t)strlen(val)};
    put_bytes(f, header, sizeof(header));
    put_bytes(f, key, header[2]);
    put_bytes(f, val, header[3]);
}

static void legacy(void) {
    FILE* f = fopen(path, "wb");
    CHECK(f);
    record(f, "a", "old");
    record(f, "b", "b");
    record(f, "a", "new");
    record(f, "b", "");
    CHECK(!fclose(f));
    dbce_t* db = open_temp();
    expect_value(db, "a", "new", 3);
    missing(db, "b");
    CHECK(dbce_set(db, "b", "ok", 2));
    dbce_close(db);
    f = fopen(path, "rb");
    CHECK(f && !fseek(f, -19, SEEK_END));
    uint32_t header[4];
    char payload[3];
    CHECK(fread(header, sizeof(header), 1, f) == 1);
    CHECK(header[0] == DBCE_MAGIC && !header[1] && header[2] == 1 && header[3] == 2);
    CHECK(fread(payload, 1, 3, f) == 3 && !memcmp(payload, "bok", 3));
    CHECK(!fclose(f));
}

static void capacity(void) {
    FILE* f = fopen(path, "wb");
    CHECK(f);
    char key[20];
    for (int i = 0; i < DBCE_INDEX_CAPACITY - 1; ++i) {
        (void)snprintf(key, sizeof(key), "k%d", i);
        record(f, key, "x");
    }
    CHECK(!fclose(f));
    dbce_t* db = open_temp();
    CHECK(dbce_set(db, "last", "x", 1));
    CHECK(dbce_set_async(db, "extra", "x", 1));
    FAILS(dbce_sync_wait_checked(db), ENOSPC);
    FAILS(dbce_set(db, "extra", "x", 1), ENOSPC);
    CHECK(dbce_set(db, "last", "updated", 7));
    FAILS(dbce_close_checked(db), ENOSPC);
    db = open_temp();
    missing(db, "extra");
    expect_value(db, "last", "updated", 7);
    CHECK(dbce_close_checked(db));
    f = fopen(path, "ab");
    CHECK(f);
    record(f, "extra", "x");
    CHECK(!fclose(f));
    FAILS(dbce_open(path), ENOSPC);
}

typedef struct { dbce_t* db; int seen; } scan_t;
static void reenter(const char* key, const void* value, size_t len, void* arg) {
    scan_t* s = arg;
    CHECK(len == 1 && !memcmp(value, "x", 1));
    if (s->seen++ == 0) {
        CHECK(!strcmp(key, "a"));
        expect_value(s->db, key, "x", 1);
        CHECK(dbce_del(s->db, "b"));
        CHECK(dbce_set(s->db, "c", "y", 1));
    } else CHECK(!strcmp(key, "b"));
}

static void scan(void) {
    dbce_t* db = open_temp();
    CHECK(dbce_set(db, "a", "x", 1));
    CHECK(dbce_set_async(db, "b", "x", 1));
    scan_t s = {db, 0};
    CHECK(dbce_scan_all_checked(db, reenter, &s));
    CHECK(s.seen == 2);
    missing(db, "b");
    CHECK(dbce_close_checked(db));
}

static void* closer(void* arg) {
    call_t* c = arg;
    c->success = dbce_close_checked(c->db);
    return NULL;
}

static void drain(void) {
    gate_start();
    dbce_t* db = open_temp();
    CHECK(dbce_set_async(db, "first", "v", 1));
    await_dequeue(1);
    for (int i = 0; i < 40; ++i) CHECK(dbce_set_async(db, "last", &i, sizeof(i)));
    call_t closing = {db, false};
    pthread_t thread;
    CHECK(!pthread_create(&thread, NULL, closer, &closing));
    pthread_mutex_lock(&lock);
    while (!closing_started) pthread_cond_wait(&changed, &lock);
    pthread_mutex_unlock(&lock);
    release_writes(41);
    CHECK(!pthread_join(thread, NULL) && closing.success);
    gated = false;
    db = open_temp();
    expect_value(db, "first", "v", 1);
    int last = 39;
    expect_value(db, "last", &last, sizeof(last));
    CHECK(dbce_close_checked(db));
}


/* Independent wire fixture: SET a=00ff10, SET e="", DELETE a. */
static const unsigned char golden[] = {
    137,68,66,50,13,10,26,10,2,0,16,0,0,0,0,0,
    68,82,50,33,1,0,0,0,1,0,0,0,3,0,0,0,110,208,65,55,85,195,231,61,97,0,255,16,
    68,82,50,33,1,0,0,0,1,0,0,0,0,0,0,0,126,143,39,107,90,43,235,213,101,
    68,82,50,33,2,0,0,0,1,0,0,0,0,0,0,0,169,39,128,209,236,98,57,90,97
};

static void fixture(const char* name, const void* bytes, size_t len) {
    FILE* f = fopen(name, "wb");
    CHECK(f);
    put_bytes(f, bytes, len);
    CHECK(!fclose(f));
}

static void expect_file(const char* name, const unsigned char* expected, size_t n) {
    unsigned char bytes[256];
    CHECK(n < sizeof(bytes));
    FILE* fp = fopen(name, "rb");
    CHECK(fp && fread(bytes, 1, sizeof(bytes), fp) == n && !ferror(fp));
    CHECK(!memcmp(bytes, expected, n) && !fclose(fp));
}

static void empty_row(const char* key, const void* val, size_t n, void* arg) {
    CHECK(!strcmp(key, "e") && val && !n);
    ++*(int*)arg;
}

static void wire(void) {
    CHECK(dbce_crc32_update(0, NULL, 0) == 0);
    CHECK(dbce_crc32_update(0, "123456789", 9) == UINT32_C(0xcbf43926));
    CHECK(dbce_crc32_update(dbce_crc32_update(0, "1234", 4), "56789", 5) == UINT32_C(0xcbf43926));
    CHECK(!remove(path)); /* A missing pathname creates V2. */
    dbce_t* db = open_temp();
    const unsigned char value[] = {0,255,16};
    CHECK(dbce_set(db, "a", value, sizeof(value)));
    CHECK(dbce_set_async(db, "e", NULL, 0));
    CHECK(dbce_sync_durable(db));
    CHECK(dbce_close_checked(db));
    db = open_temp();
    expect_value(db, "a", value, sizeof(value));
    expect_value(db, "e", "", 0);
    CHECK(dbce_del(db, "a"));
    missing(db, "a");
    CHECK(dbce_close_checked(db));
    expect_file(path, golden, sizeof(golden));
    db = open_temp();
    int seen = 0;
    CHECK(dbce_scan_all_checked(db, empty_row, &seen) && seen == 1);
    CHECK(dbce_close_checked(db));
}

static void corrupt_v2(void) {
    unsigned char bytes[sizeof(golden)];
    for (size_t i = 0; i < sizeof(golden); ++i) {
        memcpy(bytes, golden, sizeof(bytes));
        bytes[i] ^= 1;
        fixture(path, bytes, sizeof(bytes));
        CHECK(!dbce_open(path));
        dbce_recovery_t report;
        CHECK(!dbce_recover(path, NULL, &report));
        CHECK(report.discarded_bytes == 0);
        expect_file(path, bytes, sizeof(bytes));
    }
    /* Re-seal the header after changing empty SET to DELETE. The independent
       record CRC must still reject these otherwise valid metadata. */
    memcpy(bytes, golden, sizeof(bytes));
    bytes[44 + 4] = OP_DELETE;
    uint32_t header_crc = dbce_crc32_update(0, bytes + 44, 20);
    for (unsigned i = 0; i < 4; ++i) bytes[44 + 20 + i] = (unsigned char)(header_crc >> (8*i));
    fixture(path, bytes, sizeof(bytes));
    FAILS(dbce_open(path), EBADMSG);
    char dest[1024];
    temp_file(dest, sizeof(dest));
    CHECK(!remove(dest));
    dbce_recovery_t damaged;
    FAILS(dbce_recover(path, dest, &damaged), EBADMSG);
    CHECK(damaged.issue == DBCE_ISSUE_RECORD_CRC && damaged.verified_bytes == 44);
    CHECK(!fopen(dest, "rb") && errno == ENOENT);
    for (size_t n = 1; n < V2_FILE_BYTES; ++n) {
        fixture(path, golden, n);
        FAILS(dbce_open(path), EILSEQ);
    }
    fixture(path, golden, sizeof(golden));
    dbce_recovery_t report;
    CHECK(dbce_recover(path, NULL, &report) && !report.discarded_bytes);
}

static void truncated_v2(void) {
    unsigned char bytes[72];
    memcpy(bytes, golden, 44);
    memcpy(bytes+44, golden+16, 28);
    char dest[1024], backup[1040];
    temp_file(dest, sizeof(dest));
    CHECK(!remove(dest));
    snprintf(backup, sizeof(backup), "%s.bak", dest);
    for (size_t cut = 1; cut < 28; ++cut) {
        fixture(path, bytes, 44 + cut);
        FAILS(dbce_open(path), EILSEQ);
        dbce_recovery_t report;
        CHECK(dbce_recover(path, NULL, &report) && report.discarded_bytes == cut);
        CHECK(report.tail == (cut < 24 ? DBCE_TAIL_HEADER : cut == 24 ? DBCE_TAIL_KEY : DBCE_TAIL_VALUE));
        CHECK(dbce_recover(path, dest, &report));
        expect_file(dest, bytes, 44);
        expect_file(backup, bytes, 44 + cut);
        expect_file(path, bytes, 44 + cut);
        FAILS(dbce_recover(path, dest, &report), EEXIST);
        CHECK(!remove(dest));
        FAILS(dbce_recover(path, dest, &report), EEXIST);
        expect_file(backup, bytes, 44 + cut);
        CHECK(!remove(backup));
    }
    bytes[44] ^= 1;
    fixture(path, bytes, 45);
    dbce_recovery_t report;
    FAILS(dbce_recover(path, dest, &report), EILSEQ);
}

static int migration_failure;
static bool exit_during_migration;
int dbce_test_migration_header(void) {
    if (exit_during_migration) _Exit(86);
    return migration_failure;
}
static void migrate_v1(void) {
    FILE* fp = fopen(path, "wb");
    CHECK(fp);
    record(fp, "a", "old");
    record(fp, "a", "new");
    record(fp, "b", "");
    CHECK(!fclose(fp));
    unsigned char binary[] = {0, 4, 0, 255};
    dbce_t* db = open_temp();
    CHECK(dbce_set(db, "c", binary, sizeof(binary)));
    CHECK(dbce_close_checked(db));
    unsigned char before[128];
    fp = fopen(path, "rb");
    CHECK(fp);
    size_t n = fread(before, 1, sizeof(before), fp);
    CHECK(n < sizeof(before) && !ferror(fp) && !fclose(fp));
    char dest[1024];
    temp_file(dest, sizeof(dest));
    FAILS(dbce_migrate(path, path), EEXIST);
    FAILS(dbce_migrate(path, dest), EEXIST);
    expect_file(dest, before, 0);
    CHECK(!remove(dest));
    migration_failure = ENOSPC;
    FAILS(dbce_migrate(path, dest), ENOSPC);
    migration_failure = 0;
    CHECK(dbce_migrate(path, dest)); /* No leftover destination after failure. */
    expect_file(path, before, n);
    db = dbce_open(dest);
    CHECK(db && db->v2);
    expect_value(db, "a", "new", 3);
    expect_value(db, "c", binary, sizeof(binary));
    missing(db, "b");
    CHECK(dbce_close_checked(db));
    fp = fopen(dest, "rb");
    CHECK(fp && !fseek(fp, 0, SEEK_END));
    CHECK(ftell(fp) == 16 + 4*24 + 4 + 3 + 3 + 4); /* History and tombstone retained. */
    CHECK(!fclose(fp) && !remove(dest));
#ifndef _WIN32
    CHECK(!link(path, dest));
    FAILS(dbce_migrate(path, dest), EEXIST);
    CHECK(!remove(dest));
#endif
}

#ifndef _WIN32
static void interrupted_migration(void) {
    FILE* fp = fopen(path, "wb");
    CHECK(fp);
    record(fp, "key", "value");
    CHECK(!fclose(fp));
    char dir[1200], dest[1250];
    snprintf(dir, sizeof(dir), "%s.XXXXXX", path);
    CHECK(mkdtemp(dir));
    snprintf(dest, sizeof(dest), "%s/new", dir);
    pid_t pid = fork();
    CHECK(pid >= 0);
    if (!pid) {
        exit_during_migration = true;
        (void)dbce_migrate(path, dest);
        _Exit(99);
    }
    int status;
    CHECK(waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 86);
    CHECK(access(dest, F_OK) == -1 && errno == ENOENT);
    CHECK(dbce_migrate(path, dest)); /* The killed process no longer owns the lock. */
    dbce_t* db = dbce_open(dest);
    CHECK(db);
    expect_value(db, "key", "value", 5);
    CHECK(dbce_close_checked(db) && !remove(dest));
    DIR* d = opendir(dir);
    CHECK(d);
    struct dirent* item;
    while ((item = readdir(d))) {
        if (!strncmp(item->d_name, "new.tmp.", 8)) CHECK(!unlinkat(dirfd(d), item->d_name, 0));
    }
    CHECK(!closedir(d) && !rmdir(dir));
}
#endif

static void durable_failure(void) {
    CHECK(!remove(path));
    dbce_t* db = open_temp();
    CHECK(dbce_set_async(db, "key", "v", 1));
    sync_failure = EIO;
    FAILS(dbce_sync_durable(db), EIO);
    sync_failure = 0;
    FAILS(dbce_sync_durable(db), EIO);
    expect_value(db, "key", "v", 1);
    FAILS(dbce_close_checked(db), EIO);
}

static void scan_failure(void) {
    CHECK(!remove(path));
    dbce_t* db = open_temp();
    header_error = ENOSPC;
    CHECK(dbce_set_async(db, "e", NULL, 0));
    int seen = 0;
    FAILS(dbce_scan_all_checked(db, empty_row, &seen), ENOSPC);
    CHECK(!seen);
    FAILS(dbce_close_checked(db), ENOSPC);
    header_error = 0;
}

static const char* executable;
static int child_probe(void) {
#ifdef _WIN32
    char quoted[sizeof(path)+3];
    snprintf(quoted, sizeof(quoted), "\"%s\"", path);
    return (int)_spawnl(_P_WAIT, executable, "dbce_tests", "--probe", quoted, (char*)NULL);
#else
    pid_t pid = fork();
    CHECK(pid >= 0);
    if (!pid) { execl(executable, executable, "--probe", path, (char*)NULL); _exit(99); }
    int status;
    CHECK(waitpid(pid, &status, 0) == pid && WIFEXITED(status));
    return WEXITSTATUS(status);
#endif
}

static void exclusive(void) {
    CHECK(!remove(path));
    dbce_t* db = open_temp();
    FAILS(dbce_open(path), EBUSY);
    CHECK(child_probe() == 23);
    CHECK(dbce_close_checked(db));
    CHECK(child_probe() == 0);
}

static void v2_capacity(void) {
    CHECK(!remove(path));
    dbce_t* db = open_temp();
    char key[16];
    for (unsigned i = 0; i < DBCE_INDEX_CAPACITY; ++i) {
        snprintf(key, sizeof(key), "k%u", i);
        CHECK(dbce_set(db, key, NULL, 0));
    }
    FAILS(dbce_set(db, "extra", NULL, 0), ENOSPC);
    CHECK(dbce_set(db, "k0", "v", 1));
    FAILS(dbce_close_checked(db), ENOSPC);
    db = open_temp();
    expect_value(db, "k0", "v", 1);
    missing(db, "extra");
    CHECK(dbce_close_checked(db));
}

static void v2_concurrent(void) {
    CHECK(!remove(path));
    run_threads(produce);
    run_threads(contend);
}
int main(int argc, char** argv) {
    executable = argv[0];
    if (argc == 3 && !strcmp(argv[1], "--probe")) {
        dbce_t* db = dbce_open(argv[2]);
        if (!db) return errno == EBUSY ? 23 : 24;
        return dbce_close_checked(db) ? 0 : 25;
    }
    struct { const char* name; void (*run)(void); } cases[] = {
        {"V2 golden wire, CRC vectors, empty and delete", wire},
        {"V2 bit flips and invalid headers", corrupt_v2}, {"V2 tail recovery and backup", truncated_v2},
        {"V1 migration, failure and collisions", migrate_v1}, {"durability failure", durable_failure},
        {"checked scan after async failure", scan_failure}, {"multi-process locking", exclusive},
        {"V2 concurrent readers and producers", v2_concurrent}, {"V2 capacity", v2_capacity},
        {"set/get/delete/reopen", round_trip}, {"binary + async copy", binary},
        {"key/value limits + empty values", validation}, {"in-flight fence + queue full", completion},
        {"multiple producers + read after write", producers}, {"concurrent readers/writer", readers},
        {"worker init + allocation overflow", init_failure}, {"failed delete + sealed tail", partial_write},
#ifndef _WIN32
        {"OS short write", file_limit}, {"process exit during migration", interrupted_migration},
#if SIZE_MAX > UINT32_MAX
        {"64-bit sparse offsets", sparse_offsets},
#endif
#endif
        {"truncated header", short_header}, {"truncated key", short_key}, {"truncated value", short_value},
        {"bad magic", bad_magic}, {"invalid lengths/embedded NUL", bad_lengths},
        {"legacy wire format", legacy}, {"4096-entry boundary", capacity},
        {"reentrant snapshot", scan}, {"close with pending writes", drain}
    };
    for (size_t i = 0; i < sizeof(cases)/sizeof(cases[0]); ++i) {
        temp_file(path, sizeof(path));
        cases[i].run();
        CHECK(!remove(path));
        printf("PASS %s\n", cases[i].name);
    }
    return 0;
}
