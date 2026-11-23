#ifndef DBCE_STORAGE_IO_H
#define DBCE_STORAGE_IO_H
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#ifdef _WIN32
#define file_seek _fseeki64
#define file_tell _ftelli64
#else
#define file_seek fseeko
#define file_tell ftello
#endif

typedef struct { FILE* fp; char* name; } stage_t;
FILE* dbce_locked_existing(const char* path, bool writable);
int dbce_disk_sync(FILE* fp);
int dbce_stage_create(stage_t* stage, const char* dest);
int dbce_stage_publish(stage_t* stage, const char* dest);
int dbce_stage_finish(stage_t* stage, int error);
int dbce_read_exact(FILE* fp, void* data, size_t len);
int dbce_write_exact(FILE* fp, const void* data, size_t len);
int dbce_io_error(void);
#endif
