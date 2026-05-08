#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CMAKE_BUILD_DIR="${CMAKE_BUILD_DIR:-$ROOT_DIR/build/cmake-qemu}"

if [[ -z "${IN_NIX_SHELL:-}" && -z "${QEMU_MINIMAL_NIX_SHELL:-}" ]]; then
  export QEMU_MINIMAL_NIX_SHELL=1
  exec nix develop --impure --expr '
    let pkgs = import <nixpkgs> {};
    in pkgs.mkShell {
      nativeBuildInputs = with pkgs; [ bash ccache cmake gcc git meson ninja perl pkg-config python3 rsync ];
      buildInputs = with pkgs; [ glib pixman zlib ];
    }
  ' --command bash "$0" "$@"
fi

cmake -S "$ROOT_DIR" -B "$CMAKE_BUILD_DIR" -G Ninja -DCHIPLETS_FETCH_QEMU=ON
cmake --build "$CMAKE_BUILD_DIR" --target qemu-x64-minimal --parallel

printf 'Built minimal QEMU: %s/out/qemu-x64-minimal/bin/qemu-system-x86_64\n' "$ROOT_DIR"
