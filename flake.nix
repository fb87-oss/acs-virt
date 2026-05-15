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
    pkgsCrossArm64 = pkgs.pkgsCross.aarch64-multiplatform;
    lib = pkgs.lib;

    x64Kernel = pkgs.linuxPackages_latest.kernel;
    x64Busybox = pkgs.pkgsStatic.busybox;
    a64Kernel = pkgsArm64.linuxPackages_latest.kernel;
    a64Busybox = pkgsArm64.pkgsStatic.busybox;
    kernelModules = kernel: "lib/modules/${kernel.modDirVersion}/kernel";
    initrdFor = kernel: busybox: build-initrd kernel busybox (kernelModules kernel);
    x64Initrd = initrdFor x64Kernel x64Busybox;
    a64Initrd = initrdFor a64Kernel a64Busybox;
    x64KernelImage = "${x64Kernel}/bzImage";
    a64KernelImage = "${a64Kernel}/Image";
    uioModuleBuildPath = lib.makeBinPath [
      pkgs.gcc
      pkgs.gnumake
      pkgs.binutils
      pkgs.perl
      pkgs.bash
      pkgs.coreutils
      pkgs.gnused
      pkgs.gawk
      pkgs.findutils
      pkgs.gnugrep
    ];

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
      for module in virtio virtio_ring virtio_mmio virtio_blk virtio_console uio axi_mmio; do
        modprobe "$module" 2>/dev/null || true
      done
      modprobe uio_pdrv_genirq of_id=chiplets,uio 2>/dev/null || true

      for pass in 1 2 3; do
        while read -r i; do
          if [ -e "/$i" ]; then
            case "$i" in
              *uio_pdrv_genirq.ko) insmod "/$i" of_id=chiplets,uio 2>/dev/null || true ;;
              *) insmod "/$i" 2>/dev/null || true ;;
            esac
          fi
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

      # copy virtio and UIO kernel modules used by frontend/backend guests
      root="$PWD/initrd"
      copy_module() {
        dst=$root/${modules}/''${1%.xz}
        mkdir -p "$(dirname "$dst")"
        xz -dc "$1" > "$dst"
        echo "${modules}/''${1%.xz}" >> $root/modules.txt
      }

      ( cd ${kernel.modules}/${modules};
        for name in \
          virtio.ko.xz \
          virtio_ring.ko.xz \
          virtio_mmio.ko.xz \
          virtio_blk.ko.xz \
          virtio_console.ko.xz \
          uio.ko.xz \
          uio_pdrv_genirq.ko.xz; do
          path=$(find . -name "$name" -print -quit)
          [ -n "$path" ] && copy_module "$path"
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

      if [ ! -x "$PWD/build/out/virtio-blkd" ] || [ ! -x "$PWD/build/out/virtio-consoled" ]; then
        "$PWD/scripts/build-tools.sh"
      fi

      kernel_image=${kernel}/Image
      [ -f "$kernel_image" ] || kernel_image=${kernel}/bzImage
      exec ${pkgs.python3}/bin/python3 "$PWD/scripts/chiplets-launcher.py" \
        --kernel $kernel_image \
        --initrd ${initrdFor kernel busybox} \
        "$@"
    '';

    runvm-x64 = runvm x64Kernel x64Busybox;
    runvm-a64 = runvm a64Kernel a64Busybox;
    runuio-x64 = pkgs.writeShellScriptBin "runuio-x64" ''
      set -euo pipefail

      CHIPLETS_BACKEND_FABRIC=linux-uio CMAKE_BUILD_DIR=build/cmake-uio "$PWD/scripts/build-tools.sh"

      tmp=$(${pkgs.coreutils}/bin/mktemp -d "''${TMPDIR:-/tmp}/chiplets-uio-initrd.XXXXXX")
      cleanup() {
        ${pkgs.coreutils}/bin/chmod -R u+w "$tmp" 2>/dev/null || true
        ${pkgs.coreutils}/bin/rm -rf "$tmp"
      }
      trap cleanup EXIT

      ${pkgs.gzip}/bin/gzip -dc ${x64Initrd} |
        (cd "$tmp" && ${pkgs.cpio}/bin/cpio -id --quiet)
      ${pkgs.coreutils}/bin/chmod -R u+w "$tmp"

      module_build="$tmp/chiplets-uio-module"
      ${pkgs.coreutils}/bin/mkdir -p "$module_build"
      ${pkgs.coreutils}/bin/cp "$PWD/src/kernel/chiplets_uio.c" "$module_build/chiplets_uio.c"
      ${pkgs.coreutils}/bin/cp "$PWD/src/kernel/axi_mmio.c" "$module_build/axi_mmio.c"
      cat > "$module_build/Makefile" <<'EOF'
obj-m += chiplets_uio.o
obj-m += axi_mmio.o
EOF
      PATH=${uioModuleBuildPath}:$PATH \
        ${pkgs.gnumake}/bin/make -s -C ${x64Kernel.dev}/lib/modules/${x64Kernel.modDirVersion}/build ARCH=x86 M="$module_build" modules
      module_dst="lib/modules/${x64Kernel.modDirVersion}/kernel/drivers/uio/chiplets_uio.ko"
      ${pkgs.coreutils}/bin/mkdir -p "$tmp/$(${pkgs.coreutils}/bin/dirname "$module_dst")"
      ${pkgs.coreutils}/bin/cp "$module_build/chiplets_uio.ko" "$tmp/$module_dst"
      printf '%s\n' "$module_dst" >> "$tmp/modules.txt"
      module_dst="lib/modules/${x64Kernel.modDirVersion}/kernel/drivers/virtio/axi_mmio.ko"
      ${pkgs.coreutils}/bin/mkdir -p "$tmp/$(${pkgs.coreutils}/bin/dirname "$module_dst")"
      ${pkgs.coreutils}/bin/cp "$module_build/axi_mmio.ko" "$tmp/$module_dst"
      printf '%s\n' "$module_dst" >> "$tmp/modules.txt"

      ${pkgs.coreutils}/bin/mkdir -p "$tmp/bin"
      ${pkgs.coreutils}/bin/cp "$PWD/build/out/virtio-blkd" "$tmp/bin/virtio-blkd"
      ${pkgs.coreutils}/bin/cp "$PWD/build/out/virtio-consoled" "$tmp/bin/virtio-consoled"
      ${pkgs.coreutils}/bin/cp "$PWD/build/out/uio-membench" "$tmp/bin/uio-membench"

      copy_deps() {
        local bin=$1
        ${pkgs.glibc.bin}/bin/ldd "$bin" | while read -r a b c rest; do
          for path in "$a" "$b" "$c" $rest; do
            [ "''${path#/}" != "$path" ] || continue
            [ -e "$path" ] || continue
            ${pkgs.coreutils}/bin/mkdir -p "$tmp/$(${pkgs.coreutils}/bin/dirname "$path")"
            ${pkgs.coreutils}/bin/chmod u+w "$tmp/$(${pkgs.coreutils}/bin/dirname "$path")" 2>/dev/null || true
            ${pkgs.coreutils}/bin/chmod u+w "$tmp/$path" 2>/dev/null || true
            ${pkgs.coreutils}/bin/cp -f -L "$path" "$tmp/$path"
          done
        done
      }
      copy_deps "$PWD/build/out/virtio-blkd"
      copy_deps "$PWD/build/out/virtio-consoled"
      copy_deps "$PWD/build/out/uio-membench"

      initrd="$tmp/uio-initrd.gz"
      (cd "$tmp" && ${pkgs.findutils}/bin/find . -print0 |
        ${pkgs.cpio}/bin/cpio --null -ov --owner=0:0 --format=newc |
        ${pkgs.gzip}/bin/gzip -9 > "$initrd") >/dev/null 2>&1

      exec ${pkgs.python3}/bin/python3 "$PWD/scripts/chiplets-uio-x64.py" \
        --kernel ${x64KernelImage} \
        --initrd "$initrd" \
        "$@"
    '';
    runuio-a64 = pkgs.writeShellScriptBin "runuio-a64" ''
      set -euo pipefail

      work_tmp=$(${pkgs.coreutils}/bin/mktemp -d "''${TMPDIR:-/tmp}/chiplets-uio-a64.XXXXXX")
      cleanup() {
        ${pkgs.coreutils}/bin/chmod -R u+w "$work_tmp" 2>/dev/null || true
        ${pkgs.coreutils}/bin/rm -rf "$work_tmp"
      }
      trap cleanup EXIT

      build_dir="$work_tmp/build"
      out_dir="$work_tmp/out"
      ${pkgs.cmake}/bin/cmake -S "$PWD" -B "$build_dir" -G Ninja \
        -DCMAKE_MAKE_PROGRAM=${pkgs.ninja}/bin/ninja \
        -DCHIPLETS_FETCH_QEMU=OFF \
        -DCHIPLETS_BACKEND_FABRIC=linux-uio \
        -DCMAKE_SYSTEM_NAME=Linux \
        -DCMAKE_SYSTEM_PROCESSOR=aarch64 \
        -DCMAKE_C_COMPILER=${pkgsCrossArm64.stdenv.cc}/bin/aarch64-unknown-linux-gnu-gcc \
        -DCMAKE_RUNTIME_OUTPUT_DIRECTORY="$out_dir"
      ${pkgs.cmake}/bin/cmake --build "$build_dir" --target virtio-blkd virtio-consoled --parallel

      tmp="$work_tmp/initrd"
      ${pkgs.coreutils}/bin/mkdir -p "$tmp"

      ${pkgs.gzip}/bin/gzip -dc ${a64Initrd} |
        (cd "$tmp" && ${pkgs.cpio}/bin/cpio -id --quiet)
      ${pkgs.coreutils}/bin/chmod -R u+w "$tmp"

      ${pkgs.coreutils}/bin/mkdir -p "$tmp/bin"
      ${pkgs.coreutils}/bin/cp "$out_dir/virtio-blkd" "$tmp/bin/virtio-blkd"
      ${pkgs.coreutils}/bin/cp "$out_dir/virtio-consoled" "$tmp/bin/virtio-consoled"
      for libdir in ${pkgsCrossArm64.glibc}/lib ${pkgsCrossArm64.stdenv.cc.cc.lib}/lib; do
        dst="$tmp/$libdir"
        ${pkgs.coreutils}/bin/mkdir -p "$dst"
        ${pkgs.coreutils}/bin/cp -P "$libdir"/*.so* "$dst"/
      done

      initrd="$tmp/uio-initrd.gz"
      (cd "$tmp" && ${pkgs.findutils}/bin/find . -print0 |
        ${pkgs.cpio}/bin/cpio --null -ov --owner=0:0 --format=newc |
        ${pkgs.gzip}/bin/gzip -9 > "$initrd") >/dev/null 2>&1

      exec ${pkgs.python3}/bin/python3 "$PWD/scripts/chiplets-uio-x64.py" \
        --arch a64 \
        --kernel ${a64KernelImage} \
        --initrd "$initrd" \
        "$@"
    '';
    runuio-a64-backend-x64-frontend = pkgs.writeShellScriptBin "runuio-a64-backend-x64-frontend" ''
      exec ${runuio-a64}/bin/runuio-a64 \
        --frontend-arch x64 \
        --frontend-kernel ${x64KernelImage} \
        --frontend-initrd ${x64Initrd} \
        "$@"
    '';
    runuio-x64-backend-a64-frontend = pkgs.writeShellScriptBin "runuio-x64-backend-a64-frontend" ''
      exec ${runuio-x64}/bin/runuio-x64 \
        --frontend-arch a64 \
        --frontend-kernel ${a64KernelImage} \
        --frontend-initrd ${a64Initrd} \
        "$@"
    '';
  in
  {
    packages.x86_64-linux = {
      inherit runvm-x64 runvm-a64 runuio-x64 runuio-a64;
      inherit runuio-a64-backend-x64-frontend;
      inherit runuio-x64-backend-a64-frontend;

      x64 = runvm-x64;
      a64 = runvm-a64;
      default = runvm-x64;
    };
  };
}
