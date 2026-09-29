// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//! Host tests of the KMS core with a mock driver: object IDs and
//! properties, framebuffers and blobs and their lifetimes, atomic state
//! duplication, rollback, the checks and their errnos, the commit, and the
//! legacy wrappers.

extern crate std;

use alloc::sync::Arc;
use alloc::vec::Vec;
use core::sync::atomic::{AtomicUsize, Ordering};
use std::string::String;
use std::sync::Mutex;

use ddi::{Errno, Sleepable};
use drm_uapi as uapi;

use super::atomic::ENOSPC;
use super::core::KmsCore;
use super::formats::*;
use super::legacy::SetCrtc;
use super::modes::{self, ERANGE, ModeLimits};
use super::objects::*;
use super::props::Prop;
use super::state::*;

fn ctx() -> Sleepable {
    // SAFETY: host tests run in an ordinary thread.
    unsafe { Sleepable::assume() }
}

#[derive(Debug)]
struct Buf {
    size: u64,
    released: Arc<AtomicUsize>,
}

impl FbBacking for Buf {
    fn size(&self) -> u64 {
        self.size
    }
    fn release(self, _ctx: &Sleepable) {
        self.released.fetch_add(1, Ordering::SeqCst);
    }
}

struct Mock {
    log: Mutex<Vec<String>>,
    released: Arc<AtomicUsize>,
}

impl Mock {
    fn new() -> Mock {
        Mock { log: Mutex::new(Vec::new()), released: Arc::new(AtomicUsize::new(0)) }
    }
    fn buf(&self, size: u64) -> Buf {
        Buf { size, released: self.released.clone() }
    }
    fn take_log(&self) -> Vec<String> {
        core::mem::take(&mut *self.log.lock().unwrap())
    }
}

impl KmsDriver<Buf> for Mock {
    fn layout(&self) -> Layout {
        let limits = ModeLimits { max_width: 2048, max_height: 2048, max_clock: 0, interlace: false, dblscan: false };
        Layout {
            min_width: 1,
            max_width: 2048,
            min_height: 1,
            max_height: 2048,
            planes: std::vec![
                PlaneDesc {
                    typ: PlaneType::Primary,
                    possible_crtcs: 1,
                    formats: std::vec![XRGB8888, ARGB8888, RGB565],
                    modifiers: std::vec![MOD_LINEAR],
                },
                PlaneDesc {
                    typ: PlaneType::Cursor,
                    possible_crtcs: 1,
                    formats: std::vec![ARGB8888],
                    modifiers: std::vec![]
                },
            ],
            crtcs: std::vec![CrtcDesc { primary: 0, cursor: Some(1), gamma_size: 256 }],
            encoders: std::vec![EncoderDesc {
                typ: uapi::DRM_MODE_ENCODER_VIRTUAL,
                possible_crtcs: 1,
                possible_clones: 0
            }],
            connectors: std::vec![ConnectorDesc { typ: uapi::DRM_MODE_CONNECTOR_VIRTUAL, encoders: 1, limits }],
        }
    }
    fn probe(&self, _c: usize, _ctx: &Sleepable) -> Probe {
        let edid = std::fs::read(std::format!("{}/tests/edid/rdrmnull.bin", env!("CARGO_MANIFEST_DIR"))).unwrap();
        Probe { status: ConnectorStatus::Connected, edid: Some(edid), ..Probe::default() }
    }
    fn check_plane(&self, p: usize, new: &PlaneState<Buf>, crtc: Option<&CrtcState>) -> Result<(), Errno> {
        if p == 1 && new.fb.as_ref().is_some_and(|f| f.width > 64 || f.height > 64) {
            return Err(Errno::EINVAL);
        }
        check_plane_helper(new, crtc, p == 1, false)
    }
    fn commit_crtc(&self, i: usize, _o: &CrtcState, n: &CrtcState, _ctx: &Sleepable) {
        self.log.lock().unwrap().push(std::format!("crtc{i} active={}", n.active));
    }
    fn commit_plane(&self, i: usize, _o: &PlaneState<Buf>, n: &PlaneState<Buf>, _ctx: &Sleepable) {
        self.log.lock().unwrap().push(std::format!("plane{i} fb={}", n.fb.as_ref().map_or(0, |f| f.id)));
    }
    fn commit_connector(&self, i: usize, _o: &ConnectorState, n: &ConnectorState, _ctx: &Sleepable) {
        self.log.lock().unwrap().push(std::format!("conn{i} crtc={:?}", n.crtc));
    }
}

const FILE: u32 = 1;
const OTHER: u32 = 2;

fn setup() -> (Mock, KmsCore<Buf>) {
    let m = Mock::new();
    let mut k = KmsCore::new(&m, &ctx()).unwrap();
    k.file_open(FILE, &ctx());
    k.file_open(OTHER, &ctx());
    (m, k)
}

fn req(w: u32, h: u32, format: u32) -> FbRequest {
    let cpp = lookup(format).unwrap().cpp[0] as u32;
    FbRequest {
        width: w,
        height: h,
        format,
        flags: 0,
        handles: [1, 0, 0, 0],
        pitches: [w * cpp, 0, 0, 0],
        offsets: [0; 4],
        modifiers: [0; 4],
    }
}

fn add_fb(m: &Mock, k: &mut KmsCore<Buf>, file: u32, w: u32, h: u32, format: u32) -> u32 {
    let r = req(w, h, format);
    k.check_fb(&r, m).unwrap();
    let size = u64::from(r.pitches[0]) * u64::from(h);
    k.add_fb(Some(file), &r, [Some(m.buf(size)), None, None, None], (0, 0), &ctx()).unwrap().id
}

fn ids(k: &KmsCore<Buf>) -> (u32, u32, u32, u32, u32) {
    let r = k.resources(FILE);
    let planes = k.plane_resources(true);
    (r.crtcs[0], r.connectors[0], r.encoders[0], planes[0], planes[1])
}

fn mode_blob(k: &mut KmsCore<Buf>, w: u16, h: u16) -> u32 {
    let m = modes::find_dmt(w, h, 60).unwrap();
    k.create_blob(FILE, mode_blob_data(&m), &ctx()).unwrap()
}

/// A full modeset through atomic properties.
fn modeset_state(k: &mut KmsCore<Buf>, fb: u32, allow: bool) -> Result<AtomicState<Buf>, Errno> {
    let (crtc, conn, _, primary, _) = ids(k);
    let blob = mode_blob(k, 1024, 768);
    let mut st = k.begin(CommitFlags { allow_modeset: allow, ..CommitFlags::default() });
    let set = |k: &KmsCore<Buf>, st: &mut AtomicState<Buf>, obj: u32, p: Prop, v: u64| {
        let kind = k.atomic_object(obj).unwrap();
        k.set_prop(st, kind, k.prop_id(p), v, Errno::ENOENT)
    };
    set(k, &mut st, conn, Prop::CrtcId, u64::from(crtc))?;
    set(k, &mut st, crtc, Prop::ModeId, u64::from(blob))?;
    set(k, &mut st, crtc, Prop::Active, 1)?;
    set(k, &mut st, primary, Prop::FbId, u64::from(fb))?;
    set(k, &mut st, primary, Prop::CrtcId, u64::from(crtc))?;
    set(k, &mut st, primary, Prop::SrcW, 1024 << 16)?;
    set(k, &mut st, primary, Prop::SrcH, 768 << 16)?;
    set(k, &mut st, primary, Prop::CrtcW, 1024)?;
    set(k, &mut st, primary, Prop::CrtcH, 768)?;
    Ok(st)
}

#[test]
fn objects_ids_and_properties() {
    let (_m, k) = setup();
    // Properties first, then planes, CRTCs, encoders, connectors, the two
    // IN_FORMATS blobs and the EDID blob.
    assert_eq!(k.prop_id(Prop::Edid), 1);
    assert_eq!(k.prop_id(Prop::InFormats), 20);
    assert_eq!(ids(&k), (23, 25, 24, 21, 22));
    assert!(k.plane_resources(false).is_empty(), "no overlay planes without universal planes");
    assert_eq!(k.get_property(k.prop_id(Prop::Type)).unwrap().name, "type");
    assert_eq!(k.get_property(23), Err(Errno::ENOENT));
    // Legacy clients do not see atomic properties.
    let legacy = k.obj_props(21, 0, false).unwrap();
    let atomic = k.obj_props(21, uapi::DRM_MODE_OBJECT_PLANE, true).unwrap();
    assert_eq!(legacy.iter().map(|p| p.0).collect::<Vec<_>>(), [k.prop_id(Prop::Type), k.prop_id(Prop::InFormats)]);
    assert_eq!(atomic.len(), 12);
    assert_eq!(k.obj_props(21, uapi::DRM_MODE_OBJECT_CRTC, true), Err(Errno::ENOENT), "type mismatch");
    assert_eq!(k.obj_props(999, 0, true), Err(Errno::ENOENT));
    assert_eq!(k.obj_props(1, 0, true), Err(Errno::EINVAL), "a property has no properties");
    // The type of the planes and their IN_FORMATS blobs.
    assert!(legacy.contains(&(k.prop_id(Prop::Type), 1)));
    let inf = k.blob(26).unwrap();
    assert_eq!(u32::from_ne_bytes(inf.data[8..12].try_into().unwrap()), 3);
    let enc = k.get_encoder(24).unwrap();
    assert_eq!((enc.typ, enc.crtc_id, enc.possible_crtcs), (uapi::DRM_MODE_ENCODER_VIRTUAL, 0, 1));
}

#[test]
fn connector_modes_from_edid() {
    let (m, mut k) = setup();
    let c = k.get_connector(25, true, false, true, &m, &ctx()).unwrap();
    assert_eq!(c.status, ConnectorStatus::Connected as u32);
    assert_eq!((c.width_mm, c.height_mm), (340, 270));
    assert_eq!(c.encoders, [24]);
    assert_eq!((c.modes[0].hdisplay, c.modes[0].vdisplay), (1024, 768));
    assert!(c.modes[0].type_ & uapi::DRM_MODE_TYPE_PREFERRED != 0);
    let edid = c.props.iter().find(|p| p.0 == k.prop_id(Prop::Edid)).unwrap().1;
    assert_eq!(edid, 28);
    assert_eq!(k.blob(28).unwrap().data.len(), 128);
    // Probing again with the same EDID keeps the blob.
    let again = k.get_connector(25, true, false, true, &m, &ctx()).unwrap();
    assert_eq!(again.props, c.props);
    assert!(c.props.iter().all(|p| p.0 != k.prop_id(Prop::CrtcId)) || c.props.len() == 5);
}

#[test]
fn framebuffers_and_modifiers() {
    let (m, mut k) = setup();
    let mut r = req(64, 64, XRGB8888);
    // Tiled: a modifier no plane supports.
    r.flags = uapi::DRM_MODE_FB_MODIFIERS;
    r.modifiers[0] = MOD_I915_X_TILED;
    assert_eq!(k.check_fb(&r, &m), Err(Errno::EINVAL));
    r.modifiers[0] = MOD_LINEAR;
    assert_eq!(k.check_fb(&r, &m), Ok(1));
    assert_eq!(k.check_fb(&req(64, 64, NV12), &m), Err(Errno::EINVAL), "no plane scans NV12 out");
    // An object too small for the image.
    let small = k.add_fb(Some(FILE), &req(64, 64, XRGB8888), [Some(m.buf(100)), None, None, None], (0, 0), &ctx());
    assert_eq!(small.err(), Some(Errno::EINVAL));
    assert_eq!(m.released.load(Ordering::SeqCst), 1, "the object reference goes back");
    let a = add_fb(&m, &mut k, FILE, 64, 64, XRGB8888);
    let b = add_fb(&m, &mut k, FILE, 64, 64, ARGB8888);
    assert_eq!(k.resources(FILE).fbs, [b, a], "newest first");
    assert!(k.resources(OTHER).fbs.is_empty());
    assert_eq!(k.rm_fb(OTHER, a, &m, &ctx()), Err(Errno::ENOENT), "not the owner");
    assert_eq!(k.rm_fb(FILE, 4242, &m, &ctx()), Err(Errno::ENOENT));
    k.rm_fb(FILE, a, &m, &ctx()).unwrap();
    assert!(k.fb(a).is_none(), "unused: freed at once");
    assert_eq!(m.released.load(Ordering::SeqCst), 2);
    let before = k.ids.len();
    k.file_close(FILE, &m, &ctx()).unwrap();
    assert!(k.fb(b).is_none());
    assert_eq!(k.ids.len(), before - 1);
}

#[test]
fn duplication_and_rollback() {
    let (m, mut k) = setup();
    let fb = add_fb(&m, &mut k, FILE, 1024, 768, XRGB8888);
    let (crtc, _, _, primary, cursor) = ids(&k);
    let mut st = k.begin(CommitFlags::default());
    let kind = k.atomic_object(primary).unwrap();
    k.set_prop(&mut st, kind, k.prop_id(Prop::SrcW), 5, Errno::ENOENT).unwrap();
    // Only the plane is duplicated; the cursor plane and the connector are
    // untouched, and the plane's (absent) CRTC is not dragged in.
    assert!(st.planes[0].is_some() && st.planes[1].is_none());
    assert!(st.connectors[0].is_none() && st.crtcs[0].is_none());
    assert_eq!(st.planes[0].as_ref().unwrap().src_w, 5);
    assert_eq!(k.cur.planes[0].src_w, 0, "the current state is untouched");
    drop(st);
    // A failing check leaves everything as it was.
    let mut bad = modeset_state(&mut k, fb, true).unwrap();
    let kind = k.atomic_object(primary).unwrap();
    k.set_prop(&mut bad, kind, k.prop_id(Prop::SrcW), 2048 << 16, Errno::ENOENT).unwrap();
    assert_eq!(k.check(&mut bad, 0, &m), Err(ENOSPC));
    drop(bad);
    assert!(!k.cur.crtcs[0].active && k.cur.planes[0].fb.is_none() && k.cur.connectors[0].crtc.is_none());
    assert!(m.take_log().is_empty(), "no hook ran");
    // The same update without the flag needs ALLOW_MODESET.
    let mut st = modeset_state(&mut k, fb, false).unwrap();
    assert_eq!(k.check(&mut st, 0, &m), Err(Errno::EINVAL));
    let mut st = modeset_state(&mut k, fb, true).unwrap();
    k.check(&mut st, 1, &m).unwrap();
    let done = k.commit(st, &m, &ctx());
    assert_eq!((done.flip_mask, done.off_mask), (1, 0));
    let plane = std::format!("plane0 fb={fb}");
    assert_eq!(m.take_log(), [plane.as_str(), "crtc0 active=true", "conn0 crtc=Some(0)"]);
    assert_eq!(k.take_transitions().len(), 1);
    super::core::release_old(done.old, &ctx());
    assert!(k.cur.crtcs[0].active);
    assert_eq!(k.cur.connectors[0].encoder, Some(0));
    assert_eq!(k.get_encoder(24).unwrap().crtc_id, crtc);
    let info = k.get_crtc(crtc).unwrap();
    assert_eq!((info.fb_id, info.mode.map(|m| m.hdisplay)), (fb, Some(1024)));
    assert_eq!(k.conns[0].dpms, uapi::DRM_MODE_DPMS_ON, "legacy DPMS follows the modeset");
    let _ = cursor;
}

#[test]
fn check_errnos() {
    let (m, mut k) = setup();
    let fb = add_fb(&m, &mut k, FILE, 1024, 768, XRGB8888);
    let (crtc, conn, _, primary, cursor) = ids(&k);
    let pk = k.atomic_object(primary).unwrap();
    let ck = k.atomic_object(crtc).unwrap();
    let nk = k.atomic_object(conn).unwrap();
    assert_eq!(k.atomic_object(fb), Err(Errno::ENOENT), "framebuffers have no properties");
    let mut st = k.begin(CommitFlags::default());
    assert_eq!(k.set_prop(&mut st, pk, 4242, 0, Errno::ENOENT), Err(Errno::ENOENT));
    assert_eq!(k.set_prop(&mut st, pk, k.prop_id(Prop::Active), 1, Errno::ENOENT), Err(Errno::ENOENT));
    assert_eq!(k.set_prop(&mut st, pk, k.prop_id(Prop::Active), 1, Errno::EINVAL), Err(Errno::EINVAL));
    // Fence properties exist but are refused until fences do (R2).
    assert_eq!(k.set_prop(&mut st, pk, k.prop_id(Prop::InFenceFd), 3, Errno::ENOENT), Err(Errno::EINVAL));
    assert_eq!(k.set_prop(&mut st, ck, k.prop_id(Prop::OutFencePtr), 8, Errno::ENOENT), Err(Errno::EINVAL));
    assert_eq!(k.set_prop(&mut st, pk, k.prop_id(Prop::Type), 1, Errno::ENOENT), Err(Errno::EINVAL), "immutable");
    assert_eq!(k.set_prop(&mut st, nk, k.prop_id(Prop::Dpms), 0, Errno::ENOENT), Err(Errno::EINVAL));
    assert_eq!(k.set_prop(&mut st, pk, k.prop_id(Prop::FbId), 4242, Errno::ENOENT), Err(Errno::EINVAL));
    assert_eq!(k.set_prop(&mut st, ck, k.prop_id(Prop::ModeId), 4242, Errno::ENOENT), Err(Errno::EINVAL));
    assert_eq!(k.set_prop(&mut st, ck, k.prop_id(Prop::Active), 2, Errno::ENOENT), Err(Errno::EINVAL));
    // A blob of the wrong size is not a mode.
    let junk = k.create_blob(FILE, std::vec![1, 2, 3], &ctx()).unwrap();
    assert_eq!(k.set_prop(&mut st, ck, k.prop_id(Prop::ModeId), u64::from(junk), Errno::ENOENT), Err(Errno::EINVAL));
    // ACTIVE without a mode.
    k.set_prop(&mut st, ck, k.prop_id(Prop::Active), 1, Errno::ENOENT).unwrap();
    assert_eq!(k.check(&mut st, 0, &m), Err(Errno::EINVAL));
    // A mode without connectors.
    let mut st = k.begin(CommitFlags { allow_modeset: true, ..CommitFlags::default() });
    let blob = mode_blob(&mut k, 1024, 768);
    k.set_prop(&mut st, ck, k.prop_id(Prop::ModeId), u64::from(blob), Errno::ENOENT).unwrap();
    assert_eq!(k.check(&mut st, 0, &m), Err(Errno::EINVAL));
    // An event for a CRTC that is off before and after.
    let mut st = k.begin(CommitFlags::default());
    k.set_prop(&mut st, ck, k.prop_id(Prop::Active), 0, Errno::ENOENT).unwrap();
    assert_eq!(k.check(&mut st, 1, &m), Err(Errno::EINVAL));
    // A framebuffer without a CRTC, and a destination past INT_MAX.
    let mut st = k.begin(CommitFlags::default());
    k.set_prop(&mut st, pk, k.prop_id(Prop::FbId), u64::from(fb), Errno::ENOENT).unwrap();
    assert_eq!(k.check(&mut st, 0, &m), Err(Errno::EINVAL));
    k.set_prop(&mut st, pk, k.prop_id(Prop::CrtcId), u64::from(crtc), Errno::ENOENT).unwrap();
    k.set_prop(&mut st, pk, k.prop_id(Prop::CrtcX), i32::MAX as u64, Errno::ENOENT).unwrap();
    k.set_prop(&mut st, pk, k.prop_id(Prop::CrtcW), 10, Errno::ENOENT).unwrap();
    assert_eq!(k.check(&mut st, 0, &m), Err(ERANGE));
    // The cursor plane cannot show XRGB8888.
    let ck2 = k.atomic_object(cursor).unwrap();
    let mut st = k.begin(CommitFlags::default());
    k.set_prop(&mut st, ck2, k.prop_id(Prop::FbId), u64::from(fb), Errno::ENOENT).unwrap();
    k.set_prop(&mut st, ck2, k.prop_id(Prop::CrtcId), u64::from(crtc), Errno::ENOENT).unwrap();
    assert_eq!(k.check(&mut st, 0, &m), Err(Errno::EINVAL));
}

#[test]
fn rmfb_and_close_turn_off_what_they_show() {
    let (m, mut k) = setup();
    let fb = add_fb(&m, &mut k, FILE, 1024, 768, XRGB8888);
    let mut st = modeset_state(&mut k, fb, true).unwrap();
    k.check(&mut st, 0, &m).unwrap();
    let d = k.commit(st, &m, &ctx());
    super::core::release_old(d.old, &ctx());
    k.take_transitions();
    m.take_log();
    // Another file's handle on the framebuffer does not keep the screen:
    // RMFB turns off the plane and, as it is primary, the CRTC.
    k.rm_fb(FILE, fb, &m, &ctx()).unwrap();
    assert!(!k.cur.crtcs[0].active && k.cur.planes[0].fb.is_none() && k.cur.connectors[0].crtc.is_none());
    assert_eq!(m.take_log(), ["crtc0 active=false", "plane0 fb=0", "conn0 crtc=None"]);
    assert_eq!(k.take_transitions(), [super::atomic::Transition { crtc: 0, on: false, frame_ns: 0 }]);
    assert!(k.fb(fb).is_none(), "freed once nothing shows it");
    // The same through close, and the file's blobs go too.
    let fb = add_fb(&m, &mut k, FILE, 1024, 768, XRGB8888);
    let mut st = modeset_state(&mut k, fb, true).unwrap();
    k.check(&mut st, 0, &m).unwrap();
    let d = k.commit(st, &m, &ctx());
    super::core::release_old(d.old, &ctx());
    let mode_id = k.cur.crtcs[0].mode_blob.as_ref().unwrap().id;
    k.file_close(FILE, &m, &ctx()).unwrap();
    assert!(!k.cur.crtcs[0].active);
    assert!(k.fb(fb).is_none() && k.blob(mode_id).is_none());
    // Only the static objects are left: 20 properties, 5 pipeline objects,
    // 2 IN_FORMATS blobs and the EDID blob.
    assert_eq!(k.ids.len(), 28);
    assert_eq!(m.released.load(Ordering::SeqCst), 2);
}

#[test]
fn blobs() {
    let (_m, mut k) = setup();
    assert_eq!(k.create_blob(FILE, Vec::new(), &ctx()), Err(Errno::EINVAL));
    assert_eq!(k.create_blob(FILE, std::vec![0; super::core::MAX_BLOB + 1], &ctx()), Err(Errno::ENOMEM));
    let b = k.create_blob(FILE, std::vec![7; 16], &ctx()).unwrap();
    assert_eq!(k.destroy_blob(OTHER, b, &ctx()), Err(Errno::EPERM));
    assert_eq!(k.destroy_blob(FILE, 4242, &ctx()), Err(Errno::ENOENT));
    // A blob a state uses outlives its destruction.
    let mut st = k.begin(CommitFlags::default());
    let ck = k.atomic_object(23).unwrap();
    let blob = mode_blob(&mut k, 800, 600);
    k.set_prop(&mut st, ck, k.prop_id(Prop::ModeId), u64::from(blob), Errno::ENOENT).unwrap();
    k.destroy_blob(FILE, blob, &ctx()).unwrap();
    assert!(k.blob(blob).is_some(), "still referenced by the update");
    drop(st);
    k.sweep(&ctx());
    assert!(k.blob(blob).is_none());
    k.destroy_blob(FILE, b, &ctx()).unwrap();
    assert!(k.blob(b).is_none());
}

#[test]
fn legacy_wrappers() {
    let (m, mut k) = setup();
    let (crtc, conn, _, primary, _) = ids(&k);
    let fb = add_fb(&m, &mut k, FILE, 1024, 768, XRGB8888);
    let fb16 = add_fb(&m, &mut k, FILE, 1024, 768, RGB565);
    let mode = modes::find_dmt(1024, 768, 60).unwrap();
    let r = |fb_id: u32, mode: Option<modes::DisplayMode>, conns: &'static [u32]| SetCrtc {
        crtc_id: crtc,
        fb_id,
        x: 0,
        y: 0,
        mode,
        connectors: conns,
    };
    static CONN: [u32; 1] = [25];
    assert_eq!(conn, CONN[0]);
    assert_eq!(k.setcrtc(&r(u32::MAX, Some(mode), &CONN), &m, &ctx()).err(), Some(Errno::EINVAL), "no current fb");
    assert_eq!(k.setcrtc(&r(4242, Some(mode), &CONN), &m, &ctx()).err(), Some(Errno::ENOENT));
    assert_eq!(k.setcrtc(&r(fb, Some(mode), &[]), &m, &ctx()).err(), Some(Errno::EINVAL));
    let mut big = r(fb, Some(mode), &CONN);
    big.x = 1;
    assert_eq!(k.setcrtc(&big, &m, &ctx()).err(), Some(ENOSPC), "viewport past the framebuffer");
    let mut st = k.setcrtc(&r(fb, Some(mode), &CONN), &m, &ctx()).unwrap();
    k.check(&mut st, 0, &m).unwrap();
    let d = k.commit(st, &m, &ctx());
    super::core::release_old(d.old, &ctx());
    let info = k.get_crtc(crtc).unwrap();
    assert_eq!((info.fb_id, info.mode.unwrap().hdisplay), (fb, 1024));
    let mode_id = k.obj_props(crtc, 0, true).unwrap()[1].1;
    assert_ne!(mode_id, 0, "a legacy mode is a blob too");
    // Page flips: another format is refused, the same format is fine.
    assert_eq!(k.page_flip(crtc, fb16).err(), Some(Errno::EINVAL));
    let fb2 = add_fb(&m, &mut k, FILE, 1024, 768, XRGB8888);
    let mut st = k.page_flip(crtc, fb2).unwrap();
    assert!(st.flags.nonblock && !st.flags.allow_modeset);
    k.check(&mut st, 1, &m).unwrap();
    let d = k.commit(st, &m, &ctx());
    assert_eq!(d.flip_mask, 1);
    super::core::release_old(d.old, &ctx());
    assert_eq!(k.get_plane(primary).unwrap().fb_id, fb2);
    // The cursor: image, move, hide; too large is refused by the driver.
    let cur = add_fb(&m, &mut k, FILE, 64, 64, ARGB8888);
    let cfb = k.fb(cur).cloned();
    let mut st = k.cursor(crtc, Some(cfb), Some((10, 20))).unwrap();
    assert!(st.flags.cursor);
    k.check(&mut st, 0, &m).unwrap();
    let d = k.commit(st, &m, &ctx());
    super::core::release_old(d.old, &ctx());
    k.set_cursor_pos(crtc, (10, 20));
    assert_eq!((k.cur.planes[1].crtc_x, k.cur.planes[1].crtc_w), (10, 64));
    let mut st = k.cursor(crtc, None, Some((30, 40))).unwrap();
    k.check(&mut st, 0, &m).unwrap();
    let d = k.commit(st, &m, &ctx());
    super::core::release_old(d.old, &ctx());
    assert_eq!((k.cur.planes[1].crtc_x, k.cur.planes[1].fb.as_ref().unwrap().id), (30, cur));
    let huge = add_fb(&m, &mut k, FILE, 128, 128, ARGB8888);
    let hfb = k.fb(huge).cloned();
    let mut st = k.cursor(crtc, Some(hfb), None).unwrap();
    assert_eq!(k.check(&mut st, 0, &m), Err(Errno::EINVAL));
    drop(st);
    let mut st = k.cursor(crtc, Some(None), None).unwrap();
    k.check(&mut st, 0, &m).unwrap();
    let d = k.commit(st, &m, &ctx());
    super::core::release_old(d.old, &ctx());
    assert!(k.cur.planes[1].fb.is_none());
    // SETPLANE errors.
    assert_eq!(k.setplane(4242, crtc, fb, (0, 0, 1, 1), (0, 0, 1 << 16, 1 << 16), &m).err(), Some(Errno::ENOENT));
    assert_eq!(k.setplane(primary, crtc, 4242, (0, 0, 1, 1), (0, 0, 0, 0), &m).err(), Some(Errno::ENOENT));
    assert_eq!(k.setplane(primary, crtc, fb, (0, 0, 1, 1), (0, 0, 2048 << 16, 1 << 16), &m).err(), Some(ENOSPC));
    assert_eq!(k.setplane(primary, crtc, fb, (i32::MAX, 0, 2, 1), (0, 0, 1 << 16, 1 << 16), &m).err(), Some(ERANGE));
    // DPMS off turns the CRTC off; on turns it back on.
    let ci = k.connector_index(conn).unwrap();
    let mut st = k.dpms(ci, u64::from(uapi::DRM_MODE_DPMS_STANDBY)).unwrap();
    k.check(&mut st, 0, &m).unwrap();
    let d = k.commit(st, &m, &ctx());
    super::core::release_old(d.old, &ctx());
    assert!(!k.cur.crtcs[0].active && k.cur.crtcs[0].enable());
    let dpms = k.obj_props(conn, 0, false).unwrap().iter().find(|p| p.0 == k.prop_id(Prop::Dpms)).unwrap().1;
    assert_eq!(dpms, u64::from(uapi::DRM_MODE_DPMS_OFF));
    let mut st = k.dpms(ci, u64::from(uapi::DRM_MODE_DPMS_ON)).unwrap();
    k.check(&mut st, 0, &m).unwrap();
    let d = k.commit(st, &m, &ctx());
    super::core::release_old(d.old, &ctx());
    assert!(k.cur.crtcs[0].active);
    // SETCRTC without a mode turns everything off.
    let mut st = k.setcrtc(&r(0, None, &[]), &m, &ctx()).unwrap();
    k.check(&mut st, 0, &m).unwrap();
    let d = k.commit(st, &m, &ctx());
    super::core::release_old(d.old, &ctx());
    assert!(!k.cur.crtcs[0].enable() && k.cur.connectors[0].crtc.is_none());
    k.sweep(&ctx());
    assert!(k.blob(mode_id as u32).is_none(), "the legacy mode blob went with its last state");
}

#[test]
fn gamma() {
    let (m, mut k) = setup();
    let (r, g, b) = k.get_gamma(23, 256).unwrap();
    assert_eq!((r[0], r[255], g[128], b.len()), (0, 0xffff, 0x8080, 256));
    assert_eq!(k.get_gamma(23, 255).err(), Some(Errno::EINVAL));
    assert_eq!(k.get_gamma(4242, 256).err(), Some(Errno::ENOENT));
    let lut: Vec<u16> = (0..768).map(|i| i as u16).collect();
    assert_eq!(k.set_gamma(23, 256, &lut[..10], &m), Err(Errno::EINVAL));
    k.set_gamma(23, 256, &lut, &m).unwrap();
    assert_eq!(k.get_gamma(23, 256).unwrap().2[0], 512);
}
