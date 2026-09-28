// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

//! The errno a failed DMA allocation or binding of object memory reaches
//! userland with.
//!
//! When the memory of a GEM object cannot be bound for DMA, the ioctl that
//! creates the object (`MODE_CREATE_DUMB`, or a driver's own create ioctl)
//! fails. Linux's DRM reports a failed object allocation as `ENOMEM`, and
//! `EINVAL` for a request the driver can never satisfy, so libdrm and Mesa
//! treat those two as "out of memory" and "bad request". The DDI reports
//! more detail, and the generic mapping of illumos-ddi
//! (`From<DmaError> for Errno`) answers `EFAULT` for `DDI_DMA_NOMAPPING`,
//! which in a create ioctl reads as a bad user pointer. On 28 September
//! 2026 the root-node drill on a patched kernel saw every failed bind reach
//! userland as `EFAULT` that way. [`object_errno`] maps for object creation
//! instead:
//!
//! * `ENOMEM` when the framework is short of resources or cannot map the
//!   memory for the device (`DDI_DMA_NORESOURCES`, `DDI_DMA_TOOBIG`,
//!   `DDI_DMA_NOMAPPING`, and `DDI_DMA_PARTIAL_MAP`, which the core never
//!   asks for and releases at once);
//! * `EINVAL` when the attributes can never be satisfied
//!   (`DDI_DMA_BADATTR`, `DDI_DMA_BADLIMITS`, `DDI_DMA_TOOSMALL`, a zero
//!   length or an out-of-range request);
//! * `ENXIO` for [`DmaError::NotOwnNode`], the own-node guard of
//!   illumos-ddi, like every other refusal of a node that is not the
//!   device's own;
//! * `EIO` for what a fresh handle cannot report (`DDI_DMA_INUSE`,
//!   `DDI_DMA_LOCKED`, `DDI_DMA_STALE`, `DDI_FAILURE`) and for codes this
//!   crate does not know: those would be framework faults, not a shortage.

use core::ffi::c_int;

use ddi::sys;
use ddi::{DmaError, Errno};

/// Interprets a failed return code of `ddi_dma_alloc_handle(9F)` or
/// `ddi_dma_addr_bind_handle(9F)`, which are in the same `DDI_DMA_*`
/// convention. `DDI_SUCCESS` and `DDI_DMA_MAPPED` (both zero) are not
/// failures; they come back as [`DmaError::Other`], like any code the
/// manual pages do not list for these functions.
pub fn from_code(r: c_int) -> DmaError {
    match r {
        sys::DDI_DMA_PARTIAL_MAP => DmaError::PartialMap,
        sys::DDI_DMA_NORESOURCES => DmaError::NoResources,
        sys::DDI_DMA_NOMAPPING => DmaError::NoMapping,
        sys::DDI_DMA_TOOBIG => DmaError::TooBig,
        sys::DDI_DMA_INUSE => DmaError::InUse,
        sys::DDI_DMA_BADATTR => DmaError::BadAttr,
        other => DmaError::Other(other),
    }
}

/// The errno for a DMA failure while creating an object, as the
/// [module documentation](self) explains.
pub fn object_errno(e: DmaError) -> Errno {
    match e {
        DmaError::NoResources | DmaError::TooBig | DmaError::NoMapping | DmaError::PartialMap => Errno::ENOMEM,
        DmaError::BadAttr
        | DmaError::BadLimits
        | DmaError::TooSmall
        | DmaError::InvalidLength
        | DmaError::OutOfRange => Errno::EINVAL,
        DmaError::NotOwnNode => Errno::ENXIO,
        DmaError::InUse | DmaError::Locked | DmaError::Stale | DmaError::Failure | DmaError::Other(_) => Errno::EIO,
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn failed_binds_are_enomem_not_efault() {
        // The codes ddi_dma_addr_bind_handle(9F) can return for a failed bind
        // of a fresh handle without DDI_DMA_PARTIAL. NOMAPPING is what the
        // patched kernel returns for a handle of the root node.
        for code in [sys::DDI_DMA_NOMAPPING, sys::DDI_DMA_NORESOURCES, sys::DDI_DMA_TOOBIG, sys::DDI_DMA_PARTIAL_MAP] {
            assert_eq!(object_errno(from_code(code)), Errno::ENOMEM, "code {code}");
            assert_ne!(object_errno(from_code(code)), Errno::EFAULT);
        }
    }

    #[test]
    fn allocation_errors() {
        // ddi_dma_alloc_handle(9F): a pseudo node gets BADATTR.
        assert_eq!(object_errno(from_code(sys::DDI_DMA_BADATTR)), Errno::EINVAL);
        assert_eq!(object_errno(from_code(sys::DDI_DMA_NORESOURCES)), Errno::ENOMEM);
        // The own-node guard of illumos-ddi.
        assert_eq!(object_errno(DmaError::NotOwnNode), Errno::ENXIO);
    }

    #[test]
    fn requests_that_can_never_succeed_are_einval() {
        for e in
            [DmaError::BadAttr, DmaError::BadLimits, DmaError::TooSmall, DmaError::InvalidLength, DmaError::OutOfRange]
        {
            assert_eq!(object_errno(e), Errno::EINVAL, "{e:?}");
        }
    }

    #[test]
    fn framework_faults_are_eio() {
        assert_eq!(object_errno(from_code(sys::DDI_DMA_INUSE)), Errno::EIO);
        assert_eq!(object_errno(from_code(12345)), Errno::EIO);
        assert_eq!(from_code(12345), DmaError::Other(12345));
        for e in [DmaError::Locked, DmaError::Stale, DmaError::Failure] {
            assert_eq!(object_errno(e), Errno::EIO, "{e:?}");
        }
    }

    #[test]
    fn every_bind_code_is_decoded() {
        assert_eq!(from_code(sys::DDI_DMA_PARTIAL_MAP), DmaError::PartialMap);
        assert_eq!(from_code(sys::DDI_DMA_NORESOURCES), DmaError::NoResources);
        assert_eq!(from_code(sys::DDI_DMA_NOMAPPING), DmaError::NoMapping);
        assert_eq!(from_code(sys::DDI_DMA_TOOBIG), DmaError::TooBig);
        assert_eq!(from_code(sys::DDI_DMA_INUSE), DmaError::InUse);
        assert_eq!(from_code(sys::DDI_DMA_BADATTR), DmaError::BadAttr);
    }
}
