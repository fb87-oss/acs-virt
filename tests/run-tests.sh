#!/usr/bin/env bash
set -euo pipefail

mkdir -p run
truncate -s 64M run/blk0.img
rm -f run/cond.out run/virt-axi-backend.log run/virt-axi-console-backend.log run/virt-axi-guest.log

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
) | timeout 25 nix run .#runvm-x64 -- samples/virt-axi-x64.toml \
  > run/virt-axi-guest.log 2>&1
guest_status=$?
set -e

if [ "$guest_status" != 0 ] && [ "$guest_status" != 124 ]; then
  echo "guest VM exited with status $guest_status" >&2
  echo "guest log: run/virt-axi-guest.log" >&2
  exit "$guest_status"
fi

grep -q 'virtio_blk virtio0: \[vda\]' run/virt-axi-guest.log
grep -q '/dev/vda' run/virt-axi-guest.log
grep -q '1+0 records out' run/virt-axi-guest.log
grep -q 'request type=1' run/virt-axi-backend.log
grep -q '/dev/hvc0' run/virt-axi-guest.log
grep -q 'cond-smoke-test' run/cond.out

echo "virt-axi backend/frontend smoke test passed"
echo "backend log: run/virt-axi-backend.log"
echo "console backend log: run/virt-axi-console-backend.log"
echo "console output: run/cond.out"
echo "guest log:   run/virt-axi-guest.log"
