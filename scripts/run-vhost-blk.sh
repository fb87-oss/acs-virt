#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SOCKET="${VHOST_BLK_SOCKET:-$ROOT_DIR/run/vhost-blk.sock}"
IMAGE="${1:-${VHOST_BLK_IMAGE:-$ROOT_DIR/run/disk.img}}"
SIZE="${VHOST_BLK_IMAGE_SIZE:-64M}"

mkdir -p "$(dirname "$SOCKET")" "$(dirname "$IMAGE")"

if [[ ! -f "$IMAGE" ]]; then
  truncate -s "$SIZE" "$IMAGE"
fi

exec nix shell \
  nixpkgs#cargo \
  nixpkgs#gcc \
  nixpkgs#rustc \
  --command cargo run --manifest-path "$ROOT_DIR/Cargo.toml" --bin vhost-blk -- \
    --socket "$SOCKET" \
    --image "$IMAGE"
