//! The ioctl command encoding of illumos, from `sys/ioccom.h`.
//!
//! The libdrm headers define `DRM_IOWR(nr, type)` as
//! `_IOWR('d', nr, type)`, and on illumos `_IOWR` is the macro from
//! `sys/ioccom.h`: direction bits at the top, the argument size masked to
//! eight bits (`IOCPARM_MASK`) at bit 16, the group letter at bit 8 and the
//! number in the low byte. The functions below are the `const` equivalents,
//! the same as `ddi_sys::ioccom`, repeated here so that the crate has no
//! kernel dependency and can serve userland code as well.
//!
//! The size is truncated to eight bits by the encoding, so the kernel can
//! learn an argument size from the command number only for structures of
//! fewer than 256 bytes. Every structure of the DRM core and its mode
//! setting is smaller than that, and the `drm` crate takes the caller's
//! size from the command for those ioctls as Linux's `drm_ioctl()` does
//! (see `drm::ioctl`); the `drm` crate's tests check the bound.

/// The group letter of every DRM ioctl (`DRM_IOCTL_BASE`).
pub const DRM_IOCTL_BASE: u8 = b'd';

/// The mask applied to the argument size (`IOCPARM_MASK`).
pub const IOCPARM_MASK: u32 = 0xff;
/// No argument (`IOC_VOID`).
pub const IOC_VOID: u32 = 0x2000_0000;
/// The kernel copies the argument out (`IOC_OUT`).
pub const IOC_OUT: u32 = 0x4000_0000;
/// The kernel copies the argument in (`IOC_IN`).
pub const IOC_IN: u32 = 0x8000_0000;
/// Both directions (`IOC_INOUT`).
pub const IOC_INOUT: u32 = IOC_IN | IOC_OUT;
/// The direction bits of a command (`IOC_VOID`, `IOC_OUT`, `IOC_IN`).
pub const IOC_DIRMASK: u32 = IOC_VOID | IOC_OUT | IOC_IN;

const fn encode(dir: u32, nr: u8, size: usize) -> u32 {
    dir | (((size as u32) & IOCPARM_MASK) << 16) | ((DRM_IOCTL_BASE as u32) << 8) | nr as u32
}

/// `DRM_IO(nr)`, that is `_IO('d', nr)`.
pub const fn drm_io(nr: u8) -> u32 {
    encode(IOC_VOID, nr, 0)
}
/// `DRM_IOR(nr, T)`, that is `_IOR('d', nr, T)`.
pub const fn drm_ior<T>(nr: u8) -> u32 {
    encode(IOC_OUT, nr, core::mem::size_of::<T>())
}
/// `DRM_IOW(nr, T)`, that is `_IOW('d', nr, T)`.
pub const fn drm_iow<T>(nr: u8) -> u32 {
    encode(IOC_IN, nr, core::mem::size_of::<T>())
}
/// `DRM_IOWR(nr, T)`, that is `_IOWR('d', nr, T)`.
pub const fn drm_iowr<T>(nr: u8) -> u32 {
    encode(IOC_INOUT, nr, core::mem::size_of::<T>())
}

/// The ioctl number (the low byte) of a command.
pub const fn ioc_nr(cmd: u32) -> u8 {
    (cmd & 0xff) as u8
}
/// The group letter of a command.
pub const fn ioc_group(cmd: u32) -> u8 {
    ((cmd >> 8) & 0xff) as u8
}
/// The argument size a command carries (`(cmd >> 16) & IOCPARM_MASK`): the
/// size of the caller's structure for structures below 256 bytes.
pub const fn ioc_size(cmd: u32) -> usize {
    ((cmd >> 16) & IOCPARM_MASK) as usize
}
/// The direction bits of a command.
pub const fn ioc_dir(cmd: u32) -> u32 {
    cmd & IOC_DIRMASK
}
/// Returns `cmd` with its size bits replaced by `size` (masked as the C
/// macros mask it), which is how a caller with an older or newer structure
/// encodes the same command.
pub const fn with_size(cmd: u32, size: usize) -> u32 {
    (cmd & !(IOCPARM_MASK << 16)) | (((size as u32) & IOCPARM_MASK) << 16)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn matches_sys_ioccom() {
        // _IO('d', 0x1e) and _IOWR('d', 0x00, struct drm_version) (64 bytes),
        // worked out by hand from sys/ioccom.h. `cargo xtask uapi-check`
        // compares every generated command with gcc on illumos.
        assert_eq!(drm_io(0x1e), 0x2000_641e);
        assert_eq!(drm_iowr::<[u8; 64]>(0x00), 0xc040_6400);
        // Sizes above 255 wrap, as with the C macro.
        assert_eq!(drm_iowr::<[u8; 0x104]>(0x01), 0xc004_6401);
        assert_eq!(ioc_nr(0xc040_6400), 0);
        assert_eq!(ioc_group(0xc040_6400), b'd');
        assert_eq!(ioc_size(0xc040_6400), 64);
        assert_eq!(ioc_dir(0xc040_6400), IOC_INOUT);
        assert_eq!(ioc_dir(drm_io(0x1e)), IOC_VOID);
        assert_eq!(with_size(0xc040_6400, 24), 0xc018_6400);
        assert_eq!(with_size(0xc040_6400, 0x104), 0xc004_6400);
    }
}
