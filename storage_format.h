#ifndef DBCE_STORAGE_FORMAT_H
#define DBCE_STORAGE_FORMAT_H
#include "dbce.h"
#include "storage_io.h"

enum { V2_FILE_BYTES = 16, V2_RECORD_BYTES = 24, OP_SET = 1, OP_DELETE = 2 };
typedef struct {
    char key[DBCE_MAX_KEY + 1];
    int64_t offset;
    uint32_t length;
    unsigned char op;
} log_record;
typedef struct {
    bool v2;
    int64_t end, verified;
    dbce_tail_t tail;
    dbce_issue_t issue;
} log_info;
typedef int (*record_visitor)(const log_record*, void*);

uint32_t dbce_crc32_update(uint32_t crc, const void* data, size_t len);
int dbce_write_v2_file_header(FILE* fp);
uint32_t dbce_v2_prefix(unsigned char header[V2_RECORD_BYTES], unsigned char op,
                   const char* key, uint32_t len);
void dbce_v2_finish_header(unsigned char header[V2_RECORD_BYTES], uint32_t record_crc);
int dbce_validate_log(FILE* fp, log_info* info);
int dbce_walk_log(FILE* fp, log_info* info, record_visitor visitor, void* context);
/* Process a bounded byte range at the current stream position. */
int dbce_stream_bytes(FILE* src, FILE* dest, uint64_t count, uint32_t* crc);
#endif
