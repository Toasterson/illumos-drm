// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//! Host tests of the driver's pure logic: response codes, damage merging,
//! `CONTEXT_INIT` parameters, capability-set masks and the wire layout.

extern crate std;

use alloc::vec;

use ddi::Errno;
use drm::kms::DirtyRect;

use crate::gpu::{Caps, CapsetInfo, ContextParam, ContextParams, check, parse_context_params};
use crate::kms::merge_clips;
use crate::proto;

#[test]
fn responses_map_to_errnos() {
    assert_eq!(check(proto::RESP_OK_NODATA, proto::RESP_OK_NODATA), Ok(()));
    assert_eq!(check(proto::RESP_OK_EDID, 0), Ok(()));
    assert_eq!(check(proto::RESP_OK_NODATA, proto::RESP_OK_EDID), Err(Errno::EIO));
    assert_eq!(check(proto::RESP_ERR_OUT_OF_MEMORY, 0), Err(Errno::ENOMEM));
    assert_eq!(check(proto::RESP_ERR_INVALID_PARAMETER, 0), Err(Errno::EINVAL));
    assert_eq!(check(0x1203, 0), Err(Errno::EINVAL));
    assert_eq!(check(proto::RESP_ERR_UNSPEC, 0), Err(Errno::EIO));
    // No response written (0) is a failure, never success.
    assert_eq!(check(0, 0), Err(Errno::EIO));
    assert!(!proto::resp_ok(0) && proto::resp_ok(0x1106) && !proto::resp_ok(0x1200));
}

fn r(x1: u32, y1: u32, x2: u32, y2: u32) -> DirtyRect {
    DirtyRect { x1, y1, x2, y2 }
}

#[test]
fn damage_merges_to_a_clamped_bounding_box() {
    assert_eq!(merge_clips(&[], 640, 480), Some(r(0, 0, 640, 480)));
    assert_eq!(merge_clips(&[], 0, 480), None);
    assert_eq!(merge_clips(&[r(10, 10, 20, 20), r(100, 5, 110, 15)], 640, 480), Some(r(10, 5, 110, 20)));
    // Clamped to the framebuffer; empty and inverted clips drop out.
    assert_eq!(
        merge_clips(&[r(600, 400, 9000, 9000), r(5, 5, 5, 50), r(9, 9, 3, 3)], 640, 480),
        Some(r(600, 400, 640, 480))
    );
    assert_eq!(merge_clips(&[r(700, 0, 800, 10)], 640, 480), None);
}

#[test]
fn context_params_are_checked_as_linux_does() {
    let mask = (1 << proto::CAPSET_VIRGL) | (1 << proto::CAPSET_VIRGL2);
    assert_eq!(
        parse_context_params(
            vec![ContextParam::Capset(2), ContextParam::Rings(2), ContextParam::Name(b"gl".to_vec())],
            mask
        ),
        Ok(ContextParams { capset: 2, rings: 2, name: b"gl".to_vec() })
    );
    assert_eq!(parse_context_params(vec![], mask), Ok(ContextParams::default()));
    assert_eq!(parse_context_params(vec![ContextParam::Capset(4)], mask), Err(Errno::EINVAL));
    assert_eq!(parse_context_params(vec![ContextParam::Capset(0)], mask), Err(Errno::EINVAL));
    assert_eq!(parse_context_params(vec![ContextParam::Capset(64)], mask), Err(Errno::EINVAL));
    assert_eq!(parse_context_params(vec![ContextParam::Rings(65)], mask), Err(Errno::EINVAL));
    assert_eq!(parse_context_params(vec![ContextParam::Rings(64)], mask).map(|p| p.rings), Ok(64));
    assert_eq!(parse_context_params(vec![ContextParam::PollRings(1)], mask), Err(Errno::EINVAL));
    assert_eq!(parse_context_params(vec![ContextParam::PollRings(0)], mask), Ok(ContextParams::default()));
    // Each parameter at most once.
    assert_eq!(parse_context_params(vec![ContextParam::Rings(1), ContextParam::Rings(2)], mask), Err(Errno::EINVAL));
    assert_eq!(parse_context_params(vec![ContextParam::Capset(1), ContextParam::Capset(1)], mask), Err(Errno::EINVAL));
    assert_eq!(
        parse_context_params(vec![ContextParam::Name(vec![]), ContextParam::Name(vec![])], mask),
        Err(Errno::EINVAL)
    );
}

#[test]
fn capset_mask_has_a_bit_per_id() {
    let mut c = Caps::default();
    assert_eq!(c.capset_mask(), 0);
    c.capsets = vec![
        CapsetInfo { id: 1, max_version: 1, max_size: 308 },
        CapsetInfo { id: 2, max_version: 2, max_size: 1032 },
        CapsetInfo { id: 70, max_version: 1, max_size: 4 },
    ];
    assert_eq!(c.capset_mask(), 0b110);
}

#[test]
fn wire_structures_read_back() {
    let h = proto::CtrlHdr { type_: 0x1104, flags: 1, fence_id: 7, ctx_id: 3, ring_idx: 2, padding: [0; 3] };
    let b = proto::bytes_of(&h);
    assert_eq!(b.len(), 24);
    assert_eq!(&b[..4], &0x1104u32.to_le_bytes());
    assert_eq!(&b[8..16], &7u64.to_le_bytes());
    assert_eq!(b[20], 2);
    let back: proto::CtrlHdr = proto::read(b).unwrap();
    assert_eq!((back.type_, back.fence_id, back.ctx_id, back.ring_idx), (0x1104, 7, 3, 2));
    assert!(proto::read::<proto::CtrlHdr>(&b[..23]).is_none());
}
