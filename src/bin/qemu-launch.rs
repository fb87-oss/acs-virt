use std::env;
use std::ffi::OsString;
use std::fs;
use std::io;
use std::path::{Path, PathBuf};
use std::process::{Command, ExitCode};

use serde::Deserialize;

#[derive(Debug, Deserialize)]
struct VmConfig {
    machine: MachineConfig,
    qemu: Option<QemuConfig>,
    transport: Option<TransportConfig>,
    #[serde(default)]
    mmio_windows: Vec<MmioWindow>,
}

#[derive(Debug, Deserialize)]
struct MachineConfig {
    #[serde(rename = "type")]
    machine_type: Option<String>,
    memory: Option<String>,
    kvm: Option<bool>,
    pcie: Option<bool>,
}

#[derive(Debug, Deserialize)]
struct QemuConfig {
    binary: Option<String>,
    bios_dir: Option<String>,
}

#[derive(Debug, Deserialize)]
struct TransportConfig {
    ram_access: Option<RamAccess>,
}

#[derive(Debug, Clone, Copy, Deserialize, PartialEq, Eq)]
#[serde(rename_all = "kebab-case")]
enum RamAccess {
    SharedMem,
    QemuMediated,
}

impl RamAccess {
    fn as_str(self) -> &'static str {
        match self {
            RamAccess::SharedMem => "shared-mem",
            RamAccess::QemuMediated => "qemu-mediated",
        }
    }
}

#[derive(Debug, Deserialize)]
struct MmioWindow {
    name: String,
    base: String,
    size: String,
    irq: u32,
    guest_irq: Option<u32>,
    socket: String,
    target: Option<String>,
    enabled: Option<bool>,
}

struct Args {
    config: PathBuf,
    kernel: PathBuf,
    initrd: PathBuf,
    dry_run: bool,
    extra_qemu_args: Vec<OsString>,
}

fn usage(program: &str) -> ! {
    eprintln!(
        "usage: {program} --kernel <bzImage> --initrd <initrd> [--dry-run] <vm.toml> [-- <qemu-args>...]"
    );
    std::process::exit(2);
}

fn main() -> ExitCode {
    match run() {
        Ok(code) => code,
        Err(e) => {
            eprintln!("qemu-launch: {e}");
            ExitCode::from(1)
        }
    }
}

fn run() -> io::Result<ExitCode> {
    let args = parse_args();
    let workspace = env::current_dir()?;
    let config_text = fs::read_to_string(&args.config)?;
    let config: VmConfig = toml::from_str(&config_text)
        .map_err(|e| io::Error::new(io::ErrorKind::InvalidInput, e.to_string()))?;

    validate_config(&config)?;

    let qemu_bin = resolve_path(
        &workspace,
        config
            .qemu
            .as_ref()
            .and_then(|q| q.binary.as_deref())
            .unwrap_or("out/qemu-x64-minimal/bin/qemu-system-x86_64"),
    );
    let bios_dir = resolve_path(
        &workspace,
        config
            .qemu
            .as_ref()
            .and_then(|q| q.bios_dir.as_deref())
            .unwrap_or("deps/qemu/pc-bios"),
    );

    if !args.dry_run && !qemu_bin.exists() {
        return Err(io::Error::new(
            io::ErrorKind::NotFound,
            format!(
                "QEMU binary not found: {}. Run scripts/build-qemu-x64.sh first.",
                qemu_bin.display()
            ),
        ));
    }

    let memory = config.machine.memory.as_deref().unwrap_or("512M");
    let pcie = config.machine.pcie.unwrap_or(false);
    let kvm = config.machine.kvm.unwrap_or(true);
    let ram_access = config
        .transport
        .as_ref()
        .and_then(|t| t.ram_access)
        .unwrap_or(RamAccess::SharedMem);

    let enabled_windows: Vec<&MmioWindow> = config
        .mmio_windows
        .iter()
        .filter(|w| w.enabled.unwrap_or(true))
        .collect();
    let mut kernel_cmdline = String::from("console=ttyS0 root=/dev/ram0 rdinit=/linuxrc loglevel=8");
    for window in &enabled_windows {
        let window_size = parse_u64(&window.size)?;
        let guest_irq = window.guest_irq.unwrap_or(window.irq);
        kernel_cmdline.push_str(&format!(
            " virtio_mmio.device={}@{}:{}",
            window_size, window.base, guest_irq
        ));
    }

    let mut qemu_args = vec![
        OsString::from("-L"),
        bios_dir.into_os_string(),
        OsString::from("-object"),
        OsString::from(format!(
            "memory-backend-memfd,id=guestmem,size={memory},share=on"
        )),
        OsString::from("-machine"),
        OsString::from(format!(
            "microvm,pcie={},virtio-mmio-transports=0,memory-backend=guestmem",
            if pcie { "on" } else { "off" }
        )),
        OsString::from("-m"),
        OsString::from(memory),
        OsString::from("-nographic"),
        OsString::from("-kernel"),
        args.kernel.into_os_string(),
        OsString::from("-initrd"),
        args.initrd.into_os_string(),
        OsString::from("-append"),
        OsString::from(kernel_cmdline),
    ];

    if kvm {
        qemu_args.push(OsString::from("-enable-kvm"));
    }

    for window in enabled_windows {
        qemu_args.push(OsString::from("-device"));
        qemu_args.push(OsString::from(format!(
            "axi-mmio-proxy,id={},base={},size={},irq={},socket={},ram-access={},target={}",
            window.name,
            window.base,
            window.size,
            window.irq,
            path_for_qemu(&workspace, &window.socket),
            ram_access.as_str(),
            window.target.as_deref().unwrap_or(&window.name)
        )));
    }

    qemu_args.extend(args.extra_qemu_args);

    if args.dry_run {
        print_command(&qemu_bin, &qemu_args);
        return Ok(ExitCode::SUCCESS);
    }

    let status = Command::new(&qemu_bin).args(&qemu_args).status()?;
    Ok(ExitCode::from(status.code().unwrap_or(1) as u8))
}

fn parse_args() -> Args {
    let mut raw = env::args_os();
    let program = raw
        .next()
        .and_then(|s| s.into_string().ok())
        .unwrap_or_else(|| "qemu-launch".to_string());

    let mut config = None;
    let mut kernel = None;
    let mut initrd = None;
    let mut dry_run = false;
    let mut extra_qemu_args = Vec::new();

    while let Some(arg) = raw.next() {
        if arg == "--" {
            extra_qemu_args.extend(raw);
            break;
        }

        match arg.to_str() {
            Some("--kernel") => kernel = raw.next().map(PathBuf::from),
            Some("--initrd") => initrd = raw.next().map(PathBuf::from),
            Some("--dry-run") => dry_run = true,
            Some("--help" | "-h") => usage(&program),
            Some(s) if s.starts_with('-') => usage(&program),
            _ if config.is_none() => config = Some(PathBuf::from(arg)),
            _ => usage(&program),
        }
    }

    Args {
        config: config.unwrap_or_else(|| usage(&program)),
        kernel: kernel.unwrap_or_else(|| usage(&program)),
        initrd: initrd.unwrap_or_else(|| usage(&program)),
        dry_run,
        extra_qemu_args,
    }
}

fn validate_config(config: &VmConfig) -> io::Result<()> {
    if config.machine.machine_type.as_deref().unwrap_or("microvm") != "microvm" {
        return Err(io::Error::new(
            io::ErrorKind::InvalidInput,
            "only machine.type = \"microvm\" is supported",
        ));
    }

    if config.machine.pcie.unwrap_or(false) {
        return Err(io::Error::new(
            io::ErrorKind::InvalidInput,
            "PCIe is not supported for this MMIO-only platform",
        ));
    }

    for window in &config.mmio_windows {
        parse_u64(&window.base)?;
        parse_u64(&window.size)?;
        if window.irq > 255 {
            return Err(io::Error::new(
                io::ErrorKind::InvalidInput,
                format!("mmio window {} has invalid irq {}", window.name, window.irq),
            ));
        }
        if window.guest_irq.unwrap_or(window.irq) > 255 {
            return Err(io::Error::new(
                io::ErrorKind::InvalidInput,
                format!(
                    "mmio window {} has invalid guest_irq {}",
                    window.name,
                    window.guest_irq.unwrap_or(window.irq)
                ),
            ));
        }
    }

    Ok(())
}

fn parse_u64(value: &str) -> io::Result<u64> {
    if let Some(hex) = value.strip_prefix("0x") {
        u64::from_str_radix(hex, 16)
    } else {
        value.parse()
    }
    .map_err(|e| io::Error::new(io::ErrorKind::InvalidInput, e.to_string()))
}

fn resolve_path(workspace: &Path, path: &str) -> PathBuf {
    let path = PathBuf::from(path);
    if path.is_absolute() {
        path
    } else {
        workspace.join(path)
    }
}

fn path_for_qemu(workspace: &Path, path: &str) -> String {
    resolve_path(workspace, path).display().to_string()
}

fn print_command(qemu_bin: &Path, args: &[OsString]) {
    print!("{}", shell_quote(qemu_bin.as_os_str()));
    for arg in args {
        print!(" {}", shell_quote(arg));
    }
    println!();
}

fn shell_quote(value: &std::ffi::OsStr) -> String {
    let value = value.to_string_lossy();
    if value
        .chars()
        .all(|c| c.is_ascii_alphanumeric() || matches!(c, '/' | '.' | '_' | '-' | ':' | '=' | ','))
    {
        value.into_owned()
    } else {
        format!("'{}'", value.replace('\'', "'\\''"))
    }
}
