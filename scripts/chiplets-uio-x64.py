#!/usr/bin/env python3

import argparse
import json
import os
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import time
from pathlib import Path


MEMORY_SIZE = 512 * 1024 * 1024
MEMORY_ARG = "512M"
MMIO_SIZE = 0x1000
ARCH_CONFIGS = {
    "x64": {
        "description": "x86_64",
        "qemu": "out/qemu-x64-minimal/bin/qemu-system-x86_64",
        "machine": "microvm,pcie=off,ioapic2=on,virtio-mmio-transports=0,memory-backend=guestmem",
        "append": "console=ttyS0 root=/dev/ram0 rdinit=/linuxrc loglevel=8",
        "cpu_args": [],
        "frontend_blk_base": 0x0010_FEB0_0000,
        "frontend_con_base": 0x0010_FEB0_1000,
        "backend_blk_base": 0x0000_FEB0_0000,
        "backend_con_base": 0x0000_FEB0_1000,
        "backend_dma_base": 0x3000_0000,
        "frontend_ram_base": 0x0,
        "blk_irq": 16,
        "con_irq": 17,
    },
    "a64": {
        "description": "ARM64",
        "qemu": "out/qemu-arm64-default/bin/qemu-system-aarch64",
        "machine": "virt,highmem-mmio=on,highmem-mmio-size=1T,gic-version=3,acpi=off,memory-backend=guestmem",
        "append": "console=ttyAMA0 root=/dev/ram0 rdinit=/linuxrc loglevel=8",
        "cpu_args": ["-cpu", "max"],
        "frontend_blk_base": 0x0010_FEB0_0000,
        "frontend_con_base": 0x0010_FEB0_1000,
        "backend_blk_base": 0x0000_FEB0_0000,
        "backend_con_base": 0x0000_FEB0_1000,
        "backend_dma_base": 0x3000_0000,
        "frontend_ram_base": 0x4000_0000,
        "blk_irq": 48,
        "con_irq": 49,
    },
}


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Run the two-VM UIO tests")
    parser.add_argument("--arch", choices=tuple(ARCH_CONFIGS), default="x64")
    parser.add_argument("--frontend-arch", choices=tuple(ARCH_CONFIGS))
    parser.add_argument("--backend-arch", choices=tuple(ARCH_CONFIGS))
    parser.add_argument("--kernel", required=True)
    parser.add_argument("--initrd", required=True)
    parser.add_argument("--frontend-kernel")
    parser.add_argument("--frontend-initrd")
    parser.add_argument("--backend-kernel")
    parser.add_argument("--backend-initrd")
    parser.add_argument("--timeout", type=float, default=45.0)
    parser.add_argument("--run-dir")
    parser.add_argument("--mode", choices=("smoke", "benchmark"), default="smoke")
    parser.add_argument("--bench-size-mb", type=int, default=16)
    parser.add_argument("--bench-bs", default="64K")
    return parser.parse_args()


def parse_block_size(value: str) -> int:
    if value.endswith("K"):
        return int(value[:-1]) * 1024
    if value.endswith("M"):
        return int(value[:-1]) * 1024 * 1024
    return int(value)


def extract_dd_result(log_text: str, start: str, end: str) -> str:
    in_section = False
    result = ""

    for line in log_text.splitlines():
        if start in line:
            in_section = True
            continue
        if end in line:
            in_section = False
        if in_section and "copied" in line:
            result = line
    return result


def truncate(path: Path, size: int) -> None:
    with path.open("wb") as f:
        f.truncate(size)


def qemu_data_dir(qemu_bin: Path) -> Path:
    return qemu_bin.parent.parent / "share" / "qemu"


def common_qemu_args(qemu_bin: Path, kernel: Path, initrd: Path, ram_path: Path,
                     config: dict[str, object]) -> list[str]:
    args = [
        str(qemu_bin),
        "-L",
        str(qemu_data_dir(qemu_bin)),
        "-object",
        f"memory-backend-file,id=guestmem,mem-path={ram_path},size={MEMORY_ARG},share=on",
        "-machine",
        str(config["machine"]),
        "-m",
        MEMORY_ARG,
        "-nographic",
        "-kernel",
        str(kernel),
        "-initrd",
        str(initrd),
        "-append",
        str(config["append"]),
    ]
    args[7:7] = list(config["cpu_args"])
    if config is ARCH_CONFIGS["x64"]:
        args.insert(7, "-enable-kvm")
    return args


def memory_object_args(name: str, path: Path, size: int) -> list[str]:
    return [
        "-object",
        f"memory-backend-file,id={name},mem-path={path},size={size},share=on",
    ]


def axi_device_arg(name: str, base: int, irq: int, memdev: str, control: Path,
                   role: str, virtio_node: bool, dma_base: int,
                   notify_delay_us: int | None = None,
                   notify_ack: bool | None = None,
                   dma_memdev: str | None = None) -> str:
    parts = [
        "axi",
        f"id={name}",
        "mode=uio",
        f"role={role}",
        f"base=0x{base:x}",
        f"size=0x{MMIO_SIZE:x}",
        f"irq={irq}",
        f"memdev={memdev}",
        f"control-socket={control}",
        f"virtio-node={'on' if virtio_node else 'off'}",
    ]
    if notify_delay_us is not None:
        parts.append(f"notify-delay-us={notify_delay_us}")
    if notify_ack is not None:
        parts.append(f"notify-ack={'on' if notify_ack else 'off'}")
    if dma_memdev:
        parts.extend([
            f"dma-memdev={dma_memdev}",
            f"dma-base=0x{dma_base:x}",
            f"dma-size={MEMORY_SIZE}",
        ])
    return ",".join(parts)


def start_qemu(args: list[str], log_path: Path) -> tuple[subprocess.Popen, object]:
    log = log_path.open("ab", buffering=0)
    proc = subprocess.Popen(args, stdin=subprocess.PIPE, stdout=log, stderr=log)
    return proc, log


def write_guest(proc: subprocess.Popen, script: str) -> None:
    if proc.stdin is None:
        raise RuntimeError("guest stdin is not available")
    proc.stdin.write(script.encode())
    proc.stdin.flush()


def wait_log(path: Path, needle: str, timeout: float) -> bool:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if path.exists() and needle in path.read_text(errors="replace"):
            return True
        time.sleep(0.1)
    return False


def wait_log_line(path: Path, needle: str, timeout: float) -> bool:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if path.exists():
            for line in path.read_text(errors="replace").splitlines():
                if line.strip() == needle:
                    return True
        time.sleep(0.1)
    return False


def activate_shell(proc: subprocess.Popen, path: Path, timeout: float) -> None:
    if not wait_log(path, "Please press Enter to activate this console", timeout):
        raise TimeoutError(f"guest console did not ask for activation: {path}")
    write_guest(proc, "\n")
    if not wait_log(path, " #", timeout):
        raise TimeoutError(f"guest shell did not become ready: {path}")


def qmp_continue(path: Path, timeout: float) -> None:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as sock:
                sock.connect(str(path))
                sock.recv(4096)
                for command in ({"execute": "qmp_capabilities"}, {"execute": "cont"}):
                    sock.sendall(json.dumps(command).encode() + b"\r\n")
                    sock.recv(4096)
                return
        except OSError:
            time.sleep(0.1)
    raise TimeoutError(f"QMP socket not ready: {path}")


def terminate(processes: list[subprocess.Popen]) -> None:
    for proc in processes:
        if proc.poll() is None:
            proc.terminate()
    deadline = time.monotonic() + 3.0
    for proc in processes:
        while proc.poll() is None and time.monotonic() < deadline:
            time.sleep(0.05)
        if proc.poll() is None:
            proc.kill()


def main() -> int:
    args = parse_args()
    workspace = Path.cwd()
    frontend_arch = args.frontend_arch or args.arch
    backend_arch = args.backend_arch or args.arch
    frontend_config = ARCH_CONFIGS[frontend_arch]
    backend_config = ARCH_CONFIGS[backend_arch]
    frontend_kernel = Path(args.frontend_kernel or args.kernel)
    frontend_initrd = Path(args.frontend_initrd or args.initrd)
    backend_kernel = Path(args.backend_kernel or args.kernel)
    backend_initrd = Path(args.backend_initrd or args.initrd)
    frontend_qemu_bin = workspace / str(frontend_config["qemu"])
    backend_qemu_bin = workspace / str(backend_config["qemu"])
    if args.bench_size_mb <= 0:
        raise ValueError("--bench-size-mb must be greater than zero")
    bench_bs_bytes = parse_block_size(args.bench_bs)
    if bench_bs_bytes <= 0:
        raise ValueError("--bench-bs must be greater than zero")
    bench_bytes = args.bench_size_mb * 1024 * 1024
    bench_count = bench_bytes // bench_bs_bytes
    if bench_count <= 0:
        raise ValueError("--bench-size-mb is smaller than --bench-bs")

    arch_label = frontend_arch if frontend_arch == backend_arch else f"{backend_arch}-backend-{frontend_arch}-frontend"
    prefix = f"uio-{arch_label}-bench." if args.mode == "benchmark" else f"uio-{arch_label}-smoke."
    run_dir = Path(args.run_dir) if args.run_dir else Path(tempfile.mkdtemp(prefix=prefix, dir=os.environ.get("TMPDIR", "/tmp")))
    run_dir.mkdir(parents=True, exist_ok=True)
    include_console = frontend_arch != "a64" or args.mode != "benchmark"
    notify_delay_us = 25000 if frontend_arch == "a64" else 50000
    notify_ack = not (backend_arch == "a64" and frontend_arch == "x64")

    frontend_ram = run_dir / "frontend.ram"
    backend_ram = run_dir / "backend.ram"
    blk_mmio = run_dir / "blk.mmio"
    con_mmio = run_dir / "con.mmio"
    blk_control = run_dir / "blk.control.sock"
    con_control = run_dir / "con.control.sock"
    qmp = run_dir / "frontend.qmp"
    frontend_log = run_dir / "frontend.log"
    backend_log = run_dir / "backend.log"

    for path, size in ((frontend_ram, MEMORY_SIZE), (backend_ram, MEMORY_SIZE),
                       (blk_mmio, MMIO_SIZE), (con_mmio, MMIO_SIZE)):
        truncate(path, size)

    frontend_args = common_qemu_args(
        frontend_qemu_bin,
        frontend_kernel,
        frontend_initrd,
        frontend_ram,
        frontend_config,
    )
    frontend_args.extend([
        "-S",
        "-qmp",
        f"unix:{qmp},server=on,wait=off",
    ])
    frontend_args.extend(memory_object_args("blkmmio", blk_mmio, MMIO_SIZE))
    if include_console:
        frontend_args.extend(memory_object_args("conmmio", con_mmio, MMIO_SIZE))
    frontend_args.extend([
        "-device",
        axi_device_arg("blk0", int(frontend_config["frontend_blk_base"]),
                       int(frontend_config["blk_irq"]),
                       "blkmmio", blk_control, "frontend", True,
                       int(backend_config["backend_dma_base"]),
                       notify_delay_us, notify_ack),
    ])
    if include_console:
        frontend_args.extend([
            "-device",
            axi_device_arg("con0", int(frontend_config["frontend_con_base"]),
                           int(frontend_config["con_irq"]),
                           "conmmio", con_control, "frontend", True,
                           int(backend_config["backend_dma_base"]),
                           notify_delay_us, notify_ack),
        ])

    backend_args = common_qemu_args(
        backend_qemu_bin,
        backend_kernel,
        backend_initrd,
        backend_ram,
        backend_config,
    )
    backend_args.extend(memory_object_args("blkmmio", blk_mmio, MMIO_SIZE))
    if include_console:
        backend_args.extend(memory_object_args("conmmio", con_mmio, MMIO_SIZE))
    backend_args.extend(memory_object_args("frontendram", frontend_ram, MEMORY_SIZE))
    backend_args.extend([
        "-device",
        axi_device_arg("blk0", int(backend_config["backend_blk_base"]),
                       int(backend_config["blk_irq"]),
                       "blkmmio", blk_control, "backend", False,
                       int(backend_config["backend_dma_base"]), None, None,
                       "frontendram"),
    ])
    if include_console:
        backend_args.extend([
            "-device",
            axi_device_arg("con0", int(backend_config["backend_con_base"]),
                           int(backend_config["con_irq"]),
                           "conmmio", con_control, "backend", False,
                           int(backend_config["backend_dma_base"]), None, None,
                           "frontendram"),
        ])

    processes: list[subprocess.Popen] = []
    logs = []
    try:
        frontend, frontend_log_file = start_qemu(frontend_args, frontend_log)
        processes.append(frontend)
        logs.append(frontend_log_file)
        time.sleep(0.5)

        backend, backend_log_file = start_qemu(backend_args, backend_log)
        processes.append(backend)
        logs.append(backend_log_file)
        activate_shell(backend, backend_log, args.timeout)

        image_size_mb = max(64, args.bench_size_mb)
        uio_blk_endpoint = f"uio:/dev/uio0:0x200:0x{int(frontend_config['frontend_ram_base']):x}"
        uio_con_endpoint = f"uio:/dev/uio1:0x200:0x{int(frontend_config['frontend_ram_base']):x}"
        if include_console:
            backend_script = f"""
while [ ! -e /dev/uio0 ] || [ ! -e /dev/uio1 ]; do mdev -s; sleep 1; done
dd if=/dev/zero of=/blk0.img bs=1M count={image_size_mb}
/bin/virtio-blkd 'name=blk0,socket={uio_blk_endpoint},image=/blk0.img,readonly=false,ram_access=shared-mem' &
/bin/virtio-consoled 'name=con0,socket={uio_con_endpoint},output=-,ram_access=shared-mem' &
echo UIO_BACKEND_READY
wait
"""
        else:
            backend_script = f"""
while [ ! -e /dev/uio0 ]; do mdev -s; sleep 1; done
dd if=/dev/zero of=/blk0.img bs=1M count={image_size_mb}
/bin/virtio-blkd 'name=blk0,socket={uio_blk_endpoint},image=/blk0.img,readonly=false,ram_access=shared-mem' &
echo UIO_BACKEND_READY
wait
"""
        write_guest(backend, backend_script)
        if not wait_log(backend_log, "blkd: serving UIO device", args.timeout):
            raise TimeoutError("backend block UIO daemon did not start")
        if include_console and not wait_log(backend_log, "cond: serving UIO device", args.timeout):
            raise TimeoutError("backend console UIO daemon did not start")

        qmp_continue(qmp, 5.0)
        activate_shell(frontend, frontend_log, args.timeout)
        if args.mode == "benchmark":
            frontend_script = f"""
while [ ! -b /dev/vda ]; do sleep 1; done
echo BENCH_CONFIG size_mb={args.bench_size_mb} bs={args.bench_bs} count={bench_count}
echo WRITE_BENCH_START
dd if=/dev/zero of=/dev/vda bs={args.bench_bs} count={bench_count} 2>&1
sync
echo WRITE_BENCH_END
echo READ_BENCH_START
dd if=/dev/vda of=/dev/null bs={args.bench_bs} count={bench_count} 2>&1
echo READ_BENCH_END
echo UIO_BENCH_DONE
"""
            done_marker = "UIO_BENCH_DONE"
        else:
            frontend_script = """
while [ ! -b /dev/vda ]; do sleep 1; done
ls -l /dev/vda
dd if=/dev/zero of=/dev/vda bs=512 count=1
for i in $(seq 1 10); do [ -e /dev/hvc0 ] && break; sleep 1; done
ls -l /dev/hvc0
printf "uio-smoke-test\n" > /dev/hvc0
sync
echo UIO_SMOKE_TEST_DONE
"""
            done_marker = "UIO_SMOKE_TEST_DONE"
        write_guest(frontend, frontend_script)

        if not wait_log_line(frontend_log, done_marker, args.timeout):
            raise TimeoutError("frontend guest commands did not complete")
        checks = [(frontend_log, "virtio_blk virtio", "frontend did not probe virtio-blk")]
        if args.mode == "benchmark":
            checks.extend([
                (frontend_log, "WRITE_BENCH_END", "frontend write benchmark did not complete"),
                (frontend_log, "READ_BENCH_END", "frontend read benchmark did not complete"),
            ])
        else:
            checks.extend([
                (frontend_log, "1+0 records out", "frontend did not complete block write"),
                (frontend_log, "/dev/hvc0", "frontend did not list virtio console"),
                (backend_log, "request type=1", "backend block daemon did not observe a write"),
                (backend_log, "uio-smoke-test", "backend console daemon did not receive smoke string"),
            ])
        for path, needle, message in checks:
            if needle not in path.read_text(errors="replace"):
                raise RuntimeError(message)
        if args.mode == "benchmark":
            frontend_text = frontend_log.read_text(errors="replace")
            backend_text = backend_log.read_text(errors="replace")
            write_result = extract_dd_result(frontend_text, "WRITE_BENCH_START", "WRITE_BENCH_END")
            read_result = extract_dd_result(frontend_text, "READ_BENCH_START", "READ_BENCH_END")
            if not write_result or not read_result:
                raise RuntimeError("failed to parse dd throughput from frontend log")
            print("uio dd benchmark complete")
            print()
            print("benchmark summary")
            print(f"  config: size={args.bench_size_mb}MiB bs={args.bench_bs} count={bench_count}")
            print(f"  write:  {write_result}")
            print(f"  read:   {read_result}")
            print("  backend requests: "
                  f"read={backend_text.count('request type=0')} "
                  f"write={backend_text.count('request type=1')} "
                  f"flush={backend_text.count('request type=4')}")
        else:
            print(f"{backend_config['description']} backend / "
                  f"{frontend_config['description']} frontend UIO smoke test passed")
        print(f"run dir: {run_dir}")
        print(f"frontend log: {frontend_log}")
        print(f"backend log:  {backend_log}")
        return 0
    except Exception as exc:
        print(f"chiplets-uio-{arch_label}: {exc}", file=sys.stderr)
        print(f"run dir: {run_dir}", file=sys.stderr)
        print(f"frontend log: {frontend_log}", file=sys.stderr)
        print(f"backend log:  {backend_log}", file=sys.stderr)
        return 1
    finally:
        terminate(processes)
        for log in logs:
            log.close()
        if args.run_dir and os.environ.get("CHIPLETS_KEEP_UIO_RUN_DIR") != "1":
            cleanup_paths = [frontend_ram, backend_ram, blk_mmio, blk_control, qmp]
            if include_console:
                cleanup_paths.extend([con_mmio, con_control])
            for path in cleanup_paths:
                try:
                    path.unlink()
                except FileNotFoundError:
                    pass
        if not args.run_dir and os.environ.get("CHIPLETS_KEEP_UIO_RUN_DIR") != "1":
            shutil.rmtree(run_dir, ignore_errors=True)


if __name__ == "__main__":
    signal.signal(signal.SIGPIPE, signal.SIG_DFL)
    raise SystemExit(main())
