# DBCE storage format

Offsets and lengths below count bytes. There is no alignment or padding between records. V2 integers are unsigned and little-endian. Readers must validate a complete record before publishing its key.

## File header

| Offset | Size | Required bytes or value |
| ---: | ---: | --- |
| 0 | 8 | Signature `89 44 42 32 0D 0A 1A 0A` (hex) |
| 8 | 2 | Version 2 |
| 10 | 2 | Header size 16 |
| 12 | 4 | Reserved, zero |

A complete header alone is an empty V2 database. A partial file header cannot be recovered. A supported signature with an unknown version returns `ENOTSUP`. A wrong size or nonzero reserved field returns `EILSEQ`.

## Record

| Offset | Size | Meaning |
| ---: | ---: | --- |
| 0 | 4 | ASCII `DR2!` |
| 4 | 1 | Operation 1 (SET) or 2 (DELETE) |
| 5 | 3 | Reserved, zero |
| 8 | 4 | Key length K, 1–63 |
| 12 | 4 | Value length V, 0–4,294,967,295. DELETE requires zero. |
| 16 | 4 | Record CRC |
| 20 | 4 | Header CRC |
| 24 | K | Raw key, with no embedded or trailing NUL |
| 24 + K | V | Raw value, including any NUL bytes |

SET with V=0 creates a present key with an empty value. DELETE creates a tombstone. Both consume an index slot for a distinct key. Replay applies records in order. Updating an existing key works at the 4096-key limit. Implementations whose allocation size cannot represent V+1 reject that record with `EOVERFLOW`.

Both checksums use **CRC-32/ISO-HDLC** with polynomial 0x04C11DB7 (reflected 0xEDB88320), initial register 0xFFFFFFFF, reflected input and output, and final XOR 0xFFFFFFFF. The empty sequence yields zero. ASCII `123456789` yields 0xCBF43926.

- **Record CRC** covers bytes 0–15 followed by the K key bytes and the V value bytes, in that order.
- **Header CRC** covers bytes 0–19, including the serialized record CRC. The reader checks this before trusting lengths in a complete header.

For a SET of key `a` and value `00 FF 10`, the entire record is:

```text
44 52 32 21  01 00 00 00  01 00 00 00  03 00 00 00
6E D0 41 37  55 C3 E7 3D  61 00 FF 10
```

## Detection and compatibility

A missing pathname is initialized as V2 by publishing a complete header. An existing zero-byte file is the valid empty V1 database. V1 records use the native-endian 16-byte packed header (magic 0x45434244, unused CRC field, key length, value length). Their zero-length values are tombstones. The leading V2 signature differs from either byte order of the legacy magic. Invalid input fails validation. Opening a V1 file preserves its bytes and future writes keep the V1 layout until explicit migration. V1 data has no historical checksum to verify.

## Recovery decisions

Opening never repairs a log. Recovery inspects a locked V2 source from the beginning, replaying the same validation and index limits as open. The reported byte count is the file size minus the end of the last complete, verified record. A clean file reports zero. An incomplete final record is eligible only in these cases.

| Tail | Evidence required |
| --- | --- |
| Header | Fewer than 24 bytes remain. All available magic, operation, reserved, length and header-CRC bytes are consistent with a legal prefix. |
| Key | Header validates. Fewer than K key bytes remain. Available key bytes contain no NUL. |
| Value | Header and key validate. Fewer than V value bytes remain. |

Checksum failures return EBADMSG. The recovery report identifies the failed field and the offset following the verified prefix. A complete record with a bad checksum, an invalid header or an unsupported operation fails, including at EOF. Recovery never searches forward for another plausible record. The contents of an incomplete final record cannot be verified. A short suffix can coincide with a plausible partial header by chance. A missing whole record leaves no evidence in this format. CRC32 does not authenticate deliberate edits, establish order between records or prove a write was acknowledged before a crash.

`recover SOURCE` only inspects. `recover SOURCE OUTPUT` creates a verified prefix and a byte-for-byte backup at `OUTPUT.bak`. It compares both staged copies against the locked source, validates the prefix as a database, then publishes the backup before publishing the result. The original path is never truncated or replaced. Both destination names must be unused. The two publications are separate operations; if the second fails, the completed backup may remain.

## Publication and crash boundaries

Migration locks the existing V1 source, validates its structure and capacity, converts every historical record (including tombstones) into a temporary V2 file beside the destination, then replays the result. During verification it compares each operation, key and value against the source in order and checks that both streams end together. An existing destination, including an alias to the source, cannot be replaced. Handled failures clean up private temporary files. A killed process can leave an unpublished temporary file; its name does not confer ownership or lock the database.

On Linux, an exclusive nonblocking `flock` protects each open file description. Publication syncs the staged file, links it at the new name, unlinks the temporary name and syncs the parent directory. This requires same-filesystem hard-link support. A crash between link and unlink can leave both names for the same file. Directory-sync failure is reported even if the complete destination is already visible.

On Windows, `LockFileEx` exclusively locks byte zero for the lifetime of the handle. Opening a second handle to the same file through DBCE fails even within the same process. Publication calls `FlushFileBuffers` and uses same-directory `MoveFileExA` with `MOVEFILE_WRITE_THROUGH` and replacement disabled. Windows has no equivalent directory-sync guarantee in this implementation. File names follow the existing narrow Windows API. [Windows locking](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-lockfileex) · [Windows move semantics](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-movefileexa)

Both platforms release file locks on close or process exit. Use a trusted local directory with stable pathnames and a filesystem supporting the specified operations. Sharing a single open handle among threads remains supported. Externally renaming, unlinking or modifying an open file, mixing old writers, hostile directory changes and network filesystems are outside the locking contract.

Async acceptance copies a task into the queue. Synchronous completion follows a complete, stream-flushed append and index publication. A checked barrier waits through its captured sequence. `dbce_sync_durable` then synchronizes the open file under the storage mutex. A later successful write can be included in the same disk flush. Durable-sync failures remain reportable on subsequent durable barriers and checked close. Acknowledgment is limited by OS, filesystem and hardware behavior. There is no transaction spanning multiple writes.
