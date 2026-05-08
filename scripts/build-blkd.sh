#!/usr/bin/env bash
set -euo pipefail

root_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="${BUILD_DIR:-$root_dir/build/cmake}"

if [[ -z "${IN_NIX_SHELL:-}" && -z "${BLKD_NIX_SHELL:-}" ]]; then
  export BLKD_NIX_SHELL=1
  exec nix shell nixpkgs#cmake nixpkgs#gcc nixpkgs#git nixpkgs#ninja --command bash "$0" "$@"
fi

cmake -S "$root_dir" -B "$build_dir" -G Ninja -DCHIPLETS_FETCH_QEMU=OFF
exec cmake --build "$build_dir" --target blkd --parallel
