#!/usr/bin/env bash
set -euo pipefail

bench_size_mb=${BENCH_SIZE_MB:-1}
bench_bs=${BENCH_BS:-64K}
guest_timeout=${BENCH_GUEST_TIMEOUT:-120}
tmp_root=${AXI_TEST_TMPDIR:-${TMPDIR:-/tmp}}
if [ ! -d "$tmp_root" ] || [ ! -w "$tmp_root" ]; then
  tmp_root=/tmp
fi

run_dir=$(mktemp -d "$tmp_root/uio-a64-bench.XXXXXX")

cleanup() {
  :
}

interrupt() {
  trap - EXIT
  printf '\033[2K\rinterrupted, stopping ARM64 UIO benchmark\n' >&2
  cleanup
  exit 130
}

trap cleanup EXIT
trap interrupt INT TERM

nix run .#runuio-a64 -- \
  --mode benchmark \
  --timeout "$guest_timeout" \
  --bench-size-mb "$bench_size_mb" \
  --bench-bs "$bench_bs" \
  --run-dir "$run_dir"
