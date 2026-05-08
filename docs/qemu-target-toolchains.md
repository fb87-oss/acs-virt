# QEMU Target Toolchains

QEMU build target selection lives in small CMake files under:

```text
cmake/qemu-targets/
```

The default target file is:

```text
cmake/qemu-targets/x64-minimal.cmake
```

`CMakeLists.txt` fetches the QEMU release tarball with `ExternalProject` and
includes the target file selected by `CHIPLETS_QEMU_TARGET_FILE` when
`CHIPLETS_FETCH_QEMU=ON`.

## Required Variables

Each QEMU target file must define:

- `CHIPLETS_QEMU_NAME`: CMake ExternalProject target name.
- `CHIPLETS_QEMU_INSTALL_NAME`: install directory name under `out/`.
- `CHIPLETS_QEMU_SOURCE_COPY_NAME`: patched source copy directory name under
  `build/`.
- `CHIPLETS_QEMU_BUILD_NAME`: QEMU build directory name under `build/`.
- `CHIPLETS_QEMU_SOFTMMU_TARGET`: QEMU `--target-list` value.
- `CHIPLETS_QEMU_DEVICE_CONFIG_TARGET`: suffix used by QEMU's
  `--with-devices-<target>=...` configure option.
- `CHIPLETS_QEMU_DEVICE_CONFIG`: optional QEMU device allowlist config name. Set
  it to an empty string to let QEMU use the target's allnoconfig baseline.
- `CHIPLETS_QEMU_SYSTEM_BINARY`: built QEMU system binary name.

## Current x86_64 Target

```cmake
set(CHIPLETS_QEMU_NAME qemu-x64-minimal)
set(CHIPLETS_QEMU_INSTALL_NAME qemu-x64-minimal)
set(CHIPLETS_QEMU_SOURCE_COPY_NAME qemu-src-x64-minimal)
set(CHIPLETS_QEMU_BUILD_NAME qemu-x64-minimal)

set(CHIPLETS_QEMU_SOFTMMU_TARGET x86_64-softmmu)
set(CHIPLETS_QEMU_DEVICE_CONFIG_TARGET x86_64)
set(CHIPLETS_QEMU_DEVICE_CONFIG microvm-minimal)
set(CHIPLETS_QEMU_SYSTEM_BINARY qemu-system-x86_64)
```

Build it with:

```sh
scripts/build-qemu-x64.sh
```

## Adding Another Target

For AArch64, the repository provides:

```text
cmake/qemu-targets/arm64-default.cmake
```

It builds `aarch64-softmmu` with the patched `virt-axi` device allowlist:

```cmake
set(CHIPLETS_QEMU_NAME qemu-arm64-default)
set(CHIPLETS_QEMU_SOFTMMU_TARGET aarch64-softmmu)
set(CHIPLETS_QEMU_DEVICE_CONFIG_TARGET aarch64)
set(CHIPLETS_QEMU_DEVICE_CONFIG virt-axi)
set(CHIPLETS_QEMU_SYSTEM_BINARY qemu-system-aarch64)
```

Build it with:

```sh
scripts/build-qemu-arm64.sh
```

The `axi-bus` QEMU proxy supports x86 microvm ACPI discovery and AArch64 `virt`
FDT discovery. The current launch sample remains x86 microvm-specific because it
uses the x86 kernel/initrd and microvm machine parameters.

Configure CMake with a custom target file using:

```sh
cmake -S . -B build/cmake-qemu-arm64 -G Ninja \
  -DCHIPLETS_FETCH_QEMU=ON \
  -DCHIPLETS_QEMU_TARGET_FILE="$PWD/cmake/qemu-targets/arm64-default.cmake"
```
