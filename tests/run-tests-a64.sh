#!/usr/bin/env bash
set -euo pipefail

mkdir -p run
truncate -s 64M run/blk0.img
rm -f \
  run/cond-a64.out \
  run/axi-a64-backend.log \
  run/axi-console-a64-backend.log \
  run/axi-a64-guest.log

guest_pid=

cleanup() {
  if [ -n "${guest_pid:-}" ] && kill -0 "$guest_pid" 2>/dev/null; then
    kill "$guest_pid" 2>/dev/null
    wait "$guest_pid" 2>/dev/null
  fi
}

interrupt() {
  trap - EXIT
  echo "interrupted, stopping ARM64 smoke test" >&2
  cleanup
  exit 130
}

trap cleanup EXIT
trap interrupt INT TERM

set +e
(
  sleep 8
  printf '\nwhile [ ! -b /dev/vda ]; do sleep 1; done\n'
  printf 'ls -l /dev/vda\n'
  printf 'dd if=/dev/random of=/dev/vda bs=512 count=1\n'
  printf 'for i in $(seq 1 10); do [ -e /dev/hvc0 ] && break; sleep 1; done\n'
  printf 'ls -l /dev/hvc0\n'
  printf 'printf "cond-a64-smoke-test\\n" > /dev/hvc0\n'
  printf 'sync\n'
) | timeout 45 nix run .#runvm-a64 -- samples/axi-a64.toml \
  > run/axi-a64-guest.log 2>&1 &
guest_pid=$!
wait "$guest_pid"
guest_status=$?
guest_pid=
set -e

if [ "$guest_status" != 0 ] && [ "$guest_status" != 124 ]; then
  echo "guest VM exited with status $guest_status" >&2
  echo "guest log: run/axi-a64-guest.log" >&2
  exit "$guest_status"
fi

grep -q 'virtio_blk virtio[0-9]: \[vda\]' run/axi-a64-guest.log
grep -q '/dev/vda' run/axi-a64-guest.log
grep -q '1+0 records out' run/axi-a64-guest.log
grep -q 'request type=1' run/axi-a64-backend.log
grep -q '/dev/hvc0' run/axi-a64-guest.log
grep -q 'cond-a64-smoke-test' run/cond-a64.out

echo "axi ARM64 backend/frontend smoke test passed"
echo "backend log: run/axi-a64-backend.log"
echo "console backend log: run/axi-console-a64-backend.log"
echo "console output: run/cond-a64.out"
echo "guest log:   run/axi-a64-guest.log"
