#!/usr/bin/env bash
set -euo pipefail

bench_size_mb=${BENCH_SIZE_MB:-16}
bs=${BENCH_BS:-64K}
guest_timeout=${BENCH_GUEST_TIMEOUT:-180}
case "$bs" in
  *K) bs_bytes=$(( ${bs%K} * 1024 )) ;;
  *M) bs_bytes=$(( ${bs%M} * 1024 * 1024 )) ;;
  *[!0-9]*)
    echo "BENCH_BS must be a byte count or use K/M suffix, got: $bs" >&2
    exit 2
    ;;
  *) bs_bytes=$bs ;;
esac

if [ "$bs_bytes" -le 0 ]; then
  echo "BENCH_BS must be greater than zero" >&2
  exit 2
fi

bench_bytes=$(( bench_size_mb * 1024 * 1024 ))
count=$(( bench_bytes / bs_bytes ))
if [ "$count" -le 0 ]; then
  echo "BENCH_SIZE_MB is smaller than BENCH_BS" >&2
  exit 2
fi

mkdir -p run
truncate -s 64M run/blk0.img
rm -f run/cond.out run/axi-bus-backend.log run/axi-console-backend.log run/axi-bus-bench-guest.log

backend_log=run/axi-bus-backend.log
console_backend_log=run/axi-console-backend.log
guest_log=run/axi-bus-bench-guest.log

set +e
(
  sleep 5
  printf '\nwhile [ ! -b /dev/vda ]; do sleep 1; done\n'
  printf 'echo BENCH_CONFIG size_mb=%s bs=%s count=%s\n' "$bench_size_mb" "$bs" "$count"
  printf 'echo WRITE_BENCH_START\n'
  printf 'dd if=/dev/zero of=/dev/vda bs=%s count=%s 2>&1\n' "$bs" "$count"
  printf 'sync\n'
  printf 'echo WRITE_BENCH_END\n'
  printf 'echo READ_BENCH_START\n'
  printf 'dd if=/dev/vda of=/dev/null bs=%s count=%s 2>&1\n' "$bs" "$count"
  printf 'echo READ_BENCH_END\n'
) | timeout "$guest_timeout" nix run .#runvm -- configs/axi-bus.toml \
  > "$guest_log" 2>&1
guest_status=$?
set -e

if [ "$guest_status" != 0 ] && [ "$guest_status" != 124 ]; then
  echo "guest VM exited with status $guest_status" >&2
  echo "guest log: $guest_log" >&2
  exit "$guest_status"
fi

grep -q 'virtio_blk virtio0: \[vda\]' "$guest_log"
grep -q 'WRITE_BENCH_END' "$guest_log"
grep -q 'READ_BENCH_END' "$guest_log"

write_result=$(awk '
  /WRITE_BENCH_START/ { in_write = 1; next }
  /WRITE_BENCH_END/ { in_write = 0 }
  in_write && /copied/ { result = $0 }
  END { print result }
' "$guest_log")

read_result=$(awk '
  /READ_BENCH_START/ { in_read = 1; next }
  /READ_BENCH_END/ { in_read = 0 }
  in_read && /copied/ { result = $0 }
  END { print result }
' "$guest_log")

if [ -z "$write_result" ] || [ -z "$read_result" ]; then
  echo "failed to parse dd throughput from guest log" >&2
  echo "guest log: $guest_log" >&2
  exit 1
fi

echo "axi-bus dd benchmark complete"
echo "config: size=${bench_size_mb}MiB bs=$bs count=$count"
echo "write:  $write_result"
echo "read:   $read_result"
echo "backend log: $backend_log"
echo "console backend log: $console_backend_log"
echo "guest log:   $guest_log"
