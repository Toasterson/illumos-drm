// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//! The virtio-gpu device protocol, from the virtio 1.2 specification,
//! section 5.7 ("GPU Device").
//!
//! Every request starts with a [`CtrlHdr`]; the device answers with a
//! header of its own (and a payload for the `GET_*` requests). All fields
//! are little-endian, which is the byte order of the only target
//! (`x86_64`), so the structures are written as they are. They are
//! `#[repr(C)]`, contain only `u32` and `u64` fields in an order without
//! padding, and are copied into DMA memory byte by byte ([`bytes_of`]).

use core::mem::size_of;

const _: () = assert!(cfg!(target_endian = "little"), "virtio-gpu structures are little-endian");

// --- device features (5.7.3) ---

/// 3D (virgl) mode is supported.
pub const F_VIRGL: u32 = 0;
/// `GET_EDID` is supported.
pub const F_EDID: u32 = 1;
/// Resources can be assigned UUIDs (cross-device sharing).
pub const F_RESOURCE_UUID: u32 = 2;
/// Blob resources are supported.
pub const F_RESOURCE_BLOB: u32 = 3;
/// Contexts can be created with a capability set and rings.
pub const F_CONTEXT_INIT: u32 = 4;

// --- configuration (5.7.4) ---

/// `events_read`: pending events.
pub const CFG_EVENTS_READ: usize = 0;
/// `events_clear`: write 1 bits to clear events.
pub const CFG_EVENTS_CLEAR: usize = 4;
/// `num_scanouts`: 1 to 16.
pub const CFG_NUM_SCANOUTS: usize = 8;
/// `num_capsets`.
pub const CFG_NUM_CAPSETS: usize = 12;
/// The size of `struct virtio_gpu_config`.
pub const CFG_SIZE: usize = 16;
/// The display configuration changed; `GET_DISPLAY_INFO` again.
pub const EVENT_DISPLAY: u32 = 1 << 0;
/// The most scanouts a device has.
pub const MAX_SCANOUTS: usize = 16;

// --- queues ---

/// The control queue.
pub const CONTROLQ: u16 = 0;
/// The cursor queue.
pub const CURSORQ: u16 = 1;

// --- command types (5.7.6.7) ---

/// `VIRTIO_GPU_CMD_GET_DISPLAY_INFO`.
pub const CMD_GET_DISPLAY_INFO: u32 = 0x0100;
/// `VIRTIO_GPU_CMD_RESOURCE_CREATE_2D`.
pub const CMD_RESOURCE_CREATE_2D: u32 = 0x0101;
/// `VIRTIO_GPU_CMD_RESOURCE_UNREF`.
pub const CMD_RESOURCE_UNREF: u32 = 0x0102;
/// `VIRTIO_GPU_CMD_SET_SCANOUT`.
pub const CMD_SET_SCANOUT: u32 = 0x0103;
/// `VIRTIO_GPU_CMD_RESOURCE_FLUSH`.
pub const CMD_RESOURCE_FLUSH: u32 = 0x0104;
/// `VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D`.
pub const CMD_TRANSFER_TO_HOST_2D: u32 = 0x0105;
/// `VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING`.
pub const CMD_RESOURCE_ATTACH_BACKING: u32 = 0x0106;
/// `VIRTIO_GPU_CMD_RESOURCE_DETACH_BACKING`.
pub const CMD_RESOURCE_DETACH_BACKING: u32 = 0x0107;
/// `VIRTIO_GPU_CMD_GET_CAPSET_INFO`.
pub const CMD_GET_CAPSET_INFO: u32 = 0x0108;
/// `VIRTIO_GPU_CMD_GET_CAPSET`.
pub const CMD_GET_CAPSET: u32 = 0x0109;
/// `VIRTIO_GPU_CMD_GET_EDID`.
pub const CMD_GET_EDID: u32 = 0x010a;
/// `VIRTIO_GPU_CMD_RESOURCE_CREATE_BLOB`.
pub const CMD_RESOURCE_CREATE_BLOB: u32 = 0x010c;
/// `VIRTIO_GPU_CMD_CTX_CREATE`.
pub const CMD_CTX_CREATE: u32 = 0x0200;
/// `VIRTIO_GPU_CMD_CTX_DESTROY`.
pub const CMD_CTX_DESTROY: u32 = 0x0201;
/// `VIRTIO_GPU_CMD_CTX_ATTACH_RESOURCE`.
pub const CMD_CTX_ATTACH_RESOURCE: u32 = 0x0202;
/// `VIRTIO_GPU_CMD_CTX_DETACH_RESOURCE`.
pub const CMD_CTX_DETACH_RESOURCE: u32 = 0x0203;
/// `VIRTIO_GPU_CMD_RESOURCE_CREATE_3D`.
pub const CMD_RESOURCE_CREATE_3D: u32 = 0x0204;
/// `VIRTIO_GPU_CMD_TRANSFER_TO_HOST_3D`.
pub const CMD_TRANSFER_TO_HOST_3D: u32 = 0x0205;
/// `VIRTIO_GPU_CMD_TRANSFER_FROM_HOST_3D`.
pub const CMD_TRANSFER_FROM_HOST_3D: u32 = 0x0206;
/// `VIRTIO_GPU_CMD_SUBMIT_3D`.
pub const CMD_SUBMIT_3D: u32 = 0x0207;
/// `VIRTIO_GPU_CMD_UPDATE_CURSOR`.
pub const CMD_UPDATE_CURSOR: u32 = 0x0300;
/// `VIRTIO_GPU_CMD_MOVE_CURSOR`.
pub const CMD_MOVE_CURSOR: u32 = 0x0301;

/// `VIRTIO_GPU_RESP_OK_NODATA`.
pub const RESP_OK_NODATA: u32 = 0x1100;
/// `VIRTIO_GPU_RESP_OK_DISPLAY_INFO`.
pub const RESP_OK_DISPLAY_INFO: u32 = 0x1101;
/// `VIRTIO_GPU_RESP_OK_CAPSET_INFO`.
pub const RESP_OK_CAPSET_INFO: u32 = 0x1102;
/// `VIRTIO_GPU_RESP_OK_CAPSET`.
pub const RESP_OK_CAPSET: u32 = 0x1103;
/// `VIRTIO_GPU_RESP_OK_EDID`.
pub const RESP_OK_EDID: u32 = 0x1104;
/// The first error response (`VIRTIO_GPU_RESP_ERR_UNSPEC`).
pub const RESP_ERR_UNSPEC: u32 = 0x1200;
/// `VIRTIO_GPU_RESP_ERR_OUT_OF_MEMORY`.
pub const RESP_ERR_OUT_OF_MEMORY: u32 = 0x1201;
/// `VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER`.
pub const RESP_ERR_INVALID_PARAMETER: u32 = 0x1205;

/// The request asks for a fence: the device answers once it has
/// completed the work.
pub const FLAG_FENCE: u32 = 1 << 0;
/// `ring_idx` names a ring of the context (`CONTEXT_INIT`).
pub const FLAG_INFO_RING_IDX: u32 = 1 << 1;

/// `VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM`: DRM's ARGB8888 on a little-endian
/// guest.
pub const FORMAT_B8G8R8A8_UNORM: u32 = 1;
/// `VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM`: DRM's XRGB8888.
pub const FORMAT_B8G8R8X8_UNORM: u32 = 2;
/// `VIRTIO_GPU_FORMAT_A8R8G8B8_UNORM`: DRM's BGRA8888.
pub const FORMAT_A8R8G8B8_UNORM: u32 = 3;
/// `VIRTIO_GPU_FORMAT_X8R8G8B8_UNORM`: DRM's BGRX8888.
pub const FORMAT_X8R8G8B8_UNORM: u32 = 4;

/// `VIRTIO_GPU_BLOB_MEM_GUEST`.
pub const BLOB_MEM_GUEST: u32 = 1;
/// `VIRTIO_GPU_BLOB_MEM_HOST3D`.
pub const BLOB_MEM_HOST3D: u32 = 2;
/// `VIRTIO_GPU_BLOB_MEM_HOST3D_GUEST`.
pub const BLOB_MEM_HOST3D_GUEST: u32 = 3;

/// `VIRTIO_GPU_CAPSET_VIRGL`.
pub const CAPSET_VIRGL: u32 = 1;
/// `VIRTIO_GPU_CAPSET_VIRGL2`.
pub const CAPSET_VIRGL2: u32 = 2;

/// `struct virtio_gpu_ctrl_hdr`.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct CtrlHdr {
    /// The command or response type.
    pub type_: u32,
    /// `FLAG_*`.
    pub flags: u32,
    /// With [`FLAG_FENCE`]: the fence the response completes.
    pub fence_id: u64,
    /// The 3D context.
    pub ctx_id: u32,
    /// With [`FLAG_INFO_RING_IDX`]: the context's ring.
    pub ring_idx: u8,
    /// Zero.
    pub padding: [u8; 3],
}

impl CtrlHdr {
    /// A header of type `t` without fence and context.
    pub const fn new(t: u32) -> CtrlHdr {
        CtrlHdr { type_: t, flags: 0, fence_id: 0, ctx_id: 0, ring_idx: 0, padding: [0; 3] }
    }
    /// A header of type `t` for context `ctx`.
    pub const fn ctx(t: u32, ctx: u32) -> CtrlHdr {
        CtrlHdr { type_: t, flags: 0, fence_id: 0, ctx_id: ctx, ring_idx: 0, padding: [0; 3] }
    }
}

/// `struct virtio_gpu_rect`.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct Rect {
    /// Left.
    pub x: u32,
    /// Top.
    pub y: u32,
    /// Width.
    pub width: u32,
    /// Height.
    pub height: u32,
}

/// One scanout of `struct virtio_gpu_resp_display_info`.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct DisplayOne {
    /// The preferred position and size.
    pub r: Rect,
    /// Non-zero if the scanout is enabled (a display is connected).
    pub enabled: u32,
    /// Flags.
    pub flags: u32,
}

/// `struct virtio_gpu_resp_display_info`.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct RespDisplayInfo {
    /// The response header.
    pub hdr: CtrlHdr,
    /// Every scanout.
    pub pmodes: [DisplayOne; MAX_SCANOUTS],
}

/// `struct virtio_gpu_get_edid`.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct GetEdid {
    /// The request header.
    pub hdr: CtrlHdr,
    /// The scanout.
    pub scanout: u32,
    /// Zero.
    pub padding: u32,
}

/// The size of the EDID buffer of `struct virtio_gpu_resp_edid`.
pub const EDID_MAX: usize = 1024;

/// `struct virtio_gpu_resp_edid`.
#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct RespEdid {
    /// The response header.
    pub hdr: CtrlHdr,
    /// The EDID size in bytes.
    pub size: u32,
    /// Zero.
    pub padding: u32,
    /// The EDID.
    pub edid: [u8; EDID_MAX],
}

/// `struct virtio_gpu_resource_create_2d`.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct ResourceCreate2d {
    /// The request header.
    pub hdr: CtrlHdr,
    /// The new resource.
    pub resource_id: u32,
    /// `FORMAT_*`.
    pub format: u32,
    /// Width in pixels.
    pub width: u32,
    /// Height in pixels.
    pub height: u32,
}

/// `struct virtio_gpu_resource_unref`, `struct virtio_gpu_resource_detach_backing`
/// and `struct virtio_gpu_ctx_resource` (the same layout).
#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct ResourceOnly {
    /// The request header.
    pub hdr: CtrlHdr,
    /// The resource.
    pub resource_id: u32,
    /// Zero.
    pub padding: u32,
}

/// `struct virtio_gpu_set_scanout`.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct SetScanout {
    /// The request header.
    pub hdr: CtrlHdr,
    /// The source rectangle in the resource.
    pub r: Rect,
    /// The scanout.
    pub scanout_id: u32,
    /// The resource, 0 to disable the scanout.
    pub resource_id: u32,
}

/// `struct virtio_gpu_resource_flush`.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct ResourceFlush {
    /// The request header.
    pub hdr: CtrlHdr,
    /// The rectangle to flush.
    pub r: Rect,
    /// The resource.
    pub resource_id: u32,
    /// Zero.
    pub padding: u32,
}

/// `struct virtio_gpu_transfer_to_host_2d`.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct TransferToHost2d {
    /// The request header.
    pub hdr: CtrlHdr,
    /// The rectangle to copy.
    pub r: Rect,
    /// The byte offset of the rectangle's first pixel in the backing.
    pub offset: u64,
    /// The resource.
    pub resource_id: u32,
    /// Zero.
    pub padding: u32,
}

/// `struct virtio_gpu_resource_attach_backing`, followed by `nr_entries`
/// [`MemEntry`]s.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct AttachBacking {
    /// The request header.
    pub hdr: CtrlHdr,
    /// The resource.
    pub resource_id: u32,
    /// The number of entries that follow.
    pub nr_entries: u32,
}

/// `struct virtio_gpu_mem_entry`.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct MemEntry {
    /// Guest physical (bus) address.
    pub addr: u64,
    /// Length in bytes.
    pub length: u32,
    /// Zero.
    pub padding: u32,
}

/// `struct virtio_gpu_get_capset_info`.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct GetCapsetInfo {
    /// The request header.
    pub hdr: CtrlHdr,
    /// The index, below `num_capsets`.
    pub capset_index: u32,
    /// Zero.
    pub padding: u32,
}

/// `struct virtio_gpu_resp_capset_info`.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct RespCapsetInfo {
    /// The response header.
    pub hdr: CtrlHdr,
    /// The capability set's id (`CAPSET_*`).
    pub capset_id: u32,
    /// The highest version.
    pub capset_max_version: u32,
    /// The largest size in bytes.
    pub capset_max_size: u32,
    /// Zero.
    pub padding: u32,
}

/// `struct virtio_gpu_get_capset`.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct GetCapset {
    /// The request header.
    pub hdr: CtrlHdr,
    /// The capability set's id.
    pub capset_id: u32,
    /// Its version.
    pub capset_version: u32,
}

/// `struct virtio_gpu_ctx_create`.
#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct CtxCreate {
    /// The request header, with the new context's id.
    pub hdr: CtrlHdr,
    /// The length of `debug_name`.
    pub nlen: u32,
    /// With [`F_CONTEXT_INIT`]: the capability set in the low 8 bits.
    pub context_init: u32,
    /// A name for the host's logs.
    pub debug_name: [u8; 64],
}

/// `struct virtio_gpu_resource_create_3d`.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct ResourceCreate3d {
    /// The request header.
    pub hdr: CtrlHdr,
    /// The new resource.
    pub resource_id: u32,
    /// The Gallium texture target.
    pub target: u32,
    /// The virgl format.
    pub format: u32,
    /// Gallium bind flags.
    pub bind: u32,
    /// Width.
    pub width: u32,
    /// Height.
    pub height: u32,
    /// Depth.
    pub depth: u32,
    /// Array size.
    pub array_size: u32,
    /// Last mipmap level.
    pub last_level: u32,
    /// Samples.
    pub nr_samples: u32,
    /// Flags.
    pub flags: u32,
    /// Zero.
    pub padding: u32,
}

/// `struct virtio_gpu_box`.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct Box3d {
    /// X.
    pub x: u32,
    /// Y.
    pub y: u32,
    /// Z.
    pub z: u32,
    /// Width.
    pub w: u32,
    /// Height.
    pub h: u32,
    /// Depth.
    pub d: u32,
}

/// `struct virtio_gpu_transfer_host_3d`.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct TransferHost3d {
    /// The request header.
    pub hdr: CtrlHdr,
    /// The box.
    pub box_: Box3d,
    /// The byte offset in the backing.
    pub offset: u64,
    /// The resource.
    pub resource_id: u32,
    /// The mipmap level.
    pub level: u32,
    /// The row stride, 0 for the resource's own.
    pub stride: u32,
    /// The layer stride, 0 for the resource's own.
    pub layer_stride: u32,
}

/// `struct virtio_gpu_cmd_submit`, followed by `size` bytes of commands.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct CmdSubmit {
    /// The request header.
    pub hdr: CtrlHdr,
    /// The size of the command stream.
    pub size: u32,
    /// Zero (in-fences are waited for by the guest).
    pub num_in_fences: u32,
}

/// `struct virtio_gpu_cursor_pos`.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct CursorPos {
    /// The scanout.
    pub scanout_id: u32,
    /// X.
    pub x: u32,
    /// Y.
    pub y: u32,
    /// Zero.
    pub padding: u32,
}

/// `struct virtio_gpu_update_cursor`.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct UpdateCursor {
    /// The request header.
    pub hdr: CtrlHdr,
    /// Where.
    pub pos: CursorPos,
    /// The cursor image, 0 to hide it (`UPDATE_CURSOR` only).
    pub resource_id: u32,
    /// Hotspot X.
    pub hot_x: u32,
    /// Hotspot Y.
    pub hot_y: u32,
    /// Zero.
    pub padding: u32,
}

/// `struct virtio_gpu_resource_create_blob`, followed by `nr_entries`
/// [`MemEntry`]s.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct ResourceCreateBlob {
    /// The request header.
    pub hdr: CtrlHdr,
    /// The new resource.
    pub resource_id: u32,
    /// `BLOB_MEM_*`.
    pub blob_mem: u32,
    /// `VIRTGPU_BLOB_FLAG_*`.
    pub blob_flags: u32,
    /// The number of entries that follow.
    pub nr_entries: u32,
    /// The host object (3D blobs).
    pub blob_id: u64,
    /// The size in bytes.
    pub size: u64,
}

/// A request or response structure: `#[repr(C)]` of integer fields
/// without padding (asserted below), so its bytes are its value.
///
/// # Safety
/// The type must have no padding and every bit pattern must be valid.
pub unsafe trait Wire: Copy {}

// SAFETY: integer fields only, laid out without padding (asserted below).
unsafe impl Wire for CtrlHdr {}
// SAFETY: as above.
unsafe impl Wire for Rect {}
// SAFETY: as above.
unsafe impl Wire for DisplayOne {}
// SAFETY: as above.
unsafe impl Wire for RespDisplayInfo {}
// SAFETY: as above.
unsafe impl Wire for GetEdid {}
// SAFETY: as above.
unsafe impl Wire for RespEdid {}
// SAFETY: as above.
unsafe impl Wire for ResourceCreate2d {}
// SAFETY: as above.
unsafe impl Wire for ResourceOnly {}
// SAFETY: as above.
unsafe impl Wire for SetScanout {}
// SAFETY: as above.
unsafe impl Wire for ResourceFlush {}
// SAFETY: as above.
unsafe impl Wire for TransferToHost2d {}
// SAFETY: as above.
unsafe impl Wire for AttachBacking {}
// SAFETY: as above.
unsafe impl Wire for MemEntry {}
// SAFETY: as above.
unsafe impl Wire for GetCapsetInfo {}
// SAFETY: as above.
unsafe impl Wire for RespCapsetInfo {}
// SAFETY: as above.
unsafe impl Wire for GetCapset {}
// SAFETY: as above.
unsafe impl Wire for CtxCreate {}
// SAFETY: as above.
unsafe impl Wire for ResourceCreate3d {}
// SAFETY: as above.
unsafe impl Wire for Box3d {}
// SAFETY: as above.
unsafe impl Wire for TransferHost3d {}
// SAFETY: as above.
unsafe impl Wire for CmdSubmit {}
// SAFETY: as above.
unsafe impl Wire for CursorPos {}
// SAFETY: as above.
unsafe impl Wire for UpdateCursor {}
// SAFETY: as above.
unsafe impl Wire for ResourceCreateBlob {}

// The sizes of the specification's structures.
const _: () = {
    assert!(size_of::<CtrlHdr>() == 24);
    assert!(size_of::<Rect>() == 16);
    assert!(size_of::<DisplayOne>() == 24);
    assert!(size_of::<RespDisplayInfo>() == 24 + 24 * MAX_SCANOUTS);
    assert!(size_of::<GetEdid>() == 32);
    assert!(size_of::<RespEdid>() == 32 + EDID_MAX);
    assert!(size_of::<ResourceCreate2d>() == 40);
    assert!(size_of::<ResourceOnly>() == 32);
    assert!(size_of::<SetScanout>() == 48);
    assert!(size_of::<ResourceFlush>() == 48);
    assert!(size_of::<TransferToHost2d>() == 56);
    assert!(size_of::<AttachBacking>() == 32);
    assert!(size_of::<MemEntry>() == 16);
    assert!(size_of::<GetCapsetInfo>() == 32);
    assert!(size_of::<RespCapsetInfo>() == 40);
    assert!(size_of::<GetCapset>() == 32);
    assert!(size_of::<CtxCreate>() == 96);
    assert!(size_of::<ResourceCreate3d>() == 72);
    assert!(size_of::<Box3d>() == 24);
    assert!(size_of::<TransferHost3d>() == 72);
    assert!(size_of::<CmdSubmit>() == 32);
    assert!(size_of::<CursorPos>() == 16);
    assert!(size_of::<UpdateCursor>() == 56);
    assert!(size_of::<ResourceCreateBlob>() == 56);
};

/// The bytes of a wire structure.
pub fn bytes_of<T: Wire>(v: &T) -> &[u8] {
    // SAFETY: Wire types have no padding, so every byte is initialised.
    unsafe { core::slice::from_raw_parts(v as *const T as *const u8, size_of::<T>()) }
}

/// Reads a wire structure from the start of `b`, if it is long enough.
pub fn read<T: Wire + Default>(b: &[u8]) -> Option<T> {
    let mut v = T::default();
    let src = b.get(..size_of::<T>())?;
    // SAFETY: Wire types accept every bit pattern; the destination is the
    // value's own bytes.
    unsafe { core::ptr::copy_nonoverlapping(src.as_ptr(), &mut v as *mut T as *mut u8, size_of::<T>()) };
    Some(v)
}

/// The response types that mean success.
pub fn resp_ok(t: u32) -> bool {
    (RESP_OK_NODATA..RESP_ERR_UNSPEC).contains(&t)
}

/// A short name of a response type, for log lines.
pub fn resp_name(t: u32) -> &'static str {
    match t {
        RESP_OK_NODATA => "OK_NODATA",
        RESP_OK_DISPLAY_INFO => "OK_DISPLAY_INFO",
        RESP_OK_CAPSET_INFO => "OK_CAPSET_INFO",
        RESP_OK_CAPSET => "OK_CAPSET",
        RESP_OK_EDID => "OK_EDID",
        0x1200 => "ERR_UNSPEC",
        0x1201 => "ERR_OUT_OF_MEMORY",
        0x1202 => "ERR_INVALID_SCANOUT_ID",
        0x1203 => "ERR_INVALID_RESOURCE_ID",
        0x1204 => "ERR_INVALID_CONTEXT_ID",
        0x1205 => "ERR_INVALID_PARAMETER",
        0 => "no response",
        _ => "unknown response",
    }
}

/// The virtio-gpu format of a DRM fourcc format that a 2D resource can
/// scan out, as Linux's `virtio_gpu_translate_format` maps them on a
/// little-endian guest.
pub fn format_of(fourcc: u32) -> Option<u32> {
    use drm::kms::formats::{ARGB8888, XRGB8888};
    const BGRX8888: u32 = u32::from_le_bytes(*b"BX24");
    const BGRA8888: u32 = u32::from_le_bytes(*b"BA24");
    match fourcc {
        XRGB8888 => Some(FORMAT_B8G8R8X8_UNORM),
        ARGB8888 => Some(FORMAT_B8G8R8A8_UNORM),
        BGRX8888 => Some(FORMAT_X8R8G8B8_UNORM),
        BGRA8888 => Some(FORMAT_A8R8G8B8_UNORM),
        _ => None,
    }
}
