#!/usr/bin/env bash
set -euo pipefail

root_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cmake_build_dir="${CMAKE_BUILD_DIR:-${BUILD_DIR:-$root_dir/build/cmake}}"

if [[ -z "${IN_NIX_SHELL:-}" && -z "${CHIPLETS_CMAKE_NIX_SHELL:-}" ]]; then
  export CHIPLETS_CMAKE_NIX_SHELL=1
  exec nix shell nixpkgs#cmake nixpkgs#gcc nixpkgs#git nixpkgs#ninja --command bash "$0" "$@"
fi

cmake -S "$root_dir" -B "$cmake_build_dir" -G Ninja -DCHIPLETS_FETCH_QEMU=OFF
cmake --build "$cmake_build_dir" --target virtio-blkd virtio-consoled c-backend-tests --parallel
