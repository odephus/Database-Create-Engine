# DBCE database create engine

## Build and test

On Windows, put CMake (3.20 or later), MinGW-w64 GCC with POSIX threads, and
mingw32-make on PATH. From the feature worktree run these PowerShell commands.

```powershell
cmake -S . -B build -G "MinGW Makefiles" -DCMAKE_C_COMPILER=gcc
cmake --build build
ctest --test-dir build --output-on-failure
```

On Linux use `cmake -S . -B build`, followed by the same build and test commands.
The C tests create private temporary files. CLI tests create their own temporary
directory under the build directory. For sanitizer runs use separate build
directories, passing `-DCMAKE_C_FLAGS="-g -O1 -fno-omit-frame-pointer -fsanitize=address,undefined -fno-sanitize-recover=all"`
or `-DCMAKE_C_FLAGS="-g -O1 -fno-omit-frame-pointer -fsanitize=thread"` at configure time.

## Write and synchronization contract

- A handle supports multiple producer and reader threads. Use just one open
  handle per database file across all processes. External writers, concurrent
  opens and external file changes are unsupported.
- Keys are C strings of 1–63 bytes. Values are binary, with length from 1
  through the smaller of UINT32_MAX and SIZE_MAX minus one. Get allocates a
  caller-owned buffer with a convenience trailing NUL outside its reported
  length. Zero value length on disk means deletion. Delete preserves the
  legacy idempotent tombstone behavior, including for a previously absent key.
- `dbce_set_async` copies the input and acknowledges admission to the bounded
  FIFO. A full queue reports EAGAIN. Synchronous set and delete wait for their
  own sequence and report its result. Success means the complete record was
  written, fflush succeeded, and the index entry was published. Data can still
  be in OS or device caches. Phase 1 does not issue fsync or FlushFileBuffers
  and makes no power-loss durability guarantee.
- `dbce_sync_wait_checked` captures the last submitted sequence under the queue
  mutex. It waits through that sequence and reports the **first** accepted
  failure in that prefix since open. The error is sticky for subsequent
  barriers. A later successful synchronous update still returns its own result.
  Use this checked API after async submissions. The legacy void wrapper waits
  for the same prefix.
- Failed bool and pointer APIs set errno in the calling thread. Typical errors
  include EINVAL (empty input), ENAMETOOLONG, EOVERFLOW, ENOMEM, ENOENT
  (missing key), ENOSPC (index full or storage full), and EILSEQ (malformed
  file). A seek, write or flush failure during append seals that handle against
  further writes. Already accepted writes receive the failure, and reads of
  prior committed records remain available if the storage is readable.
- The queue mutex protects admission, shutdown and sequence completion. A
  separate storage mutex protects both the index and every shared stream
  operation. The engine never holds both mutexes simultaneously. A single FIFO
  worker publishes completion under the queue mutex after releasing storage.
  Scans wait for prior submissions, copy the committed index under the storage
  lock and read from those immutable log offsets. Callbacks run with both locks
  released. A checked scan reports its own read or allocation error, and earlier
  callbacks may already have run. Use the checked barrier for write errors.
- Before close, stop all other API calls and wait for active calls and scan
  callbacks to return. Close stops admission, drains accepted work, joins the
  worker and releases the handle. `dbce_close_checked` reports prior accepted
  write failures and file-close errors. The original void close remains
  available. Callbacks may read or write but cannot close the active handle.

## Legacy log and recovery

The native-endian, packed legacy header is four uint32_t fields in order:
magic, crc32, key length, value length. Raw key bytes exclude the terminating
NUL. The value follows immediately. New records keep crc32 zero. The reserved
field is ignored on read and provides no checksum. This preserves valid legacy
files on compatible architectures without conversion. Format versioning and
checksum enforcement are deferred.

Open checks the file size, each header and the complete key and value extents
before admitting a record. An incomplete final record, embedded NUL in a key,
invalid magic or impossible length makes open fail. An index overflow also
fails open. All 4096 slots track distinct keys for the life of the log,
including tombstones. Existing keys can still be updated at capacity.

A rejected file is left byte-for-byte intact. Preserve a copy for manual
recovery or restore a known-good backup. A failed or interrupted write may
leave a partial tail, or a structurally complete record that is visible on
reopen despite an unsuccessful acknowledgment. The legacy format cannot
detect same-length value corruption, a missing whole record or an intact but
unacknowledged final record.

## CLI

```powershell
.\build\dbce.exe create db users
.\build\dbce.exe db list
.\build\dbce.exe add users id:1 '{"name":"Alper"}'
.\build\dbce.exe get users id:1
.\build\dbce.exe get users table
.\build\dbce.exe del users id:1
.\build\dbce.exe go users
```

The REPL accepts `add`, `get`, `get table`, `del`, `help`, `cls` and `exit`.
Configuration examples are in `config.json`. The legacy line-based reader
accepts db_path and queue_size. The reserved sync_interval_ms setting does
not establish a durability interval.
