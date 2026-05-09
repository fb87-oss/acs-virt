#!/usr/bin/env bash
set -euo pipefail

guest_timeout=${UIO_GUEST_TIMEOUT:-120}
tmp_root=${AXI_TEST_TMPDIR:-${TMPDIR:-/tmp}}
if [ ! -d "$tmp_root" ] || [ ! -w "$tmp_root" ]; then
  tmp_root=/tmp
fi

run_dir=$(mktemp -d "$tmp_root/uio-x64-backend-a64-frontend.XXXXXX")

cleanup() {
  :
}

interrupt() {
  trap - EXIT
  printf '\033[2K\rinterrupted, stopping mixed UIO test\n' >&2
  cleanup
  exit 130
}

trap cleanup EXIT
trap interrupt INT TERM

nix run .#runuio-x64-backend-a64-frontend -- \
  --mode smoke \
  --timeout "$guest_timeout" \
  --run-dir "$run_dir"
