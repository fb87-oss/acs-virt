use std::io;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum RamAccessMode {
    SharedMem,
    QemuMediated,
}

impl RamAccessMode {
    pub fn as_str(self) -> &'static str {
        match self {
            Self::SharedMem => "shared-mem",
            Self::QemuMediated => "qemu-mediated",
        }
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[repr(u16)]
pub enum MessageKind {
    Hello = 1,
    MemRegion = 2,
    MmioRead = 3,
    MmioReadReply = 4,
    MmioWrite = 5,
    IrqAssert = 6,
    IrqDeassert = 7,
    DmaRead = 8,
    DmaReadReply = 9,
    DmaWrite = 10,
    Error = 0xffff,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct MessageHeader {
    pub kind: MessageKind,
    pub flags: u16,
    pub window_id: u32,
    pub offset: u64,
    pub length: u32,
}

pub trait GuestMemoryAccess {
    fn read(&self, gpa: u64, buf: &mut [u8]) -> io::Result<()>;
    fn write(&self, gpa: u64, buf: &[u8]) -> io::Result<()>;
}

#[cfg(feature = "shared-mem")]
pub struct SharedMemGuestMemory;

#[cfg(feature = "shared-mem")]
impl GuestMemoryAccess for SharedMemGuestMemory {
    fn read(&self, _gpa: u64, _buf: &mut [u8]) -> io::Result<()> {
        Err(io::Error::new(
            io::ErrorKind::Unsupported,
            "shared memory mapping is not wired yet",
        ))
    }

    fn write(&self, _gpa: u64, _buf: &[u8]) -> io::Result<()> {
        Err(io::Error::new(
            io::ErrorKind::Unsupported,
            "shared memory mapping is not wired yet",
        ))
    }
}

#[cfg(feature = "qemu-mediated")]
pub struct QemuMediatedGuestMemory;

#[cfg(feature = "qemu-mediated")]
impl GuestMemoryAccess for QemuMediatedGuestMemory {
    fn read(&self, _gpa: u64, _buf: &mut [u8]) -> io::Result<()> {
        Err(io::Error::new(
            io::ErrorKind::Unsupported,
            "QEMU-mediated DMA_READ is not wired yet",
        ))
    }

    fn write(&self, _gpa: u64, _buf: &[u8]) -> io::Result<()> {
        Err(io::Error::new(
            io::ErrorKind::Unsupported,
            "QEMU-mediated DMA_WRITE is not wired yet",
        ))
    }
}
