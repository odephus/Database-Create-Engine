#define _POSIX_C_SOURCE 200809L
#include <dbce.h>
#ifndef DBCE_PHASE1
#include "storage_format.h"
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

enum { KEYS = 1024, READS = 8192, REOPENS = 16, BYTES = 256 };
static double now(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts)) { perror("clock"); exit(1); }
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}
static void check(bool ok, const char* action) {
    if (!ok) { perror(action); exit(1); }
}
static void rate(const char* name, size_t ops, double start) {
    printf("%s %.1f ops/s\n", name, (double)ops / (now() - start));
}
static void acknowledge(dbce_t* db, bool durable) {
#ifndef DBCE_PHASE1
    if (durable) check(dbce_sync_durable(db), "durable sync");
#else
    (void)db;
    (void)durable;
#endif
}

int main(int argc, char** argv) {
    if (argc != 2 || (strcmp(argv[1], "v1") && strcmp(argv[1], "v2") && strcmp(argv[1], "durable"))) {
        fputs("usage: dbce_bench v1|v2|durable\n", stderr);
        return 2;
    }
    bool durable = !strcmp(argv[1], "durable");
#ifdef DBCE_PHASE1
    if (strcmp(argv[1], "v1")) return 2;
#endif
    char dir[] = "/var/tmp/dbce-bench-XXXXXX", path[128];
    check(mkdtemp(dir) != NULL, "create private directory");
    snprintf(path, sizeof(path), "%s/data", dir);
    if (!strcmp(argv[1], "v1")) {
        FILE* f = fopen(path, "wbx");
        check(f && !fclose(f), "create V1");
    }
    dbce_t* db = dbce_open(path);
    check(db != NULL, "open");
    unsigned char value[BYTES];
    memset(value, 0xa5, sizeof(value));
    char keys[KEYS][16];
    for (unsigned i = 0; i < KEYS; ++i) snprintf(keys[i], sizeof(keys[i]), "key%04u", i);
    puts(argv[1]);
    double start = now();
    for (unsigned i = 0; i < KEYS; ++i) {
        check(dbce_set(db, keys[i], value, BYTES), "insert");
        acknowledge(db, durable);
    }
    rate("insert", KEYS, start);
    uint32_t random = 42;
    start = now();
    for (unsigned i = 0; i < READS; ++i) {
        random = random * UINT32_C(1664525) + UINT32_C(1013904223);
        void* data;
        size_t len;
        check(dbce_get(db, keys[random >> 22], &data, &len), "read");
        check(len == BYTES && !memcmp(data, value, len), "value");
        free(data);
    }
    rate("read", READS, start);
    ++value[0];
    start = now();
    for (unsigned i = 0; i < KEYS; ++i) {
        check(dbce_set(db, keys[i], value, BYTES), "update");
        acknowledge(db, durable);
    }
    rate("update", KEYS, start);
    check(dbce_close_checked(db), "close");
    start = now();
    for (unsigned i = 0; i < REOPENS; ++i) {
        db = dbce_open(path);
        check(db && dbce_close_checked(db), "reopen");
    }
    rate("reopen", REOPENS, start);
    db = dbce_open(path);
    check(db != NULL, "open");
    start = now();
    for (unsigned i = 0; i < KEYS; ++i) {
        check(dbce_del(db, keys[i]), "delete");
        acknowledge(db, durable);
    }
    rate("delete", KEYS, start);
    check(dbce_close_checked(db), "close");
#ifndef DBCE_PHASE1
    uint32_t crc = 0;
    start = now();
    for (unsigned i = 0; i < 65536; ++i) crc = dbce_crc32_update(crc, value, BYTES);
    rate("crc256", 65536, start);
    printf("crc=%08x\n", (unsigned)crc);
#endif
    check(!remove(path) && !rmdir(dir), "cleanup");
    return 0;
}
