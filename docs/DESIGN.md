# MiniKV Design Document

## 1. Requirements

Functional: put / get / delete binary keys and values; persist across restarts;
list keys; report statistics; reclaim space.

Non-functional: never return silently corrupted data; recover automatically
from a crash at any point; O(1) average lookup; no memory leaks or undefined
behaviour (verified with sanitizers); portable C11 + POSIX.

## 2. Architecture

```
          CLI (main.c)  /  library users
                   |
             kv.h public API
                   |
   +---------------+----------------+
   |                                |
 In-memory index               Append-only log file
 key -> (offset, vlen)         [rec][rec][rec]...
 hash table, chaining          each record has a CRC-32
```

- **Writes** append one record to the end of the log, then update the index.
- **Reads** look up the offset in the index, `pread()` the record, verify its CRC, return the value.
- **Startup** replays the log from the beginning to rebuild the index.
- **Delete** appends a tombstone record (so the delete itself survives restart).

## 3. On-disk record format (little endian)

```
 0        4        8        12  13
 +--------+--------+--------+---+----------+-----------+
 | crc32  | klen   | vlen   |flg|  key     |  value    |
 +--------+--------+--------+---+----------+-----------+
 crc32 covers bytes [4 .. end of record)
 flg: 0 = PUT, 1 = DELETE (vlen must be 0)
```

Integers are serialised byte by byte, so the file format is independent of the
host's endianness and struct padding.

## 4. Key algorithms

**Recovery** (`recover()`): for each record starting at offset 0, validate in
order: header fits in file → lengths within limits → flag valid → whole record
fits in file → CRC matches. On the first failure, stop, `ftruncate()` the file
to the last good offset and `fsync()`. Records replayed before that are exactly
the writes that fully reached disk.

**Hash index**: FNV-1a hash, separate chaining, table doubles when load factor
exceeds 0.75. Each node stores the key, record offset, and value length. The
index tracks `live_bytes` incrementally so statistics are O(1).

**Compaction** (`kv_compact()`): write every live record to `<db>.compact`
(re-verifying each CRC while copying, so corruption is never laundered into a
fresh file), `fsync()` it, then `rename()` over the original. `rename()` is
atomic on POSIX, so a crash leaves either the complete old file or the complete
new file. Index offsets are updated only after the rename succeeds; on any
error the original file and index are untouched.

**Failed append**: if a `pwrite` fails part-way (e.g. disk full), the file is
truncated back to the previous end so no half record is left in the middle of
the log.

## 5. Failure analysis

| Failure | Behaviour |
|---|---|
| Crash during append | Torn tail dropped at next open; all earlier data intact |
| Crash during compaction | Either old or new file survives whole; a stale `.compact` file is overwritten next time |
| Bit flip in an old record | Detected by CRC on `get` (`KV_ERR_CORRUPT`) and at next open |
| Second process opens same DB | `KV_ERR_LOCKED`, no data touched |
| Oversized / empty key or value | `KV_ERR_INVALID`, nothing written |
| `malloc` failure | `KV_ERR_NOMEM` propagated to caller |

## 6. Design decisions and alternatives considered

- **Log + hash index vs B-tree**: simpler, faster writes (sequential I/O), and the
  recovery logic is small enough to test exhaustively. Cost: keys must fit in RAM.
- **CRC-32 vs cryptographic hash**: the goal is detecting accidental corruption,
  not tampering; CRC-32 is fast and sufficient.
- **`flock` vs lock file**: the lock is released automatically if the process dies,
  so a crash never leaves a stale lock.
- **Stop-at-first-bad-record vs skip-and-continue**: skipping would risk resurrecting
  deleted keys or applying updates out of order.

## 7. Possible extensions

Hint/index file for faster startup, background compaction, range scans with a
sorted index, per-record timestamps / TTL, multi-threaded readers with an RW lock.
