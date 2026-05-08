#!/usr/bin/env bash
set -euo pipefail

bench_size_mb=${BENCH_SIZE_MB:-16}
bs=${BENCH_BS:-64K}
guest_timeout=${BENCH_GUEST_TIMEOUT:-60}
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

tmp_root=${AXI_TEST_TMPDIR:-${TMPDIR:-/tmp}}
if [ ! -d "$tmp_root" ] || [ ! -w "$tmp_root" ]; then
  tmp_root=/tmp
fi
run_dir=$(mktemp -d "$tmp_root/axi-x64-bench.XXXXXX")
config=$run_dir/axi-x64.toml
disk_image=$run_dir/blk0.img
backend_log=$run_dir/axi-backend.log
console_backend_log=$run_dir/axi-console-backend.log
console_output=$run_dir/cond.out
guest_log=$run_dir/axi-bench-guest.log
guest_pid=
countdown_pid=

cleanup() {
  if [ -n "${countdown_pid:-}" ] && kill -0 "$countdown_pid" 2>/dev/null; then
    kill "$countdown_pid" 2>/dev/null
    wait "$countdown_pid" 2>/dev/null
  fi
  if [ -n "${guest_pid:-}" ] && kill -0 "$guest_pid" 2>/dev/null; then
    kill "$guest_pid" 2>/dev/null
    wait "$guest_pid" 2>/dev/null
  fi
  rm -f "$disk_image"
}

interrupt() {
  trap - EXIT
  printf '\033[2K\rinterrupted, stopping benchmark\n' >&2
  cleanup
  exit 130
}

trap cleanup EXIT
trap interrupt INT TERM

truncate -s 64M "$disk_image"

cat > "$config" <<EOF
ram_access = "qemu-mediated"

[[devices]]
name = "blk0"
type = "virtio-blk"
mmio = { base = "0xfeb00000", size = "0x200", irq = 16 }

[[devices]]
name = "con0"
type = "virtio-console"
mmio = { base = "0xfeb00200", size = "0x200", irq = 17 }

[targets.qemu]
type = "microvm"
binary = "out/qemu-x64-minimal/bin/qemu-system-x86_64"
parameters = { memory = "512M", kvm = true, pcie = false }

[[targets.qemu.devices]]
name = "blk0"
socket = "$run_dir/axi.sock"
log = "$backend_log"
image = "$disk_image"
readonly = false

[[targets.qemu.devices]]
name = "con0"
socket = "$run_dir/axi-console.sock"
log = "$console_backend_log"
output = "$console_output"
EOF

fail() {
  echo "$1" >&2
  echo "run dir: $run_dir" >&2
  echo "backend log: $backend_log" >&2
  echo "console backend log: $console_backend_log" >&2
  echo "console output: $console_output" >&2
  echo "guest log:   $guest_log" >&2
  exit 1
}

countdown() {
  local timeout=$1
  local label=$2
  local remaining=$timeout

  while [ "$remaining" -gt 0 ]; do
    printf '\033[2K\r%s: %ss remaining' "$label" "$remaining" >&2
    sleep 1
    remaining=$((remaining - 1))
  done

  printf '\033[2K\r%s: timeout expired, waiting for command to exit' "$label" >&2
  while true; do
    sleep 1
  done
}

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
  printf 'echo AXI_BENCH_DONE\n'
) | timeout "$guest_timeout" nix run .#runvm-x64 -- "$config" \
  > "$guest_log" 2>&1 &
guest_pid=$!
countdown "$guest_timeout" "waiting for benchmark" &
countdown_pid=$!
wait "$guest_pid"
guest_status=$?
guest_pid=
kill "$countdown_pid" 2>/dev/null
wait "$countdown_pid" 2>/dev/null
countdown_pid=
printf '\033[2K\rwaiting for benchmark: done\n' >&2
set -e

if [ "$guest_status" != 0 ] && [ "$guest_status" != 124 ]; then
  echo "guest VM exited with status $guest_status" >&2
  echo "guest log: $guest_log" >&2
  exit "$guest_status"
fi

grep -q 'AXI_BENCH_DONE' "$guest_log" || fail "guest benchmark commands did not complete"
grep -q 'virtio_blk virtio0: \[vda\]' "$guest_log" || fail "guest did not probe virtio-blk as /dev/vda"
grep -q 'WRITE_BENCH_END' "$guest_log" || fail "guest write benchmark did not complete"
grep -q 'READ_BENCH_END' "$guest_log" || fail "guest read benchmark did not complete"

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
  fail "failed to parse dd throughput from guest log"
fi

read_requests=$(grep -c 'request type=0' "$backend_log" || true)
write_requests=$(grep -c 'request type=1' "$backend_log" || true)
flush_requests=$(grep -c 'request type=4' "$backend_log" || true)

echo "axi dd benchmark complete"
echo
echo "benchmark summary"
echo "  config: size=${bench_size_mb}MiB bs=$bs count=$count"
echo "  write:  $write_result"
echo "  read:   $read_result"
echo "  backend requests: read=$read_requests write=$write_requests flush=$flush_requests"
echo "  run dir: $run_dir"
echo "  backend log: $backend_log"
echo "  console backend log: $console_backend_log"
echo "  console output: $console_output"
echo "  guest log: $guest_log"
