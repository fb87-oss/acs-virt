use std::fs::{self, File};
use std::io::{self, Read, Write};
use std::os::unix::fs::FileExt;
use std::os::unix::net::{UnixListener, UnixStream};
use std::path::PathBuf;

use serde::Deserialize;

const MSG_MMIO_READ: u16 = 3;
const MSG_MMIO_READ_REPLY: u16 = 4;
const MSG_MMIO_WRITE: u16 = 5;
const MSG_IRQ_ASSERT: u16 = 6;
const MSG_DMA_READ: u16 = 8;
const MSG_DMA_READ_REPLY: u16 = 9;
const MSG_DMA_WRITE: u16 = 10;
const MSG_ERROR: u16 = 0xffff;
const HEADER_LEN: usize = 24;

const VIRTIO_MAGIC: u32 = 0x7472_6976;
const VIRTIO_VERSION: u32 = 2;
const VIRTIO_DEVICE_ID_BLOCK: u32 = 2;
const VIRTIO_VENDOR_ID: u32 = 0x4348_4950;
const QUEUE_SIZE: u32 = 256;
const SECTOR_SIZE: u64 = 512;
const VIRTQ_DESC_F_WRITE: u16 = 2;
const VIRTIO_BLK_T_IN: u32 = 0;
const VIRTIO_BLK_T_OUT: u32 = 1;
const VIRTIO_BLK_T_FLUSH: u32 = 4;
const VIRTIO_BLK_S_OK: u8 = 0;
const VIRTIO_BLK_S_IOERR: u8 = 1;
const VIRTIO_BLK_S_UNSUPP: u8 = 2;

#[derive(Debug, Deserialize)]
struct BackendConfig {
    block: BlockConfig,
    transport: TransportConfig,
}

#[derive(Debug, Deserialize)]
struct BlockConfig {
    image: String,
    readonly: Option<bool>,
}

#[derive(Debug, Deserialize)]
struct TransportConfig {
    qemu_mmio: QemuMmioConfig,
}

#[derive(Debug, Deserialize)]
struct QemuMmioConfig {
    socket: String,
    ram_access: Option<String>,
}

#[derive(Debug, Clone, Copy)]
struct Header {
    kind: u16,
    flags: u16,
    window_id: u32,
    offset: u64,
    length: u32,
}

#[derive(Debug)]
struct VirtioMmioBlock {
    image: File,
    capacity_sectors: u64,
    device_features_sel: u32,
    driver_features_sel: u32,
    driver_features: [u32; 2],
    queue_sel: u32,
    queue_num: u32,
    queue_ready: u32,
    queue_desc: u64,
    queue_driver: u64,
    queue_device: u64,
    last_avail_idx: u16,
    status: u32,
    interrupt_status: u32,
}

impl VirtioMmioBlock {
    fn new(image: File, image_len: u64) -> Self {
        Self {
            image,
            capacity_sectors: image_len / SECTOR_SIZE,
            device_features_sel: 0,
            driver_features_sel: 0,
            driver_features: [0; 2],
            queue_sel: 0,
            queue_num: QUEUE_SIZE,
            queue_ready: 0,
            queue_desc: 0,
            queue_driver: 0,
            queue_device: 0,
            last_avail_idx: 0,
            status: 0,
            interrupt_status: 0,
        }
    }

    fn read(&self, offset: u64, len: u32) -> u64 {
        let value = match offset {
            0x000 => VIRTIO_MAGIC,
            0x004 => VIRTIO_VERSION,
            0x008 => VIRTIO_DEVICE_ID_BLOCK,
            0x00c => VIRTIO_VENDOR_ID,
            0x010 => self.device_features(),
            0x014 => self.device_features_sel,
            0x020 => self.driver_features[self.driver_features_sel.min(1) as usize],
            0x024 => self.driver_features_sel,
            0x030 => self.queue_sel,
            0x034 => QUEUE_SIZE,
            0x038 => self.queue_num,
            0x044 => self.queue_ready,
            0x060 => self.interrupt_status,
            0x070 => self.status,
            0x0fc => 0,
            0x100 => self.capacity_sectors as u32,
            0x104 => (self.capacity_sectors >> 32) as u32,
            0x114 => SECTOR_SIZE as u32,
            _ => 0,
        } as u64;

        match len {
            1 => value & 0xff,
            2 => value & 0xffff,
            4 => value & 0xffff_ffff,
            _ => value,
        }
    }

    fn write(&mut self, offset: u64, value: u64) {
        let value = value as u32;
        match offset {
            0x014 => self.device_features_sel = value,
            0x020 => self.driver_features[self.driver_features_sel.min(1) as usize] = value,
            0x024 => self.driver_features_sel = value,
            0x030 => self.queue_sel = value,
            0x038 => self.queue_num = value,
            0x044 => self.queue_ready = value,
            0x080 => self.queue_desc = (self.queue_desc & !0xffff_ffff) | value as u64,
            0x084 => self.queue_desc = (self.queue_desc & 0xffff_ffff) | ((value as u64) << 32),
            0x090 => self.queue_driver = (self.queue_driver & !0xffff_ffff) | value as u64,
            0x094 => self.queue_driver = (self.queue_driver & 0xffff_ffff) | ((value as u64) << 32),
            0x0a0 => self.queue_device = (self.queue_device & !0xffff_ffff) | value as u64,
            0x0a4 => self.queue_device = (self.queue_device & 0xffff_ffff) | ((value as u64) << 32),
            0x064 => self.interrupt_status &= !value,
            0x070 => self.status = value,
            _ => {}
        }
    }

    fn device_features(&self) -> u32 {
        match self.device_features_sel {
            0 => (1 << 6) | (1 << 9),
            1 => 1,
            _ => 0,
        }
    }

    fn notify_queue(&mut self, stream: &mut UnixStream, queue: u32) -> io::Result<()> {
        eprintln!("virtio-mmio-backend: notify queue={queue}");
        if queue != 0 || self.queue_ready == 0 {
            return Ok(());
        }

        let mut used_any = false;
        loop {
            let avail_idx = dma_read_u16(stream, self.queue_driver + 2)?;
            if self.last_avail_idx == avail_idx {
                break;
            }

            let ring_off = 4 + (u64::from(self.last_avail_idx % self.queue_num as u16) * 2);
            let head = dma_read_u16(stream, self.queue_driver + ring_off)?;
            eprintln!("virtio-mmio-backend: process head={head}");
            let used_len = self.process_chain(stream, head)?;
            self.add_used(stream, head, used_len)?;
            self.last_avail_idx = self.last_avail_idx.wrapping_add(1);
            used_any = true;
        }

        if used_any {
            self.interrupt_status |= 1;
            write_header(stream, Header { kind: MSG_IRQ_ASSERT, flags: 0, window_id: 0, offset: 0, length: 0 })?;
        }

        Ok(())
    }

    fn process_chain(&mut self, stream: &mut UnixStream, head: u16) -> io::Result<u32> {
        eprintln!("virtio-mmio-backend: read descriptor chain head={head}");
        let header_desc = self.read_desc(stream, head)?;
        let data_desc = self.read_desc(stream, header_desc.next)?;
        let status_desc = self.read_desc(stream, data_desc.next)?;
        let header = dma_read(stream, header_desc.addr, 16)?;
        let request_type = u32::from_le_bytes(header[0..4].try_into().unwrap());
        let sector = u64::from_le_bytes(header[8..16].try_into().unwrap());
        eprintln!(
            "virtio-mmio-backend: request type={request_type} sector={sector} data_len={}",
            data_desc.len
        );
        let offset = sector.checked_mul(SECTOR_SIZE).ok_or_else(|| {
            io::Error::new(io::ErrorKind::InvalidInput, "sector offset overflow")
        })?;
        let mut status = VIRTIO_BLK_S_OK;
        let mut used_len = 1u32;

        match request_type {
            VIRTIO_BLK_T_IN => {
                if data_desc.flags & VIRTQ_DESC_F_WRITE == 0 {
                    status = VIRTIO_BLK_S_IOERR;
                } else {
                    let mut data = vec![0u8; data_desc.len as usize];
                    if self.image.read_exact_at(&mut data, offset).is_ok() {
                        dma_write(stream, data_desc.addr, &data)?;
                        used_len = used_len.saturating_add(data_desc.len);
                    } else {
                        status = VIRTIO_BLK_S_IOERR;
                    }
                }
            }
            VIRTIO_BLK_T_OUT => {
                if data_desc.flags & VIRTQ_DESC_F_WRITE != 0 {
                    status = VIRTIO_BLK_S_IOERR;
                } else {
                    let data = dma_read(stream, data_desc.addr, data_desc.len)?;
                    if self.image.write_all_at(&data, offset).is_err() {
                        status = VIRTIO_BLK_S_IOERR;
                    }
                }
            }
            VIRTIO_BLK_T_FLUSH => {
                if self.image.sync_all().is_err() {
                    status = VIRTIO_BLK_S_IOERR;
                }
            }
            _ => status = VIRTIO_BLK_S_UNSUPP,
        }

        dma_write(stream, status_desc.addr, &[status])?;
        Ok(used_len)
    }

    fn read_desc(&self, stream: &mut UnixStream, index: u16) -> io::Result<Descriptor> {
        let bytes = dma_read(stream, self.queue_desc + (u64::from(index) * 16), 16)?;
        Ok(Descriptor {
            addr: u64::from_le_bytes(bytes[0..8].try_into().unwrap()),
            len: u32::from_le_bytes(bytes[8..12].try_into().unwrap()),
            flags: u16::from_le_bytes(bytes[12..14].try_into().unwrap()),
            next: u16::from_le_bytes(bytes[14..16].try_into().unwrap()),
        })
    }

    fn add_used(&self, stream: &mut UnixStream, head: u16, len: u32) -> io::Result<()> {
        let used_idx = dma_read_u16(stream, self.queue_device + 2)?;
        let elem = self.queue_device + 4 + (u64::from(used_idx % self.queue_num as u16) * 8);
        dma_write(stream, elem, &(head as u32).to_le_bytes())?;
        dma_write(stream, elem + 4, &len.to_le_bytes())?;
        dma_write(stream, self.queue_device + 2, &used_idx.wrapping_add(1).to_le_bytes())
    }
}

#[derive(Debug)]
struct Descriptor {
    addr: u64,
    len: u32,
    flags: u16,
    next: u16,
}

fn main() -> io::Result<()> {
    let config_path = std::env::args_os()
        .nth(1)
        .map(PathBuf::from)
        .ok_or_else(|| io::Error::new(io::ErrorKind::InvalidInput, "usage: virtio-mmio-backend <backend.toml>"))?;
    let config_text = fs::read_to_string(&config_path)?;
    let config: BackendConfig = toml::from_str(&config_text)
        .map_err(|e| io::Error::new(io::ErrorKind::InvalidInput, e.to_string()))?;
    let image = fs::OpenOptions::new()
        .read(true)
        .write(!config.block.readonly.unwrap_or(false))
        .open(&config.block.image)?;
    let image_len = image.metadata()?.len();
    let socket = PathBuf::from(&config.transport.qemu_mmio.socket);
    if socket.exists() {
        fs::remove_file(&socket)?;
    }
    if let Some(parent) = socket.parent() {
        fs::create_dir_all(parent)?;
    }

    let listener = UnixListener::bind(&socket)?;
    eprintln!(
        "virtio-mmio-backend: serving {} sectors on {} ({})",
        image_len / SECTOR_SIZE,
        socket.display(),
        config
            .transport
            .qemu_mmio
            .ram_access
            .as_deref()
            .unwrap_or("shared-mem")
    );

    for stream in listener.incoming() {
        eprintln!("virtio-mmio-backend: accepted QEMU connection");
        let mut device = VirtioMmioBlock::new(image.try_clone()?, image_len);
        if let Err(e) = serve(stream?, &mut device) {
            eprintln!("virtio-mmio-backend: connection closed: {e}");
        }
    }

    Ok(())
}

fn serve(mut stream: UnixStream, device: &mut VirtioMmioBlock) -> io::Result<()> {
    loop {
        let header = read_header(&mut stream)?;
        match header.kind {
            MSG_MMIO_READ => {
                let value = device.read(header.offset, header.length);
                eprintln!(
                    "virtio-mmio-backend: read offset=0x{:x} len={} -> 0x{:x}",
                    header.offset, header.length, value
                );
                write_read_reply(&mut stream, header, value)?;
            }
            MSG_MMIO_WRITE => {
                let value = read_value(&mut stream, header.length)?;
                eprintln!(
                    "virtio-mmio-backend: write offset=0x{:x} len={} value=0x{:x}",
                    header.offset, header.length, value
                );
                device.write(header.offset, value);
                if header.offset == 0x050 {
                    device.notify_queue(&mut stream, value as u32)?;
                }
                write_header(&mut stream, Header { kind: MSG_ERROR, flags: 0, window_id: 0, offset: 0, length: 0 })?;
            }
            _ => {
                return Err(io::Error::new(
                    io::ErrorKind::InvalidData,
                    format!("unsupported message kind {}", header.kind),
                ));
            }
        }
    }
}

fn dma_read(stream: &mut UnixStream, gpa: u64, len: u32) -> io::Result<Vec<u8>> {
    write_header(stream, Header { kind: MSG_DMA_READ, flags: 0, window_id: 0, offset: gpa, length: len })?;
    let reply = read_header(stream)?;
    if reply.kind != MSG_DMA_READ_REPLY || reply.length != len {
        return Err(io::Error::new(io::ErrorKind::InvalidData, "invalid DMA_READ reply"));
    }
    let mut data = vec![0u8; len as usize];
    stream.read_exact(&mut data)?;
    Ok(data)
}

fn dma_read_u16(stream: &mut UnixStream, gpa: u64) -> io::Result<u16> {
    let bytes = dma_read(stream, gpa, 2)?;
    Ok(u16::from_le_bytes([bytes[0], bytes[1]]))
}

fn dma_write(stream: &mut UnixStream, gpa: u64, data: &[u8]) -> io::Result<()> {
    write_header(stream, Header { kind: MSG_DMA_WRITE, flags: 0, window_id: 0, offset: gpa, length: data.len() as u32 })?;
    stream.write_all(data)?;
    let reply = read_header(stream)?;
    if reply.kind != MSG_ERROR {
        return Err(io::Error::new(io::ErrorKind::InvalidData, "invalid DMA_WRITE reply"));
    }
    Ok(())
}

fn read_header(stream: &mut UnixStream) -> io::Result<Header> {
    let mut buf = [0u8; HEADER_LEN];
    stream.read_exact(&mut buf)?;
    Ok(Header {
        kind: u16::from_le_bytes([buf[0], buf[1]]),
        flags: u16::from_le_bytes([buf[2], buf[3]]),
        window_id: u32::from_le_bytes([buf[4], buf[5], buf[6], buf[7]]),
        offset: u64::from_le_bytes([
            buf[8], buf[9], buf[10], buf[11], buf[12], buf[13], buf[14], buf[15],
        ]),
        length: u32::from_le_bytes([buf[16], buf[17], buf[18], buf[19]]),
    })
}

fn write_read_reply(stream: &mut UnixStream, request: Header, value: u64) -> io::Result<()> {
    let len = request.length.min(8);
    let reply = Header {
        kind: MSG_MMIO_READ_REPLY,
        flags: request.flags,
        window_id: request.window_id,
        offset: request.offset,
        length: len,
    };
    write_header(stream, reply)?;
    stream.write_all(&value.to_le_bytes()[..len as usize])
}

fn write_header(stream: &mut UnixStream, header: Header) -> io::Result<()> {
    let mut buf = [0u8; HEADER_LEN];
    buf[0..2].copy_from_slice(&header.kind.to_le_bytes());
    buf[2..4].copy_from_slice(&header.flags.to_le_bytes());
    buf[4..8].copy_from_slice(&header.window_id.to_le_bytes());
    buf[8..16].copy_from_slice(&header.offset.to_le_bytes());
    buf[16..20].copy_from_slice(&header.length.to_le_bytes());
    stream.write_all(&buf)
}

fn read_value(stream: &mut UnixStream, len: u32) -> io::Result<u64> {
    if len > 8 {
        return Err(io::Error::new(
            io::ErrorKind::InvalidData,
            format!("invalid write size {len}"),
        ));
    }
    let mut bytes = [0u8; 8];
    stream.read_exact(&mut bytes[..len as usize])?;
    Ok(u64::from_le_bytes(bytes))
}
