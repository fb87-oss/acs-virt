# run-benchmark.sh

`tests/run-benchmark.sh` runs a simple end-to-end throughput benchmark for the
x86_64 two-VM UIO backend/frontend path.

The script calls `nix run .#runuio-x64`, which builds backend daemons with
`CHIPLETS_BACKEND_FABRIC=linux-uio`, packages a temporary initrd with UIO support,
starts backend and frontend QEMU processes through `scripts/chiplets-uio-x64.py`,
waits for `/dev/vda` in the frontend guest, then runs `dd` write and read tests
against the virtio block device.

## Defaults

```text
BENCH_SIZE_MB=1
BENCH_BS=64K
BENCH_REPEAT=1
BENCH_GUEST_TIMEOUT=300
```

The default benchmark writes and reads 1 MiB using 64 KiB `dd` blocks. Larger
runs can be requested with `BENCH_SIZE_MB`.

## Usage

```sh
tests/run-benchmark.sh
```

Custom size and block size:

```sh
BENCH_SIZE_MB=32 BENCH_BS=128K tests/run-benchmark.sh
```

Repeated runs within the same VM pair:

```sh
BENCH_SIZE_MB=64 BENCH_REPEAT=3 tests/run-benchmark.sh
```

Optional backend profiling and direct read-DMA experiments:

```sh
CHIPLETS_PROFILE_BACKEND=1 BENCH_SIZE_MB=64 tests/run-benchmark.sh
CHIPLETS_DIRECT_READ_DMA=1 BENCH_SIZE_MB=64 tests/run-benchmark.sh
```

## Runtime Files

The script creates a temporary run directory under `${TMPDIR:-/tmp}` or
`AXI_TEST_TMPDIR` and passes it to the orchestrator. It contains:

```text
frontend.log
backend.log
frontend.ram
backend.ram
blk.mmio
blk.control.sock
```

The orchestrator removes the large RAM/MMIO artifacts when cleanup is enabled
and prints the run directory and log paths at the end.

## Guest Workload

The guest commands are injected through QEMU serial input:

```sh
while [ ! -b /dev/vda ]; do sleep 1; done
for run in $(seq 1 <BENCH_REPEAT>); do
  dd if=/dev/zero of=/dev/vda bs=<BENCH_BS> count=<count>
  sync
  dd if=/dev/vda of=/dev/null bs=<BENCH_BS> count=<count>
done
```

The benchmark uses `/dev/zero` for writes to measure the backend/frontend block
path without spending guest CPU time generating random data.

## Output

The script prints the parsed `dd` throughput lines and aggregate summaries when
`BENCH_REPEAT` is greater than one, for example:

```text
uio dd benchmark complete

benchmark summary
  config: size=1MiB bs=64K count=16 repeat=2
  write[1]: 1048576 bytes (1.0MB) copied, 0.160013 seconds, 6.2MB/s
  write[2]: 1048576 bytes (1.0MB) copied, 0.158653 seconds, 6.3MB/s
  read[1]:  1048576 bytes (1.0MB) copied, 0.249040 seconds, 4.0MB/s
  read[2]:  1048576 bytes (1.0MB) copied, 0.247073 seconds, 4.0MB/s
  write summary: min=6.2MiB/s avg=6.3MiB/s max=6.3MiB/s
  read summary:  min=4.0MiB/s avg=4.0MiB/s max=4.0MiB/s
  backend requests: read=7 write=4 flush=0
```

The backend request counts are parsed from `virtio-blkd` logs. When
`CHIPLETS_PROFILE_BACKEND=1` is set, the output also includes average backend
timing for descriptor-chain processing, guest DMA, image I/O, used-ring updates,
and IRQ signaling.
