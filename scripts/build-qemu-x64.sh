#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
QEMU_SRC="$ROOT_DIR/deps/qemu"
PATCH_DIR="$ROOT_DIR/patches/qemu"
PATCHED_SRC="$ROOT_DIR/build/qemu-src-x64-minimal"
BUILD_DIR="${BUILD_DIR:-$ROOT_DIR/build/qemu-x64-minimal}"
INSTALL_DIR="${INSTALL_DIR:-$ROOT_DIR/out/qemu-x64-minimal}"
CCACHE_DIR="${CCACHE_DIR:-$ROOT_DIR/.cache/ccache}"

if [[ ! -d "$QEMU_SRC" ]]; then
  printf 'QEMU source directory not found: %s\n' "$QEMU_SRC" >&2
  exit 1
fi

if [[ -z "${IN_NIX_SHELL:-}" && -z "${QEMU_MINIMAL_NIX_SHELL:-}" ]]; then
  export QEMU_MINIMAL_NIX_SHELL=1
  exec nix develop --impure --expr '
    let pkgs = import <nixpkgs> {};
    in pkgs.mkShell {
      nativeBuildInputs = with pkgs; [ bash ccache gcc git meson ninja perl pkg-config python3 rsync ];
      buildInputs = with pkgs; [ glib pixman zlib ];
    }
  ' --command bash "$0" "$@"
fi

mkdir -p "$(dirname "$PATCHED_SRC")" "$BUILD_DIR" "$INSTALL_DIR" "$CCACHE_DIR"

rsync -a --delete \
  --exclude='.git/' \
  --exclude='build/' \
  "$QEMU_SRC/" "$PATCHED_SRC/"

if compgen -G "$PATCH_DIR/*.patch" >/dev/null; then
  for patch in "$PATCH_DIR"/*.patch; do
    git -C "$PATCHED_SRC" apply "$patch"
  done
fi

export CCACHE_DIR
export CC="ccache gcc"
export CXX="ccache g++"

cd "$BUILD_DIR"

"$PATCHED_SRC/configure" \
  --prefix="$INSTALL_DIR" \
  --target-list=x86_64-softmmu \
  --with-devices-x86_64=microvm-minimal \
  --without-default-features \
  --without-default-devices \
  --enable-fdt=internal \
  --disable-docs \
  --disable-tools \
  --disable-guest-agent \
  --disable-install-blobs \
  --disable-modules \
  --disable-slirp \
  --disable-vnc \
  --disable-sdl \
  --disable-gtk \
  --disable-curses \
  --disable-opengl \
  --disable-plugins \
  --disable-rust \
  --disable-werror \
  --enable-kvm \
  --enable-tcg \
  --enable-vhost-user \
  --enable-strip \
  --enable-trace-backends=nop \
  --audio-drv-list= \
  "$@"

ninja qemu-system-x86_64

mkdir -p "$INSTALL_DIR/bin"
install -m 0755 qemu-system-x86_64 "$INSTALL_DIR/bin/qemu-system-x86_64"
strip -s "$INSTALL_DIR/bin/qemu-system-x86_64"

printf 'Built minimal QEMU: %s/bin/qemu-system-x86_64\n' "$INSTALL_DIR"
