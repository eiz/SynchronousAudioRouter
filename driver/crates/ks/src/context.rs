// Copyright (c) 2026 Itty Bitty Binaries, Inc.
// SPDX-License-Identifier: MIT

//! The tagged blocks this crate stores in AVStream `Context` fields.

use core::{any::TypeId, ffi::c_void};

/// What a context block is.
#[derive(Clone, Copy, PartialEq, Eq)]
pub(crate) enum Kind {
    Device,
    Factory,
}

/// First field of every context block.
#[repr(C)]
pub(crate) struct Header {
    kind: Kind,
    /// The handler type the block was created for.
    handler: TypeId,
}

impl Header {
    pub(crate) fn new<H: 'static>(kind: Kind) -> Self {
        Self {
            kind,
            handler: TypeId::of::<H>(),
        }
    }
}

/// Whether `context` is a block of the given kind, created for handler `H`.
///
/// # Safety
///
/// `context` must be null or a pointer this crate stored in an AVStream
/// `Context` field (directly or by inheritance), whose block is still live.
pub(crate) unsafe fn is<H: 'static>(context: *mut c_void, kind: Kind) -> bool {
    if context.is_null() {
        return false;
    }
    // SAFETY: by the caller's contract (and the crate invariant described
    // in the crate documentation) a non-null context addresses a live
    // `#[repr(C)]` block whose first field is a `Header`.
    let header = unsafe { &*context.cast::<Header>() };
    header.kind == kind && header.handler == TypeId::of::<H>()
}
