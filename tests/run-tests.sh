#!/usr/bin/env bash
set -euo pipefail

mkdir -p run
truncate -s 64M run/blk0.img
rm -f run/cond.out

if [ ! -x out/blkd ] || [ ! -x out/cond ] || [ ! -x out/qemu-launch ]; then
  scripts/build-tools.sh
fi

timeout 40 "${BACKEND_BIN:-out/blkd}" configs/backends/axi-bus.toml \
  > run/axi-bus-backend.log 2>&1 &
backend_pid=$!

timeout 40 "${CONSOLE_BACKEND_BIN:-out/cond}" configs/backends/axi-console.toml \
  > run/axi-console-backend.log 2>&1 &
console_backend_pid=$!

cleanup() {
  kill "$backend_pid" 2>/dev/null || true
  kill "$console_backend_pid" 2>/dev/null || true
  wait "$backend_pid" 2>/dev/null || true
  wait "$console_backend_pid" 2>/dev/null || true
}
trap cleanup EXIT

for _ in $(seq 1 50); do
  test -S run/axi-bus.sock && test -S run/axi-console.sock && break
  sleep 0.2
done

if [ ! -S run/axi-bus.sock ] || [ ! -S run/axi-console.sock ]; then
  echo "backend sockets were not created" >&2
  echo "block backend log:   run/axi-bus-backend.log" >&2
  echo "console backend log: run/axi-console-backend.log" >&2
  exit 1
fi

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
) | timeout 25 nix run .#runvm -- configs/qemu-vms/axi-bus.toml \
  > run/axi-bus-guest.log 2>&1
guest_status=$?
set -e

if [ "$guest_status" != 0 ] && [ "$guest_status" != 124 ]; then
  echo "guest VM exited with status $guest_status" >&2
  echo "guest log: run/axi-bus-guest.log" >&2
  exit "$guest_status"
fi

grep -q 'virtio_blk virtio0: \[vda\]' run/axi-bus-guest.log
grep -q '/dev/vda' run/axi-bus-guest.log
grep -q '1+0 records out' run/axi-bus-guest.log
grep -q 'request type=1' run/axi-bus-backend.log
grep -q '/dev/hvc0' run/axi-bus-guest.log
grep -q 'cond-smoke-test' run/cond.out

echo "axi-bus backend/frontend smoke test passed"
echo "backend log: run/axi-bus-backend.log"
echo "console backend log: run/axi-console-backend.log"
echo "console output: run/cond.out"
echo "guest log:   run/axi-bus-guest.log"
