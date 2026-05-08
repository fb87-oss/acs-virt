# 0001-add-x86-64-microvm-minimal-device-config.patch

This patch adds a minimal QEMU device configuration for the `x86_64-softmmu`
target.

It creates:

```text
configs/devices/x86_64-softmmu/microvm-minimal.mak
```

The config enables only the pieces needed by the current bring-up path:

```text
CONFIG_MICROVM=y
CONFIG_AXI_BUS=y
```

## Purpose

The build script configures QEMU with `--without-default-devices`. That keeps the
binary small and prevents unrelated platform devices from being included by
default, but it also means the needed machine and custom device must be selected
explicitly.

This patch provides that explicit device list.

## Why It Exists

The current frontend VM needs:

- `microvm`, because it is the small x86_64 machine used for MMIO-only boot.
- `axi-bus`, because it is the QEMU-side MMIO forwarding device used by the C
  backend path.

Without this patch, the minimal QEMU build would not include the `microvm`
machine or the `axi-bus` device.

## Runtime Relationship

This patch only affects what is compiled into QEMU. It does not create any
runtime device by itself.

Runtime device creation still comes from the launcher-generated QEMU argument:

```text
-device axi-bus,...
```
