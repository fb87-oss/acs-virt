#!/usr/bin/env bash
set -euo pipefail

root_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="${BUILD_DIR:-$root_dir/build/cmake}"

if [[ -z "${IN_NIX_SHELL:-}" && -z "${C_UNIT_TESTS_NIX_SHELL:-}" ]]; then
  export C_UNIT_TESTS_NIX_SHELL=1
  exec nix shell nixpkgs#cmake nixpkgs#gcc nixpkgs#git nixpkgs#ninja --command bash "$0" "$@"
fi

mkdir -p "$root_dir/run"
cmake -S "$root_dir" -B "$build_dir" -G Ninja -DCHIPLETS_FETCH_QEMU=OFF
cmake --build "$build_dir" --target c-backend-tests --parallel
exec "$root_dir/build/out/c-backend-tests" "$@"
