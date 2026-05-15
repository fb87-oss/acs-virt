#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
workspace="$(cd "$script_dir/.." && pwd)"

cache_volume="${NIX_CACHE_VOLUME:-nix-store-cache}"
image="${NIX_IMAGE:-nixos/nix:latest}"

runtime=
if   command -v docker &>/dev/null; then runtime=docker
elif command -v podman &>/dev/null; then runtime=podman
else echo "error: neither docker nor podman found" >&2; exit 1
fi

kvm_dev=()
[ -e /dev/kvm ] && kvm_dev=(--device /dev/kvm)

if [ "$runtime" = podman ]; then
  podman volume exists "$cache_volume" 2>/dev/null || podman volume create "$cache_volume"
else
  docker volume inspect "$cache_volume" &>/dev/null || docker volume create "$cache_volume"
fi

selinux=
[ "$runtime" = podman ] && selinux=:Z

# Project-specific env vars to always pass through
passthrough_envs=(
  CHIPLETS_BACKEND_FABRIC CHIPLETS_PROFILE_BACKEND
  CHIPLETS_DIRECT_READ_DMA CHIPLETS_UIO_NOTIFY_POLICY
  CHIPLETS_KEEP_UIO_RUN_DIR CMAKE_BUILD_DIR BUILD_DIR
  BENCH_SIZE_MB BENCH_BS BENCH_REPEAT BENCH_GUEST_TIMEOUT
  UIO_GUEST_TIMEOUT AXI_TEST_TMPDIR
)

env_args=()
for var in "${passthrough_envs[@]}"; do
  [ -n "${!var-}" ] && env_args+=(-e "$var")
done

# Parse -e/--env arguments from the command line; everything else is the command
cmd_args=()
skip=
for arg in "$@"; do
  if [ -n "$skip" ]; then
    env_args+=(-e "$arg"); skip=
  elif [ "$arg" = -e ] || [ "$arg" = --env ]; then
    skip=1
  elif [[ "$arg" == --env=* ]]; then
    env_args+=(-e "${arg#--env=}")
  else
    cmd_args+=("$arg")
  fi
done

exec "$runtime" run \
  --rm \
  -it \
  "${kvm_dev[@]}" \
  -e NIX_CONFIG="experimental-features = nix-command flakes" \
  "${env_args[@]}" \
  -v "$workspace:$workspace$selinux" \
  -w "$workspace" \
  -v "$cache_volume:/nix$selinux" \
  "$image" \
  "${cmd_args[@]}"
