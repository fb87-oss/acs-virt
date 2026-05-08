# cond-virtio.c

`cond-virtio.c` implements the minimal virtio-mmio virtio-console device model
used by `cond`.

It exposes virtio device ID `3` and currently implements the simple two-queue
console shape:

```text
queue 0: guest receive queue, currently left pending because host input is not implemented
queue 1: guest transmit queue, copied to the host console backend
```

## Responsibilities

- expose virtio-mmio magic/version/device/vendor registers
- report `VIRTIO_CONSOLE_F_SIZE` in feature word 0
- report `VIRTIO_F_VERSION_1` in feature word 1
- store driver feature selection and device status
- store per-queue descriptor, driver ring, and device ring addresses
- process queue notifications
- leave queue `0` receive buffers pending until host input support exists
- read transmit descriptors from guest memory through `cond-axi.c`
- write guest output to `cond-console.c`
- add used-ring entries and raise interrupts
- expose a simple 80x25 console size in config space

## Limitations

The current implementation is a bring-up stub. It does not yet inject host input
into the guest receive queue, implement multiport control queues, or expose
console resize events.
