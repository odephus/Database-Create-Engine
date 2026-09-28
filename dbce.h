#ifndef DBCE_H
#define DBCE_H

#include "stdint.h"
#include "stddef.h"
#include "stdbool.h"

#define DBCE_MAGIC 0x45434244

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

typedef struct dbce_engine dbce_t;

typedef void (*dbce_scan_cb)(const char* key, const void* val, size_t val_len, void* udata);

dbce_t* dbce_open(const char* filepath);
dbce_t* dbce_open_config(const char* config_path);
void    dbce_close(dbce_t* db);

bool dbce_set(dbce_t* db, const char* key, const void* val, size_t val_len);
bool dbce_set_async(dbce_t* db, const char* key, const void* val, size_t val_len);
bool dbce_get(dbce_t* db, const char* key, void** out_val, size_t* out_len);
bool dbce_del(dbce_t* db, const char* key);
void dbce_sync_wait(dbce_t* db);
void dbce_scan_all(dbce_t* db, dbce_scan_cb cb, void* udata);

#endif