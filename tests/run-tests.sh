#!/usr/bin/env bash
set -euo pipefail

guest_timeout=25

tmp_root=${AXI_TEST_TMPDIR:-${TMPDIR:-/tmp}}
if [ ! -d "$tmp_root" ] || [ ! -w "$tmp_root" ]; then
  tmp_root=/tmp
fi
run_dir=$(mktemp -d "$tmp_root/axi-x64-smoke.XXXXXX")
config=$run_dir/axi-x64.toml
disk_image=$run_dir/blk0.img
backend_log=$run_dir/axi-backend.log
console_backend_log=$run_dir/axi-console-backend.log
console_output=$run_dir/cond.out
guest_log=$run_dir/axi-guest.log
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
  printf '\033[2K\rinterrupted, stopping test\n' >&2
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
  printf 'ls -l /dev/vda\n'
  printf 'dd if=/dev/random of=/dev/vda bs=512 count=1\n'
  printf 'for i in $(seq 1 10); do [ -e /dev/hvc0 ] && break; sleep 1; done\n'
  printf 'ls -l /dev/hvc0\n'
  printf 'printf "cond-smoke-test\\n" > /dev/hvc0\n'
  printf 'sync\n'
  printf 'echo AXI_SMOKE_TEST_DONE\n'
) | timeout "$guest_timeout" nix run .#runvm-x64 -- "$config" \
  > "$guest_log" 2>&1 &
guest_pid=$!
countdown "$guest_timeout" "waiting for smoke test" &
countdown_pid=$!
wait "$guest_pid"
guest_status=$?
guest_pid=
kill "$countdown_pid" 2>/dev/null
wait "$countdown_pid" 2>/dev/null
countdown_pid=
printf '\033[2K\rwaiting for smoke test: done\n' >&2
set -e

if [ "$guest_status" != 0 ] && [ "$guest_status" != 124 ]; then
  echo "guest VM exited with status $guest_status" >&2
  echo "guest log: $guest_log" >&2
  exit "$guest_status"
fi

grep -q 'AXI_SMOKE_TEST_DONE' "$guest_log" || fail "guest smoke commands did not complete"
grep -q 'virtio_blk virtio0: \[vda\]' "$guest_log" || fail "guest did not probe virtio-blk as /dev/vda"
grep -q '/dev/vda' "$guest_log" || fail "guest did not list /dev/vda"
grep -q '1+0 records out' "$guest_log" || fail "guest did not complete block write"
grep -q 'request type=1' "$backend_log" || fail "backend did not observe a write request"
grep -q '/dev/hvc0' "$guest_log" || fail "guest did not list /dev/hvc0"
grep -q 'cond-smoke-test' "$console_output" || fail "console backend did not receive smoke string"

echo "axi backend/frontend smoke test passed"
echo "run dir: $run_dir"
echo "backend log: $backend_log"
echo "console backend log: $console_backend_log"
echo "console output: $console_output"
echo "guest log:   $guest_log"
