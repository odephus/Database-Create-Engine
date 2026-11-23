#ifndef DBCE_H
#define DBCE_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define DBCE_MAGIC 0x45434244
#define DBCE_MAX_KEY 63
#define DBCE_INDEX_CAPACITY 4096

#pragma pack(push, 1)
typedef struct {
    uint32_t magic;
    uint32_t crc32;
    uint32_t key_len;
    uint32_t val_len;
} dbce_record_header_t;
#pragma pack(pop)

typedef struct {
    char db_path[256];
    size_t queue_size;
    uint32_t sync_interval_ms;
} dbce_config_t;

typedef enum { DBCE_TAIL_NONE, DBCE_TAIL_HEADER, DBCE_TAIL_KEY, DBCE_TAIL_VALUE } dbce_tail_t;
typedef enum {
    DBCE_ISSUE_NONE, DBCE_ISSUE_FILE_HEADER, DBCE_ISSUE_MAGIC, DBCE_ISSUE_VERSION,
    DBCE_ISSUE_OPERATION, DBCE_ISSUE_FIELDS, DBCE_ISSUE_LENGTH, DBCE_ISSUE_KEY,
    DBCE_ISSUE_HEADER_CRC, DBCE_ISSUE_RECORD_CRC
} dbce_issue_t;
typedef struct {
    dbce_tail_t tail;
    dbce_issue_t issue;
    uint64_t verified_bytes, discarded_bytes;
} dbce_recovery_t;

/* NULL destination inspects. Recovery creates destination plus destination.bak. */
bool dbce_recover(const char* source, const char* destination, dbce_recovery_t* report);
bool dbce_migrate(const char* source, const char* destination);

typedef struct dbce_engine dbce_t;
typedef void (*dbce_scan_cb)(const char* key, const void* val, size_t val_len, void* udata);

/* Failed pointer/bool operations set errno on the calling thread. */
dbce_t* dbce_open(const char* filepath);
dbce_t* dbce_open_config(const char* config_path);
/* The caller owns shutdown exclusively. Close drains accepted writes. */
bool dbce_close_checked(dbce_t* db);
void dbce_close(dbce_t* db);

/* Set/delete acknowledge a flushed record. Async acknowledges queue admission. */
bool dbce_set(dbce_t* db, const char* key, const void* val, size_t val_len);
bool dbce_set_async(dbce_t* db, const char* key, const void* val, size_t val_len);
bool dbce_get(dbce_t* db, const char* key, void** out_val, size_t* out_len);
bool dbce_del(dbce_t* db, const char* key);
/* Cumulative from open through the submission sequence captured at entry. */
bool dbce_sync_wait_checked(dbce_t* db);
void dbce_sync_wait(dbce_t* db);
/* Checked completion followed by an OS file synchronization. */
bool dbce_sync_durable(dbce_t* db);
/* Snapshot callbacks may re-enter the engine and borrow key/value until return. */
bool dbce_scan_all_checked(dbce_t* db, dbce_scan_cb cb, void* udata);
void dbce_scan_all(dbce_t* db, dbce_scan_cb cb, void* udata);

#endif
