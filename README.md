# MiniKV: A Crash-Safe Persistent Key-Value Store in C

MiniKV is a small embedded database library plus command-line tool, written in
standard C11 (POSIX file APIs). It stores arbitrary binary keys and values on
disk, survives power loss and torn writes, detects data corruption with
checksums, and reclaims space through log compaction.

It follows the same core design as production storage engines (Bitcask,
write-ahead logs in LevelDB/SQLite): **an append-only log as the source of
truth, plus an in-memory hash index rebuilt on startup.**

## The engineering problem

Naive file-based storage (rewrite a file on every update) has three failure
modes: a crash mid-write corrupts the whole file, silent disk corruption goes
unnoticed, and lookups require scanning. MiniKV addresses each:

| Problem | Solution |
|---|---|
| Crash / power loss mid-write | Append-only log; on open, replay and truncate the torn tail |
| Silent bit rot | CRC-32 on every record, verified on recovery **and** on every read |
| Slow lookup | In-memory hash table (FNV-1a, chaining, auto-resize) → O(1) get |
| Unbounded file growth | Compaction rewrites only live records, swapped in atomically with `rename()` |
| Two processes corrupting one file | Exclusive `flock()` on the database file |

## Build and run

Requires `gcc` (or any C11 compiler) and `make` on Linux/macOS.

```
make            # builds ./minikv
make test       # unit, recovery and randomised tests
make sanitize   # same tests under AddressSanitizer + UBSan
```

```
$ ./minikv mydata.db put city Nagpur
$ ./minikv mydata.db get city
Nagpur
$ ./minikv mydata.db del city
$ ./minikv mydata.db list
$ ./minikv mydata.db stats
$ ./minikv mydata.db compact
$ ./minikv mydata.db bench 100000
$ ./minikv mydata.db shell          # interactive prompt
$ ./minikv --sync mydata.db put k v # fsync after every write
```

Exit codes: `0` success, `1` key not found, `2` error.

## Using it as a library

```c
#include "kv.h"

kv_t *db;
kv_open("data.db", /*sync=*/1, &db);
kv_put(db, "k", 1, "value", 5);

void *v; size_t n;
if (kv_get(db, "k", 1, &v, &n) == KV_OK) { /* use v[0..n) */ free(v); }

kv_del(db, "k", 1);
kv_compact(db);
kv_close(db);
```

## Project layout

```
src/kv.h, kv.c      storage engine (public API in kv.h)
src/crc32.h, .c     checksum
src/main.c          command-line interface
tests/test_kv.c     test suite (13 test groups, 25,000+ checks)
docs/DESIGN.md      architecture, file format, failure analysis
docs/TESTING.md     test plan and results
Makefile
```

## Limits and honest trade-offs

- All keys live in memory (values do not), so RAM use is proportional to key count.
- Single writer, single thread. The API is not thread-safe.
- Recovery stops at the first bad record; anything after it is discarded.
  This is the standard write-ahead-log policy: later records can't be trusted
  once an earlier one is damaged.
- Without `--sync`, a crash can lose the most recent writes still in the OS cache
  (but never corrupts earlier data). With `--sync`, every acknowledged write is on disk.
- Max key 1 KiB, max value 1 MiB.
