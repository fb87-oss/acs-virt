# run-benchmark.sh

`tests/run-benchmark.sh` runs a simple end-to-end throughput benchmark for the
current `axi-bus` backend/frontend path.

The script starts the block backend and the `cond` console backend, boots the
QEMU microvm through `nix run .#runvm`, waits for `/dev/vda` in the guest, then
runs `dd` write and read tests against the virtio block device.

## Defaults

```text
BENCH_SIZE_MB=16
BENCH_BS=64K
BENCH_GUEST_TIMEOUT=180
BENCH_BACKEND_TIMEOUT=220
```

The default benchmark writes and reads 16 MiB using 64 KiB `dd` blocks.

## Usage

```sh
tests/run-benchmark.sh
```

Custom size and block size:

```sh
BENCH_SIZE_MB=32 BENCH_BS=128K tests/run-benchmark.sh
```

Run with explicit backend overrides:

```sh
scripts/build-tools.sh
BACKEND_BIN=out/blkd tests/run-benchmark.sh
```

The console backend is always started because the VM config includes a second
`axi-bus` MMIO window for virtio-console. Override it with:

```sh
CONSOLE_BACKEND_BIN=out/cond tests/run-benchmark.sh
```

## Runtime Files

The script creates or overwrites:

```text
run/blk0.img
run/axi-bus-bench-backend.log
run/axi-console-bench-backend.log
run/axi-bus-bench-guest.log
```

`run/blk0.img` is truncated to 64 MiB before each run.

## Guest Workload

The guest commands are injected through QEMU serial input:

```sh
while [ ! -b /dev/vda ]; do sleep 1; done
dd if=/dev/zero of=/dev/vda bs=<BENCH_BS> count=<count>
sync
dd if=/dev/vda of=/dev/null bs=<BENCH_BS> count=<count>
```

The benchmark uses `/dev/zero` for writes to measure the backend/frontend block
path without spending guest CPU time generating random data.

## Output

The script prints the parsed `dd` throughput lines, for example:

```text
axi-bus dd benchmark complete
config: size=16MiB bs=64K count=256
write:  16777216 bytes (16.0MB) copied, 1.076516 seconds, 14.9MB/s
read:   16777216 bytes (16.0MB) copied, 0.066046 seconds, 242.3MB/s
```

The backend currently uses `qemu-mediated` DMA, so the numbers primarily measure
the debug transport path and should not be treated as the expected shared-memory
fast-path performance.
