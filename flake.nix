# vim: tabstop=2 shiftwidth=2 expandtab autoindent colorcolumn=80
{
  description = "Flake for Inter-Chiplets Communication";

  inputs = {
    nixpkgs.url = "github:nixos/nixpkgs?ref=nixos-25.11-small";
  };

  outputs = { self, nixpkgs }:
  let
    pkgs = import nixpkgs { system = "x86_64-linux"; };
    kernel = pkgs.linuxPackages_latest.kernel;
    moduleDir = "lib/modules/${kernel.modDirVersion}/kernel";

    initrc = pkgs.writeScript "initrc" ''
      #!/bin/sh
      
      mount -t proc none /proc
      mount -t sysfs none /sys

      # and the device files
      mount -t devtmpfs none /dev

      # populate the device node
      mdev -s

      insmod /${moduleDir}/drivers/virtio/virtio_ring.ko 2>/dev/null || true
      insmod /${moduleDir}/drivers/virtio/virtio.ko 2>/dev/null || true
      insmod /${moduleDir}/drivers/virtio/virtio_pci_modern_dev.ko 2>/dev/null || true
      insmod /${moduleDir}/drivers/virtio/virtio_pci_legacy_dev.ko 2>/dev/null || true
      insmod /${moduleDir}/drivers/virtio/virtio_pci.ko 2>/dev/null || true
      virtio_mmio_params=""
      for arg in $(cat /proc/cmdline); do
        case "$arg" in
          virtio_mmio.device=*)
            virtio_mmio_params="$virtio_mmio_params device=''${arg#virtio_mmio.device=}"
            ;;
        esac
      done
      insmod /${moduleDir}/drivers/virtio/virtio_mmio.ko $virtio_mmio_params 2>/dev/null || true
      insmod /${moduleDir}/drivers/block/virtio_blk.ko 2>/dev/null || true
      mdev -s
    '';

    initrd = pkgs.runCommand "make-initrd" {
      buildInputs = with pkgs; [ cpio rsync xz ];
    } ''
      # filesystem skeleton
      mkdir -p initrd/{dev,sys,proc,bin,sbin,etc/init.d}

      # copy initrd scripts
      cp -rf ${initrc} initrd/etc/init.d/rcS
      rsync -av ${pkgs.pkgsStatic.busybox}/* initrd/

      copy_module() {
        src=${kernel.modules}/$1.xz
        dst=initrd/$1
        mkdir -p "$(dirname "$dst")"
        xz -dc "$src" > "$dst"
      }

      copy_module ${moduleDir}/drivers/virtio/virtio.ko
      copy_module ${moduleDir}/drivers/virtio/virtio_ring.ko
      copy_module ${moduleDir}/drivers/virtio/virtio_pci_modern_dev.ko
      copy_module ${moduleDir}/drivers/virtio/virtio_pci_legacy_dev.ko
      copy_module ${moduleDir}/drivers/virtio/virtio_pci.ko
      copy_module ${moduleDir}/drivers/virtio/virtio_mmio.ko
      copy_module ${moduleDir}/drivers/block/virtio_blk.ko

      (cd initrd; find . -print0 | \
                    cpio --null -ov --owner=0:0 --format=newc | \
                    gzip -9 > $out)
    '';

    runvm = pkgs.writeShellScriptBin "runvm" ''
      if [ "$#" -lt 1 ]; then
        echo "usage: runvm <configs/qemu-vms/*.toml> [-- <qemu-args>...]" >&2
        exit 2
      fi

      exec nix shell \
        nixpkgs#cargo \
        nixpkgs#gcc \
        nixpkgs#rustc \
        --command cargo run --quiet --manifest-path "$PWD/Cargo.toml" --bin qemu-launch -- \
          --kernel ${kernel}/bzImage \
          --initrd ${initrd} \
          "$@"
    '';
  in
  {

    packages.x86_64-linux.default = runvm;
    packages.x86_64-linux.runvm = runvm;

  };
}
