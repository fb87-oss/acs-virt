# vim: tabstop=2 shiftwidth=2 expandtab autoindent colorcolumn=80
{
  description = "Flake for Inter-Chiplets Communication";

  inputs = {
    nixpkgs.url = "github:nixos/nixpkgs?ref=nixos-25.11-small";
  };

  outputs = { self, nixpkgs }:
  let
    pkgs = import nixpkgs { system = "x86_64-linux"; };
    pkgsArm64 = import nixpkgs { system = "aarch64-linux"; };

    build-initrc = modules: pkgs.writeScript "make-initrc" ''
      #!/bin/sh

      mount -t proc none /proc
      mount -t sysfs none /sys

      # and the device files
      mount -t devtmpfs none /dev

      # populate the device node
      mdev -s

      # Some kernels build virtio pieces in; modular kernels may need dependency
      # retries because this tiny initrd does not run full modprobe.
      for pass in 1 2 3; do
        while read -r i; do
          [ -e "/$i" ] && insmod "/$i" 2>/dev/null || true
        done < /modules.txt
      done

      mdev -s
    '';

    build-initrd = kernel: busybox: modules: pkgs.runCommand "make-initrd" {
      buildInputs = with pkgs; [ cpio rsync xz ];
    } ''
      # filesystem skeleton
      mkdir -p initrd/{dev,sys,proc,bin,sbin,etc/init.d}

      # copy initrd scripts
      cp -rf ${build-initrc modules} initrd/etc/init.d/rcS
      rsync -av ${busybox}/* initrd/

      # copy all the virtio kernel modules
      root="$PWD/initrd"
      copy_module() {
        dst=$root/${modules}/''${1%.xz}
        mkdir -p "$(dirname "$dst")"
        xz -dc "$1" > "$dst"
        echo "${modules}/''${1%.xz}" >> $root/modules.txt
      }

      ( cd ${kernel.modules}/${modules};
        for i in $(find . -name "virtio*.ko.xz"); do
          copy_module "$i"
        done
      )

      # relocate all modules file
      cp -f $(realpath ${kernel.modules}/${modules}/..)/modules.* \
            $(realpath initrd/${modules}/../)

      # pack everything
      (cd initrd; find . -print0 | \
                    cpio --null -ov --owner=0:0 --format=newc | \
                    gzip -9 > $out)
    '';

    runvm = kernel: busybox: pkgs.writeShellScriptBin "runvm" ''
      set -x

      if [ "$#" -lt 1 ]; then
      echo "usage: runvm [--dry-run] [--no-backend] <samples/*.toml> [-- <qemu-args>...]" >&2
        exit 2
      fi

      if [ ! -x "$PWD/out/virtio-blkd" ] || [ ! -x "$PWD/out/virtio-consoled" ]; then
        "$PWD/scripts/build-tools.sh"
      fi

      kernel_image=${kernel}/Image
      [ -f "$kernel_image" ] || kernel_image=${kernel}/bzImage
      exec ${pkgs.python3}/bin/python3 "$PWD/scripts/chiplets-launcher.py" \
        --kernel $kernel_image \
        --initrd ${build-initrd kernel busybox "lib/modules/${kernel.modDirVersion}/kernel" } \
        "$@"
    '';

    runvm-x64 = runvm pkgs.linuxPackages_latest.kernel pkgs.pkgsStatic.busybox;
    runvm-a64 = runvm pkgsArm64.linuxPackages_latest.kernel pkgsArm64.pkgsStatic.busybox;
  in
  {

    packages.x86_64-linux.runvm-x64 = runvm-x64;
    packages.x86_64-linux.runvm-a64 = runvm-a64;
    packages.x86_64-linux.x64 = runvm-x64;
    packages.x86_64-linux.a64 = runvm-a64;
    packages.x86_64-linux.default = runvm-x64;

  };
}
