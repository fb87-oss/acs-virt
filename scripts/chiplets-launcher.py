#!/usr/bin/env python3

import argparse
import os
import shlex
import subprocess
import sys
import time
import tomllib
from pathlib import Path


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        prog="chiplets-launcher.py",
        usage="%(prog)s --kernel <bzImage> --initrd <initrd> [--dry-run] [--no-backend] <vm.toml> [-- <qemu-args>...]",
    )
    parser.add_argument("--kernel", required=True)
    parser.add_argument("--initrd", required=True)
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument("--no-backend", action="store_true", help="do not launch backend daemons")
    parser.add_argument("config")
    parser.add_argument("extra_qemu_args", nargs=argparse.REMAINDER)
    args = parser.parse_args()
    if args.extra_qemu_args and args.extra_qemu_args[0] == "--":
        args.extra_qemu_args = args.extra_qemu_args[1:]
    return args


def resolve(workspace: Path, value: str) -> Path:
    path = Path(value)
    return (path if path.is_absolute() else workspace / path).resolve()


def qemu_data_dir(qemu_bin: Path) -> Path | None:
    data_dir = qemu_bin.parent.parent / "share" / "qemu"
    if data_dir.is_dir() or qemu_bin.parent.parent.name == "qemu-x64-minimal":
        return data_dir
    return None


def validate_qemu_data_dir(qemu_args: list[str]) -> None:
    if "-L" not in qemu_args:
        return
    data_dir = Path(qemu_args[qemu_args.index("-L") + 1])
    if not data_dir.is_dir():
        raise FileNotFoundError(f"QEMU data dir not found: {data_dir}. Build the configured QEMU target first.")


def parse_int_literal(value: str) -> int:
    return int(value, 16) if value.startswith("0x") else int(value, 10)


def load_config(path: Path) -> dict:
    with path.open("rb") as f:
        return tomllib.load(f)


def validate(config: dict, qemu: dict) -> None:
    parameters = qemu.get("parameters", {})
    machine_type = qemu.get("type", "microvm")
    if machine_type not in ("microvm", "virt"):
        raise ValueError('only [targets.qemu] type = "microvm" or "virt" is supported')
    if parameters.get("pcie", False):
        raise ValueError("PCIe is not supported for this MMIO-only platform")
    if config.get("ram_access", "shared-mem") not in ("shared-mem", "qemu-mediated"):
        raise ValueError(f"invalid ram_access: {config.get('ram_access')}")


def iter_enabled_devices(config: dict, qemu: dict):
    devices = {device["name"]: device for device in config.get("devices", [])}
    for qemu_device in qemu.get("devices", []):
        name = qemu_device["name"]
        device = devices.get(name)
        if device is None:
            raise ValueError(f'qemu target references unknown device "{name}"')
        yield device, qemu_device


def iter_mmio_windows(config: dict, qemu: dict):
    for device, qemu_device in iter_enabled_devices(config, qemu):
        name = device["name"]
        mmio = device.get("mmio")
        if mmio is None:
            continue
        window = dict(mmio)
        window.update({key: qemu_device[key] for key in ("socket",) if key in qemu_device})
        window["name"] = name
        window["target"] = name
        yield window


def find_target(config: dict, name: str) -> dict | None:
    return config.get("targets", {}).get(name)


def validate_windows(windows: list[dict]) -> None:
    for window in windows:
        parse_int_literal(window["base"])
        parse_int_literal(window["size"])
        irq = int(window["irq"])
        if irq > 255:
            raise ValueError(f"mmio window {window['name']} has invalid irq {irq}")


def key_value_arg(values: dict) -> str:
    parts = []
    for key, value in values.items():
        if isinstance(value, bool):
            value = "true" if value else "false"
        else:
            value = str(value)
        if "," in value or "=" in value:
            raise ValueError(f'backend argument {key} contains unsupported character: {value}')
        parts.append(f"{key}={value}")
    return ",".join(parts)


def build_backend_commands(workspace: Path, config: dict, qemu: dict) -> list[tuple[list[str], Path | None]]:
    ram_access = config.get("ram_access", "shared-mem")
    commands = []
    for device, qemu_device in iter_enabled_devices(config, qemu):
        name = device["name"]
        device_type = device.get("type")
        socket = str(resolve(workspace, qemu_device["socket"]))
        log = resolve(workspace, qemu_device["log"]) if "log" in qemu_device else None
        common = {"name": name, "socket": socket, "ram_access": ram_access}

        if device_type == "virtio-blk":
            values = dict(common)
            values["image"] = str(resolve(workspace, qemu_device["image"]))
            if "readonly" in qemu_device:
                values["readonly"] = qemu_device["readonly"]
            commands.append(([str(resolve(workspace, "build/out/virtio-blkd")), key_value_arg(values)], log))
        elif device_type == "virtio-console":
            values = dict(common)
            if "output" in qemu_device:
                values["output"] = str(resolve(workspace, qemu_device["output"]))
            commands.append(([str(resolve(workspace, "build/out/virtio-consoled")), key_value_arg(values)], log))
        else:
            raise ValueError(f'unsupported device type for {name}: {device_type}')
    return commands


def build_qemu_args(workspace: Path, config: dict, kernel: str, initrd: str, extra_args: list[str]) -> list[str]:
    qemu = find_target(config, "qemu")
    if qemu is None:
        raise ValueError("missing [targets.qemu]")

    validate(config, qemu)
    windows = list(iter_mmio_windows(config, qemu))
    validate_windows(windows)

    parameters = qemu.get("parameters", {})
    machine_type = qemu.get("type", "microvm")
    memory = parameters.get("memory", "512M")
    pcie = "on" if parameters.get("pcie", False) else "off"
    ram_access = config.get("ram_access", "shared-mem")
    qemu_bin = resolve(workspace, qemu.get("binary", "build/out/qemu-x64-minimal/bin/qemu-system-x86_64"))
    data_dir = qemu_data_dir(qemu_bin)
    kernel_path = resolve(workspace, kernel)
    initrd_path = resolve(workspace, initrd)

    machine_args = []
    if machine_type == "microvm":
        machine_args = [
            "-machine",
            f"microvm,pcie={pcie},ioapic2=on,virtio-mmio-transports=0,memory-backend=guestmem",
            "-append",
            parameters.get("append", "console=ttyS0 root=/dev/ram0 rdinit=/linuxrc loglevel=8"),
        ]
    elif machine_type == "virt":
        machine_args = [
            "-machine",
            "virt,highmem-mmio=on,highmem-mmio-size=1T,gic-version=3,acpi=off,memory-backend=guestmem",
            "-cpu",
            parameters.get("cpu", "max"),
            "-append",
            parameters.get("append", "console=ttyAMA0 root=/dev/ram0 rdinit=/linuxrc loglevel=8"),
        ]

    args = [
        str(qemu_bin),
        "-object",
        f"memory-backend-memfd,id=guestmem,size={memory},share=on",
        "-m",
        memory,
        "-nographic",
        "-kernel",
        str(kernel_path),
        "-initrd",
        str(initrd_path),
    ]
    args[3:3] = machine_args

    if data_dir is not None:
        args[1:1] = ["-L", str(data_dir)]

    if parameters.get("kvm", True):
        args.append("-enable-kvm")

    for window in windows:
        socket = resolve(workspace, window["socket"])
        target = window["name"]
        args.extend(
            [
                "-device",
                "axi,"
                f"id={window['name']},"
                f"base={window['base']},"
                f"size={window['size']},"
                f"irq={window['irq']},"
                f"socket={socket},"
                f"ram-access={ram_access},"
                f"target={target}",
            ]
        )

    args.extend(extra_args)
    return args


def print_command(label: str, command: list[str]) -> None:
    print(f"{label}: " + " ".join(shlex.quote(arg) for arg in command))


def command_text(command: list[str]) -> str:
    return " ".join(shlex.quote(arg) for arg in command)


def write_start_scripts(workspace: Path, backend_commands: list[tuple[list[str], Path | None]], qemu_args: list[str]) -> None:
    run_dir = workspace / "run"
    run_dir.mkdir(parents=True, exist_ok=True)

    srv_lines = ["#!/usr/bin/env bash", "set -euo pipefail", ""]
    if backend_commands:
        for command, log_path in backend_commands:
            line = command_text(command)
            if log_path is not None:
                log_path.parent.mkdir(parents=True, exist_ok=True)
                line += f" >> {shlex.quote(str(log_path))} 2>&1"
            srv_lines.append(f"{line} &")
        srv_lines.append("wait")
    else:
        srv_lines.append("# No backend commands configured.")

    guest_lines = ["#!/usr/bin/env bash", "set -euo pipefail", "", f"exec {command_text(qemu_args)}"]

    scripts = {
        run_dir / "run-srv.sh": "\n".join(srv_lines) + "\n",
        run_dir / "run-guest.sh": "\n".join(guest_lines) + "\n",
    }
    for path, text in scripts.items():
        path.write_text(text)
        path.chmod(0o755)


def open_log(path: Path | None):
    if path is None:
        return None
    path.parent.mkdir(parents=True, exist_ok=True)
    return path.open("ab")


def terminate_processes(processes: list[subprocess.Popen]) -> None:
    for process in processes:
        if process.poll() is None:
            process.terminate()
    for process in processes:
        if process.poll() is None:
            try:
                process.wait(timeout=2)
            except subprocess.TimeoutExpired:
                process.kill()


def wait_for_sockets(commands: list[tuple[list[str], Path | None]], timeout: float = 5.0) -> None:
    sockets = backend_socket_paths(commands)

    deadline = time.monotonic() + timeout
    while True:
        missing = [socket for socket in sockets if not socket.exists()]
        if not missing:
            return
        if time.monotonic() >= deadline:
            missing_text = ", ".join(str(socket) for socket in missing)
            raise TimeoutError(f"backend sockets not ready: {missing_text}")
        time.sleep(0.05)


def backend_socket_paths(commands: list[tuple[list[str], Path | None]]) -> list[Path]:
    sockets = []
    for command, _ in commands:
        values = dict(part.split("=", 1) for part in command[1].split(","))
        sockets.append(Path(values["socket"]))
    return sockets


def main() -> int:
    args = parse_args()
    workspace = Path.cwd()
    try:
        config = load_config(Path(args.config))
        qemu = find_target(config, "qemu")
        if qemu is None:
            raise ValueError("missing [targets.qemu]")
        backend_commands = [] if args.no_backend else build_backend_commands(workspace, config, qemu)
        qemu_args = build_qemu_args(workspace, config, args.kernel, args.initrd, args.extra_qemu_args)
        write_start_scripts(workspace, backend_commands, qemu_args)
        qemu_bin = Path(qemu_args[0])
        if args.dry_run:
            for command, _ in backend_commands:
                print_command("backend", command)
            print_command("qemu", qemu_args)
            return 0
        for command, _ in backend_commands:
            if not os.access(command[0], os.X_OK):
                raise FileNotFoundError(f"backend binary not found: {command[0]}. Run scripts/build-tools.sh first.")
        if not os.access(qemu_bin, os.X_OK):
            raise FileNotFoundError(f"QEMU binary not found: {qemu_bin}. Build the configured QEMU target first.")
        validate_qemu_data_dir(qemu_args)
        processes = []
        logs = []
        try:
            for socket in backend_socket_paths(backend_commands):
                socket.unlink(missing_ok=True)
            for command, log_path in backend_commands:
                log = open_log(log_path)
                logs.append(log)
                output = log if log is not None else None
                processes.append(subprocess.Popen(command, stdout=output, stderr=output))
            wait_for_sockets(backend_commands)
            return subprocess.call(qemu_args)
        finally:
            terminate_processes(processes)
            for log in logs:
                if log is not None:
                    log.close()
    except Exception as e:
        print(f"chiplets-launcher: {e}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
