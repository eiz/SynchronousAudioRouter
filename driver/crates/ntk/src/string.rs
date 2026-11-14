// Copyright (c) 2026 Itty Bitty Binaries, Inc.
// SPDX-License-Identifier: MIT

//! Counted UTF-16 strings (`UNICODE_STRING`).

use core::marker::PhantomData;

use wdk_sys::{PCUNICODE_STRING, PUNICODE_STRING, UNICODE_STRING};

/// A `UNICODE_STRING` that borrows its characters from a slice.
pub struct UnicodeStr<'a> {
    raw: UNICODE_STRING,
    _borrow: PhantomData<&'a [u16]>,
}

impl<'a> UnicodeStr<'a> {
    /// Wraps `units` (no terminator needed). `None` if the string is longer
    /// than a `UNICODE_STRING` can describe.
    pub fn new(units: &'a [u16]) -> Option<Self> {
        let bytes = u16::try_from(units.len().checked_mul(2)?).ok()?;
        Some(Self {
            raw: UNICODE_STRING {
                Length: bytes,
                MaximumLength: bytes,
                // An empty slice has a dangling, non-null pointer (its
                // alignment, 2). The kernel wants null for "no buffer", and
                // Driver Verifier treats the dangling value as a user-mode
                // address handed to a Zw routine.
                Buffer: if units.is_empty() {
                    core::ptr::null_mut()
                } else {
                    units.as_ptr().cast_mut()
                },
            },
            _borrow: PhantomData,
        })
    }

    /// The pointer form kernel routines take. They declare it mutable but
    /// only read the strings this crate passes them.
    pub(crate) fn as_ptr(&self) -> PUNICODE_STRING {
        core::ptr::from_ref(&self.raw).cast_mut()
    }
}

/// The characters of a kernel-owned `UNICODE_STRING`.
///
/// # Safety
///
/// `string` must be null or point to a valid `UNICODE_STRING` whose buffer
/// stays valid and unmodified for `'a`.
pub(crate) unsafe fn units_of<'a>(string: PCUNICODE_STRING) -> &'a [u16] {
    if string.is_null() {
        return &[];
    }
    // SAFETY: non-null and valid by the caller's contract.
    let s = unsafe { &*string };
    if s.Buffer.is_null() || s.Length < 2 {
        return &[];
    }
    // SAFETY: a valid UNICODE_STRING's buffer holds `Length` bytes of
    // UTF-16, and the caller keeps it alive for `'a`.
    unsafe { core::slice::from_raw_parts(s.Buffer, usize::from(s.Length / 2)) }
}
