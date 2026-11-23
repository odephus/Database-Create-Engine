#define _POSIX_C_SOURCE 200809L
#define _FILE_OFFSET_BITS 64
#include "storage_io.h"
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#ifdef _WIN32
#include <windows.h>
#include <io.h>
#else
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

int dbce_io_error(void) { return errno ? errno : EIO; }

int dbce_read_exact(FILE* fp, void* data, size_t len) {
    errno = 0;
    if (!len || fread(data, 1, len, fp) == len) return 0;
    return ferror(fp) ? dbce_io_error() : EILSEQ;
}

int dbce_write_exact(FILE* fp, const void* data, size_t len) {
    errno = 0;
    return !len || fwrite(data, 1, len, fp) == len ? 0 : dbce_io_error();
}

#ifdef _WIN32
static int win_error(DWORD code) {
    switch (code) {
    case ERROR_SHARING_VIOLATION: case ERROR_LOCK_VIOLATION: return EBUSY;
    case ERROR_ALREADY_EXISTS: case ERROR_FILE_EXISTS: return EEXIST;
    case ERROR_FILE_NOT_FOUND: case ERROR_PATH_NOT_FOUND: return ENOENT;
    case ERROR_ACCESS_DENIED: return EACCES;
    case ERROR_DISK_FULL: case ERROR_HANDLE_DISK_FULL: return ENOSPC;
    default: return EIO;
    }
}
#endif

static FILE* open_locked(const char* path, bool writable, bool create) {
    int error = 0;
#ifdef _WIN32
    DWORD access = GENERIC_READ | (writable ? GENERIC_WRITE : 0u);
    HANDLE h = CreateFileA(path, access, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           NULL, create ? CREATE_NEW : OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) { errno = win_error(GetLastError()); return NULL; }
    OVERLAPPED range = {0};
    if (!LockFileEx(h, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0, &range))
        error = win_error(GetLastError());
    int fd = -1;
    if (!error) {
        fd = _open_osfhandle((intptr_t)h, _O_BINARY | (writable ? _O_RDWR : _O_RDONLY));
        if (fd < 0) error = dbce_io_error();
    }
    if (error) { CloseHandle(h); if (create) remove(path); errno = error; return NULL; }
    FILE* fp = _fdopen(fd, writable ? "r+b" : "rb");
    if (!fp) { error = dbce_io_error(); _close(fd); }
#else
    int flags = (writable ? O_RDWR : O_RDONLY) | O_CLOEXEC | O_NONBLOCK;
    if (create) flags |= O_CREAT | O_EXCL;
    int fd = open(path, flags, 0600);
    if (fd < 0) return NULL;
    struct stat st;
    if (fstat(fd, &st)) error = dbce_io_error();
    else if (!S_ISREG(st.st_mode)) error = EINVAL;
    else if (flock(fd, LOCK_EX | LOCK_NB)) error = errno == EWOULDBLOCK ? EBUSY : dbce_io_error();
    FILE* fp = NULL;
    if (!error) {
        fp = fdopen(fd, writable ? "r+b" : "rb");
        if (!fp) error = dbce_io_error();
    }
    if (error) close(fd);
#endif
    if (!fp) { if (create) remove(path); errno = error; return NULL; }
    if (setvbuf(fp, NULL, _IONBF, 0)) { fclose(fp); if (create) remove(path); errno = EIO; return NULL; }
    return fp;
}

FILE* dbce_locked_existing(const char* path, bool writable) {
    return open_locked(path, writable, false);
}

int dbce_disk_sync(FILE* fp) {
    if (fflush(fp)) return dbce_io_error();
#ifdef _WIN32
    HANDLE h = (HANDLE)_get_osfhandle(_fileno(fp));
    return FlushFileBuffers(h) ? 0 : win_error(GetLastError());
#else
    while (fsync(fileno(fp))) { if (errno != EINTR) return dbce_io_error(); }
    return 0;
#endif
}

int dbce_stage_create(stage_t* stage, const char* dest) {
    static atomic_uint counter;
    size_t n = strlen(dest);
    if (n > SIZE_MAX - 64) return ENAMETOOLONG;
    char* path = malloc(n + 64);
    if (!path) return ENOMEM;
#ifdef _WIN32
    unsigned long pid = GetCurrentProcessId();
#else
    unsigned long pid = (unsigned long)getpid();
#endif
    int error = EEXIST;
    for (int attempt = 0; attempt < 64; ++attempt) {
        snprintf(path, n + 64, "%s.tmp.%lu.%u", dest, pid, atomic_fetch_add(&counter, 1));
        FILE* fp = open_locked(path, true, true);
        if (fp) { stage->name = path; stage->fp = fp; return 0; }
        error = dbce_io_error();
        if (error != EEXIST) break;
    }
    free(path);
    return error;
}

#ifndef _WIN32
static int sync_parent(const char* path) {
    char* dir = strdup(path);
    if (!dir) return ENOMEM;
    char* last = strrchr(dir, '/');
    if (last) *last = 0;
    int fd = open(last ? (*dir ? dir : "/") : ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    int error = fd < 0 ? dbce_io_error() : 0;
    free(dir);
    if (!error) {
        while (fsync(fd)) {
            if (errno != EINTR) { error = dbce_io_error(); break; }
        }
        if (close(fd) && !error) error = dbce_io_error();
    }
    return error;
}
#endif

/* No replacement, including hard-link aliases. Keep the file locked across publication. */
int dbce_stage_publish(stage_t* stage, const char* dest) {
    int error = dbce_disk_sync(stage->fp);
    if (error) return error;
#ifdef _WIN32
    if (!MoveFileExA(stage->name, dest, MOVEFILE_WRITE_THROUGH)) return win_error(GetLastError());
#else
    if (link(stage->name, dest)) return dbce_io_error();
    if (unlink(stage->name)) return dbce_io_error();
#endif
    free(stage->name);
    stage->name = NULL;
#ifndef _WIN32
    return sync_parent(dest);
#else
    return 0;
#endif
}

int dbce_stage_finish(stage_t* stage, int error) {
    if (stage->fp && fclose(stage->fp) && !error) error = dbce_io_error();
    if (stage->name && remove(stage->name) && !error) error = dbce_io_error();
    free(stage->name);
    *stage = (stage_t){0};
    return error;
}
