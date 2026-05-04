# vim: tabstop=2 shiftwidth=2 expandtab autoindent colorcolumn=80
{
  description = "Flake for Inter-Chiplets Communication";

  inputs = {
    nixpkgs.url = "github:nixos/nixpkgs?ref=nixos-25.11-small";
  };

  outputs = { self, nixpkgs }:
  let
    pkgs = import nixpkgs { system = "x86_64-linux"; };

    initrc = pkgs.writeScript "initrc" ''
      #!/bin/sh
      
      mount -t proc none /proc
      mount -t sysfs none /sys

      # and the device files
      mount -t devtmpfs none /dev

      # populate the device node
      mdev -s
    '';

    initrd = pkgs.runCommand "make-initrd" {
      buildInputs = with pkgs; [ cpio rsync ];
    } ''
      # filesystem skeleton
      mkdir -p initrd/{dev,sys,proc,bin,sbin,etc/init.d}

      # copy initrd scripts
      cp -rf ${initrc} initrd/etc/init.d/rcS
      cp -rf ${pkgs.linuxPackages_latest.kernel.modules}/lib initrd/
      rsync -av ${pkgs.pkgsStatic.busybox}/* initrd/

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

      "$qemu_bin" -L "$PWD/deps/qemu/pc-bios" \
        -machine microvm -enable-kvm -m 512M -nographic \
        -kernel ${pkgs.linuxPackages_latest.kernel}/bzImage \
        -initrd ${initrd} \
        -append "console=ttyS0 root=/dev/ram0 rdinit=/linuxrc loglevel=8" \
        $@
    '';
  in
  {

    packages.x86_64-linux.default = runvm;
    packages.x86_64-linux.runvm = runvm;

  };
}
