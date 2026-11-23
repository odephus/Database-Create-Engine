# Storage benchmark

Median of three sequential runs per configuration on a Ryzen 9 8945H, x86-64 Arch Linux under WSL2 (kernel 6.18.58.1), ext4 on the guest virtual disk, GCC 16.2.1, `-O3`, without sanitizers. Each run used a fresh private directory under `/var/tmp`, one producer and one writer thread. The baseline compiled the same harness against Phase 1 commit `99c7cf8`.

The workload inserts 1024 distinct seven-byte keys with 256-byte values, performs 8192 fixed-seed random reads, replaces all 1024 values, opens and closes the resulting 2048-record log 16 times, then deletes all keys. Reads check the returned bytes. Reopens use the warm OS cache. Initial file creation and fixture cleanup are outside the timers.

| Operations per second | Phase 1 V1 | Current V1 | V2 | V2, durable per write |
| --- | ---: | ---: | ---: | ---: |
| Sequential insert | 17,947 | 17,448 | 17,290 | 882 |
| Random read | 487,506 | 492,396 | 493,172 | 478,416 |
| Update | 17,693 | 16,913 | 16,901 | 879 |
| Delete | 18,145 | 18,433 | 17,530 | 902 |
| Open, replay and close | 208.8 | 194.6 | 138.3 | 143.4 |

The first three columns acknowledge each complete write after stream flush. The final column **also waits for a successful storage sync after each mutation**. A batch can amortize this cost by calling the durable barrier once after several operations.

V2 replay took 7.23 ms per open versus 5.14 ms for current V1. That 2.09 ms difference includes reading values and verifying both checksums. V1 can seek past values. In a separate in-memory measurement, the V2 CRC routine processed a median 1,290,909 blocks of 256 bytes per second (315 MiB/s). These are short, warm-cache measurements on a virtual disk; latency on other storage can differ substantially.

## Reproduce on Linux

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target dbce_bench
./build/dbce_bench v1
./build/dbce_bench v2
./build/dbce_bench durable
```

For the historical column, export `dbce.c` and `dbce.h` from the baseline commit to a temporary directory. Set `phase1` to that directory, then compile from the V2 tree:

```sh
cc -O3 -std=c11 -pthread -DDBCE_PHASE1 -I"$phase1" bench_dbce.c "$phase1/dbce.c" -o "$phase1/bench"
"$phase1/bench" v1
```
