# QEMU Build Configuration

The QEMU build is configured directly in `CMakeLists.txt` under the
`if(CHIPLETS_FETCH_QEMU)` block. It builds both `x86_64-softmmu` and
`aarch64-softmmu` in a single CMake ExternalProject.

Build it:

```sh
scripts/build-qemu.sh
```

## Configure Flags

The QEMU configure invocation uses:

```
--target-list=x86_64-softmmu,aarch64-softmmu
--with-devices-x86_64=microvm-minimal
--with-devices-aarch64=virt-minimal
```

Both machine types share the same patched source tree and build directory.

## Architecture-Specific Device Configs

- **x86_64** uses `microvm-minimal`: the microvm machine with only the `axi`
  sysbus device enabled.
- **AArch64** uses `virt-minimal`: the `virt` machine with only the `axi`
  sysbus device enabled.

The `axi` QEMU proxy supports x86 microvm ACPI discovery and AArch64 `virt`
FDT discovery.
