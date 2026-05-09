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
      for module in virtio virtio_ring virtio_mmio virtio_blk virtio_console uio; do
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
    runuio-x64 = pkgs.writeShellScriptBin "runuio-x64" ''
      set -euo pipefail

      CHIPLETS_BACKEND_FABRIC=linux-uio CMAKE_BUILD_DIR=build/cmake-uio "$PWD/scripts/build-tools.sh"

      tmp=$(${pkgs.coreutils}/bin/mktemp -d "''${TMPDIR:-/tmp}/chiplets-uio-initrd.XXXXXX")
      cleanup() {
        ${pkgs.coreutils}/bin/chmod -R u+w "$tmp" 2>/dev/null || true
        ${pkgs.coreutils}/bin/rm -rf "$tmp"
      }
      trap cleanup EXIT

      ${pkgs.gzip}/bin/gzip -dc ${build-initrd pkgs.linuxPackages_latest.kernel pkgs.pkgsStatic.busybox "lib/modules/${pkgs.linuxPackages_latest.kernel.modDirVersion}/kernel" } |
        (cd "$tmp" && ${pkgs.cpio}/bin/cpio -id --quiet)
      ${pkgs.coreutils}/bin/chmod -R u+w "$tmp"

      module_build="$tmp/chiplets-uio-module"
      ${pkgs.coreutils}/bin/mkdir -p "$module_build"
      ${pkgs.coreutils}/bin/cp "$PWD/src/kernel/chiplets_uio.c" "$module_build/chiplets_uio.c"
      cat > "$module_build/Makefile" <<'EOF'
obj-m += chiplets_uio.o
EOF
      PATH=${pkgs.gcc}/bin:${pkgs.gnumake}/bin:${pkgs.binutils}/bin:${pkgs.perl}/bin:${pkgs.bash}/bin:${pkgs.coreutils}/bin:$PATH \
        ${pkgs.gnumake}/bin/make -s -C ${pkgs.linuxPackages_latest.kernel.dev}/lib/modules/${pkgs.linuxPackages_latest.kernel.modDirVersion}/build M="$module_build" modules
      module_dst="lib/modules/${pkgs.linuxPackages_latest.kernel.modDirVersion}/kernel/drivers/uio/chiplets_uio.ko"
      ${pkgs.coreutils}/bin/mkdir -p "$tmp/$(${pkgs.coreutils}/bin/dirname "$module_dst")"
      ${pkgs.coreutils}/bin/cp "$module_build/chiplets_uio.ko" "$tmp/$module_dst"
      printf '%s\n' "$module_dst" >> "$tmp/modules.txt"

      ${pkgs.coreutils}/bin/mkdir -p "$tmp/bin"
      ${pkgs.coreutils}/bin/cp "$PWD/out/virtio-blkd" "$tmp/bin/virtio-blkd"
      ${pkgs.coreutils}/bin/cp "$PWD/out/virtio-consoled" "$tmp/bin/virtio-consoled"

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
      copy_deps "$PWD/out/virtio-blkd"
      copy_deps "$PWD/out/virtio-consoled"

      initrd="$tmp/uio-initrd.gz"
      (cd "$tmp" && ${pkgs.findutils}/bin/find . -print0 |
        ${pkgs.cpio}/bin/cpio --null -ov --owner=0:0 --format=newc |
        ${pkgs.gzip}/bin/gzip -9 > "$initrd") >/dev/null 2>&1

      exec ${pkgs.python3}/bin/python3 "$PWD/scripts/chiplets-uio-x64.py" \
        --kernel ${pkgs.linuxPackages_latest.kernel}/bzImage \
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

      ${pkgs.gzip}/bin/gzip -dc ${build-initrd pkgsArm64.linuxPackages_latest.kernel pkgsArm64.pkgsStatic.busybox "lib/modules/${pkgsArm64.linuxPackages_latest.kernel.modDirVersion}/kernel" } |
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
        --kernel ${pkgsArm64.linuxPackages_latest.kernel}/Image \
        --initrd "$initrd" \
        "$@"
    '';
    runuio-a64-backend-x64-frontend = pkgs.writeShellScriptBin "runuio-a64-backend-x64-frontend" ''
      exec ${runuio-a64}/bin/runuio-a64 \
        --frontend-arch x64 \
        --frontend-kernel ${pkgs.linuxPackages_latest.kernel}/bzImage \
        --frontend-initrd ${build-initrd pkgs.linuxPackages_latest.kernel pkgs.pkgsStatic.busybox "lib/modules/${pkgs.linuxPackages_latest.kernel.modDirVersion}/kernel" } \
        "$@"
    '';
    runuio-x64-backend-a64-frontend = pkgs.writeShellScriptBin "runuio-x64-backend-a64-frontend" ''
      exec ${runuio-x64}/bin/runuio-x64 \
        --frontend-arch a64 \
        --frontend-kernel ${pkgsArm64.linuxPackages_latest.kernel}/Image \
        --frontend-initrd ${build-initrd pkgsArm64.linuxPackages_latest.kernel pkgsArm64.pkgsStatic.busybox "lib/modules/${pkgsArm64.linuxPackages_latest.kernel.modDirVersion}/kernel" } \
        "$@"
    '';
  in
  {

    packages.x86_64-linux.runvm-x64 = runvm-x64;
    packages.x86_64-linux.runvm-a64 = runvm-a64;
    packages.x86_64-linux.runuio-x64 = runuio-x64;
    packages.x86_64-linux.runuio-a64 = runuio-a64;
    packages.x86_64-linux.runuio-a64-backend-x64-frontend = runuio-a64-backend-x64-frontend;
    packages.x86_64-linux.runuio-x64-backend-a64-frontend = runuio-x64-backend-a64-frontend;
    packages.x86_64-linux.x64 = runvm-x64;
    packages.x86_64-linux.a64 = runvm-a64;
    packages.x86_64-linux.default = runvm-x64;

  };
}
