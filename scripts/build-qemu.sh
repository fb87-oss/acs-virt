#!/usr/bin/env bash
set -euo pipefail

root_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
target="${1:-x64}"

case "$target" in
  x64)
    cmake_build_dir="${CMAKE_BUILD_DIR:-$root_dir/build/cmake-qemu-x64}"
    target_file="$root_dir/cmake/qemu-targets/x64-minimal.cmake"
    cmake_target="qemu-x64-minimal"
    binary="out/qemu-x64-minimal/bin/qemu-system-x86_64"
    ;;
  a64|arm64)
    cmake_build_dir="${CMAKE_BUILD_DIR:-$root_dir/build/cmake-qemu-a64}"
    target_file="$root_dir/cmake/qemu-targets/arm64-default.cmake"
    cmake_target="qemu-arm64-default"
    binary="out/qemu-arm64-default/bin/qemu-system-aarch64"
    ;;
  *)
    echo "usage: $0 [x64|a64]" >&2
    exit 2
    ;;
esac

if [[ -z "${IN_NIX_SHELL:-}" && -z "${CHIPLETS_QEMU_NIX_SHELL:-}" ]]; then
  export CHIPLETS_QEMU_NIX_SHELL=1
  exec nix develop --impure --expr '
    let pkgs = import <nixpkgs> {};
    in pkgs.mkShell {
      nativeBuildInputs = with pkgs; [ bash ccache cmake gcc git meson ninja perl pkg-config python3 rsync ];
      buildInputs = with pkgs; [ glib pixman zlib ];
    }
  ' --command bash "$0" "$@"
fi

cmake -S "$root_dir" -B "$cmake_build_dir" -G Ninja \
  -DCHIPLETS_FETCH_QEMU=ON \
  -DCHIPLETS_QEMU_TARGET_FILE="$target_file"
cmake --build "$cmake_build_dir" --target "$cmake_target" --parallel

printf 'Built QEMU %s target: %s/%s\n' "$target" "$root_dir" "$binary"
