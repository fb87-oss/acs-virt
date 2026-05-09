#!/usr/bin/env bash
set -euo pipefail

bench_size_mb=${BENCH_SIZE_MB:-1}
bs=${BENCH_BS:-64K}
repeat=${BENCH_REPEAT:-1}
guest_timeout=${BENCH_GUEST_TIMEOUT:-300}
tmp_root=${AXI_TEST_TMPDIR:-${TMPDIR:-/tmp}}
if [ ! -d "$tmp_root" ] || [ ! -w "$tmp_root" ]; then
  tmp_root=/tmp
fi

run_dir=$(mktemp -d "$tmp_root/uio-x64-bench.XXXXXX")

cleanup() {
  :
}

interrupt() {
  trap - EXIT
  printf '\033[2K\rinterrupted, stopping UIO benchmark\n' >&2
  cleanup
  exit 130
}

trap cleanup EXIT
trap interrupt INT TERM

nix run .#runuio-x64 -- \
  --mode benchmark \
  --timeout "$guest_timeout" \
  --run-dir "$run_dir" \
  --bench-size-mb "$bench_size_mb" \
  --bench-bs "$bs" \
  --bench-repeat "$repeat"
