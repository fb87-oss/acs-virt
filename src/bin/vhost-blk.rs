use std::env;
use std::fs::{File, OpenOptions};
use std::io::{self, Read, Write};
use std::os::unix::fs::FileExt;
use std::path::PathBuf;
use std::ops::Deref;
use std::sync::{Arc, Mutex, RwLock};

use vhost::vhost_user::message::{VhostUserProtocolFeatures, VhostUserVirtioFeatures};
use vhost_user_backend::{VhostUserBackendMut, VhostUserDaemon, VringRwLock, VringT};
use virtio_bindings::bindings::virtio_blk::{
    VIRTIO_BLK_F_BLK_SIZE, VIRTIO_BLK_F_FLUSH, VIRTIO_BLK_S_IOERR, VIRTIO_BLK_S_OK,
    VIRTIO_BLK_S_UNSUPP, VIRTIO_BLK_T_FLUSH, VIRTIO_BLK_T_GET_ID, VIRTIO_BLK_T_IN,
    VIRTIO_BLK_T_OUT,
};
use virtio_bindings::bindings::virtio_config::VIRTIO_F_VERSION_1;
use virtio_queue::QueueOwnedT;
use vm_memory::{ByteValued, GuestAddressSpace, GuestMemoryAtomic, GuestMemoryMmap};
use vmm_sys_util::epoll::EventSet;

const SECTOR_SIZE: u64 = 512;
const QUEUE_SIZE: usize = 256;

#[repr(C)]
#[derive(Clone, Copy, Default)]
struct BlkRequestHeader {
    request_type: u32,
    _ioprio: u32,
    sector: u64,
}

unsafe impl ByteValued for BlkRequestHeader {}

#[derive(Clone)]
struct BlockBackend {
    image: Arc<Mutex<File>>,
    config: Vec<u8>,
    mem: Option<GuestMemoryAtomic<GuestMemoryMmap>>,
    event_idx: bool,
}

impl BlockBackend {
    fn new(image: File, image_len: u64) -> Self {
        let mut config = vec![0u8; 96];
        config[0..8].copy_from_slice(&(image_len / SECTOR_SIZE).to_le_bytes());
        config[20..24].copy_from_slice(&(SECTOR_SIZE as u32).to_le_bytes());

        Self {
            image: Arc::new(Mutex::new(image)),
            config,
            mem: None,
            event_idx: false,
        }
    }

    fn process_queue(&mut self, vring: &VringRwLock) -> io::Result<()> {
        let mut used_any = false;
        let mem_atomic = self
            .mem
            .as_ref()
            .ok_or_else(|| io::Error::new(io::ErrorKind::Other, "guest memory not configured"))?
            .clone();

        loop {
            vring.disable_notification().map_err(queue_err)?;

            {
                let mem = mem_atomic.memory();
                let mut guard = vring.get_mut();
                loop {
                    let chain = {
                        let mut iter = guard.get_queue_mut().iter(mem.deref()).map_err(queue_err)?;
                        iter.next()
                    };

                    let Some(chain) = chain else {
                        break;
                    };

                    let head = chain.head_index();
                    let used_len = match self.process_request(mem.deref(), chain) {
                        Ok(len) => len,
                        Err(e) => {
                            eprintln!("vhost-blk: request failed: {e}");
                            1
                        }
                    };

                    guard.add_used(head, used_len).map_err(queue_err)?;
                    used_any = true;
                }
            }

            if !vring.enable_notification().map_err(queue_err)? {
                break;
            }
        }

        if used_any && vring.needs_notification().map_err(queue_err)? {
            vring.signal_used_queue()?;
        }

        Ok(())
    }

    fn process_request(
        &mut self,
        mem: &GuestMemoryMmap,
        chain: virtio_queue::DescriptorChain<&GuestMemoryMmap>,
    ) -> io::Result<u32> {
        let mut reader = chain.clone().reader::<()>(mem).map_err(queue_err)?;
        let mut writer = chain.writer::<()>(mem).map_err(queue_err)?;

        let header: BlkRequestHeader = reader.read_obj()?;
        let mut status = VIRTIO_BLK_S_OK as u8;
        let mut used_len = 1u32;

        let offset = header
            .sector
            .checked_mul(SECTOR_SIZE)
            .ok_or_else(|| io::Error::new(io::ErrorKind::InvalidInput, "sector offset overflow"))?;

        match header.request_type {
            VIRTIO_BLK_T_IN => {
                let len = writer.available_bytes().saturating_sub(1);
                let mut buf = vec![0u8; len];
                if let Err(e) = self.image.lock().unwrap().read_exact_at(&mut buf, offset) {
                    status = VIRTIO_BLK_S_IOERR as u8;
                    if e.kind() != io::ErrorKind::UnexpectedEof {
                        eprintln!("vhost-blk: read failed at {offset}: {e}");
                    }
                } else {
                    writer.write_all(&buf)?;
                    used_len += len as u32;
                }
            }
            VIRTIO_BLK_T_OUT => {
                let len = reader.available_bytes();
                let mut buf = vec![0u8; len];
                reader.read_exact(&mut buf)?;
                if let Err(e) = self.image.lock().unwrap().write_all_at(&buf, offset) {
                    status = VIRTIO_BLK_S_IOERR as u8;
                    eprintln!("vhost-blk: write failed at {offset}: {e}");
                }
            }
            VIRTIO_BLK_T_FLUSH => {
                if let Err(e) = self.image.lock().unwrap().sync_all() {
                    status = VIRTIO_BLK_S_IOERR as u8;
                    eprintln!("vhost-blk: flush failed: {e}");
                }
            }
            VIRTIO_BLK_T_GET_ID => {
                let id = b"rust-vmm-vhost-blk\0";
                let len = writer.available_bytes().saturating_sub(1).min(id.len());
                writer.write_all(&id[..len])?;
                used_len += len as u32;
            }
            _ => {
                status = VIRTIO_BLK_S_UNSUPP as u8;
            }
        }

        writer.write_all(&[status])?;
        Ok(used_len)
    }
}

impl VhostUserBackendMut for BlockBackend {
    type Bitmap = ();
    type Vring = VringRwLock;

    fn num_queues(&self) -> usize {
        1
    }

    fn max_queue_size(&self) -> usize {
        QUEUE_SIZE
    }

    fn features(&self) -> u64 {
        (1u64 << VIRTIO_F_VERSION_1)
            | (1u64 << VIRTIO_BLK_F_BLK_SIZE)
            | (1u64 << VIRTIO_BLK_F_FLUSH)
            | VhostUserVirtioFeatures::PROTOCOL_FEATURES.bits()
    }

    fn protocol_features(&self) -> VhostUserProtocolFeatures {
        VhostUserProtocolFeatures::MQ | VhostUserProtocolFeatures::CONFIG
    }

    fn set_event_idx(&mut self, enabled: bool) {
        self.event_idx = enabled;
    }

    fn get_config(&self, offset: u32, size: u32) -> Vec<u8> {
        let offset = offset as usize;
        let size = size as usize;
        let mut out = vec![0u8; size];
        if offset >= self.config.len() {
            return out;
        }
        let end = self.config.len().min(offset + size);
        out[..end - offset].copy_from_slice(&self.config[offset..end]);
        out
    }

    fn update_memory(&mut self, mem: GuestMemoryAtomic<GuestMemoryMmap>) -> io::Result<()> {
        self.mem = Some(mem);
        Ok(())
    }

    fn handle_event(
        &mut self,
        _device_event: u16,
        evset: EventSet,
        vrings: &[VringRwLock],
        _thread_id: usize,
    ) -> io::Result<()> {
        if !evset.contains(EventSet::IN) {
            return Ok(());
        }

        if let Some(vring) = vrings.first() {
            if !vring.get_ref().is_enabled() || !vring.read_kick()? {
                return Ok(());
            }
            self.process_queue(vring)?;
        }

        Ok(())
    }
}

fn queue_err<E: std::fmt::Display>(e: E) -> io::Error {
    io::Error::new(io::ErrorKind::Other, e.to_string())
}

fn usage(program: &str) -> ! {
    eprintln!("usage: {program} --socket <path> --image <path> [--readonly]");
    std::process::exit(2);
}

fn main() -> io::Result<()> {
    let mut args = env::args();
    let program = args.next().unwrap_or_else(|| "vhost-blk".to_string());
    let mut socket = None::<PathBuf>;
    let mut image = None::<PathBuf>;
    let mut readonly = false;

    while let Some(arg) = args.next() {
        match arg.as_str() {
            "--socket" => socket = args.next().map(PathBuf::from),
            "--image" => image = args.next().map(PathBuf::from),
            "--readonly" => readonly = true,
            "--help" | "-h" => usage(&program),
            _ => usage(&program),
        }
    }

    let socket = socket.unwrap_or_else(|| usage(&program));
    let image = image.unwrap_or_else(|| usage(&program));

    let image_file = OpenOptions::new()
        .read(true)
        .write(!readonly)
        .open(&image)?;
    let image_len = image_file.metadata()?.len();
    if image_len < SECTOR_SIZE || image_len % SECTOR_SIZE != 0 {
        return Err(io::Error::new(
            io::ErrorKind::InvalidInput,
            "image size must be a non-zero multiple of 512 bytes",
        ));
    }

    if socket.exists() {
        std::fs::remove_file(&socket)?;
    }

    let mem = GuestMemoryAtomic::new(GuestMemoryMmap::new());
    let backend = Arc::new(RwLock::new(BlockBackend::new(image_file, image_len)));
    let mut daemon = VhostUserDaemon::new("vhost-blk".to_string(), backend, mem)
        .map_err(queue_err)?;

    println!(
        "vhost-blk: serving {} sectors from {} on {}",
        image_len / SECTOR_SIZE,
        image.display(),
        socket.display()
    );
    daemon.serve(&socket).map_err(queue_err)
}
