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
      copy_module ${moduleDir}/drivers/block/virtio_blk.ko

      (cd initrd; find . -print0 | \
                    cpio --null -ov --owner=0:0 --format=newc | \
                    gzip -9 > $out)
    '';

    runvm = pkgs.writeShellScriptBin "runvm" ''
      qemu_bin="''${QEMU_BIN:-$PWD/out/qemu-x64-minimal/bin/qemu-system-x86_64}"

      if [ ! -x "$qemu_bin" ]; then
        echo "QEMU binary not found: $qemu_bin" >&2
        echo "Run scripts/build-qemu-x64.sh from the workspace root, or set QEMU_BIN." >&2
        exit 1
      fi

      blk_args=()
      if [ -n "''${VHOST_BLK_SOCKET:-}" ]; then
        blk_args+=(
          -chardev "socket,id=vhostblk,path=$VHOST_BLK_SOCKET"
          -device "vhost-user-blk-pci,chardev=vhostblk,id=vhostblk0"
        )
      fi

      "$qemu_bin" -L "$PWD/deps/qemu/pc-bios" \
        -object memory-backend-memfd,id=guestmem,size=512M,share=on \
        -machine microvm,pcie=on,memory-backend=guestmem \
        -enable-kvm -m 512M -nographic \
        -kernel ${kernel}/bzImage \
        -initrd ${initrd} \
        -append "console=ttyS0 root=/dev/ram0 rdinit=/linuxrc loglevel=8" \
        "''${blk_args[@]}" \
        $@
    '';
  in
  {

    packages.x86_64-linux.default = runvm;
    packages.x86_64-linux.runvm = runvm;

  };
}
