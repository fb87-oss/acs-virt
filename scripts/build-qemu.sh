#!/usr/bin/env bash
set -euo pipefail

root_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cmake_build_dir="${CMAKE_BUILD_DIR:-$root_dir/build/cmake-qemu}"

if [[ -z "${IN_NIX_SHELL:-}" && -z "${CHIPLETS_QEMU_NIX_SHELL:-}" ]]; then
  export CHIPLETS_QEMU_NIX_SHELL=1
  exec nix --extra-experimental-features nix-command develop --impure --expr '
    let pkgs = import <nixpkgs> {};
    in pkgs.mkShell {
      nativeBuildInputs = with pkgs; [ bash ccache cmake gcc git meson ninja perl pkg-config python3 rsync ];
      buildInputs = with pkgs; [ glib pixman zlib ];
    }
  ' --command bash "$0" "$@"
fi

cmake -S "$root_dir" -B "$cmake_build_dir" -G Ninja \
  -DCHIPLETS_FETCH_QEMU=ON
cmake --build "$cmake_build_dir" --target qemu --parallel

for bin in build/out/qemu/bin/qemu-system-x86_64 build/out/qemu/bin/qemu-system-aarch64; do
  printf 'Built QEMU target: %s/%s\n' "$root_dir" "$bin"
done
