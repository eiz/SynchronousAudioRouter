// Copyright (c) 2026 Itty Bitty Binaries, Inc.
// SPDX-License-Identifier: MIT

//! Safe wrappers over the parts of the NT kernel API this driver uses.
//!
//! This crate is the FFI boundary for everything that is not kernel
//! streaming: it owns the `unsafe` needed to call `wdk-sys` and `ntx-sys`
//! and exposes types whose safe methods uphold the kernel's memory-safety
//! rules by construction (ownership for handles and references, guards for
//! locks, linear types for I/O request packets).
//!
//! # What "safe" covers here
//!
//! Rust's safety contract is about memory safety. The kernel has a second
//! set of rules about *where* code may run: most of these wrappers call
//! routines documented for `IRQL = PASSIVE_LEVEL` and must be used from
//! there. Calling them at a raised IRQL is a kernel-rule violation that
//! Windows answers by stopping the system (a bugcheck), not by corrupting
//! memory, and the type system here does not model IRQL. The few items that
//! may be used at `DISPATCH_LEVEL` say so ([`sync::SpinLock`],
//! [`csq::IrpQueue`] state closures, [`alloc_ext`]). Every driver entry point
//! that reaches this crate runs at `PASSIVE_LEVEL`.
#![no_std]
#![deny(unsafe_op_in_unsafe_fn)]
#![deny(clippy::undocumented_unsafe_blocks)]

extern crate alloc;

pub mod alloc_ext;
pub mod csq;
pub mod device;
pub mod dispatch;
pub mod interface;
pub mod irp;
pub mod object;
pub mod registry;
pub mod section;
pub mod security;
pub mod string;
pub mod sync;
pub mod work;

pub use ntstatus::{IoStatus, NtStatus};
/// The Windows `GUID` structure.
pub use wdk_sys::GUID as Guid;

/// Pool allocator for `alloc` (Microsoft's `wdk-alloc`: non-paged,
/// non-executable pool through `ExAllocatePool2`, usable up to
/// `DISPATCH_LEVEL`).
#[global_allocator]
static ALLOCATOR: wdk_alloc::WdkAllocator = wdk_alloc::WdkAllocator;

/// Bugcheck code used when the driver panics: "SAR" and a zero byte, in the
/// range Windows leaves to drivers.
pub const PANIC_BUGCHECK_CODE: u32 = 0xE053_4152;

/// A Rust panic in a kernel driver cannot unwind and must not continue:
/// stop the system with a recognisable code, the source line as parameter 1
/// and the address of the file name as parameter 2 (parameter 3 its length).
#[panic_handler]
fn panic(info: &core::panic::PanicInfo<'_>) -> ! {
    let (line, file_ptr, file_len) = match info.location() {
        Some(location) => (
            location.line() as usize,
            location.file().as_ptr() as usize,
            location.file().len(),
        ),
        None => (0, 0, 0),
    };
    // SAFETY: KeBugCheckEx may be called at any IRQL with any arguments and
    // does not return.
    unsafe {
        wdk_sys::ntddk::KeBugCheckEx(
            PANIC_BUGCHECK_CODE,
            line as u64,
            file_ptr as u64,
            file_len as u64,
            0,
        )
    }
}

/// Converts a raw `NTSTATUS` into `Ok(())` for `NT_SUCCESS` values and
/// `Err(status)` otherwise.
pub(crate) fn check(status: wdk_sys::NTSTATUS) -> Result<(), NtStatus> {
    NtStatus(status).into_result()
}

/// Builds a [`Guid`] from its in-memory byte form.
pub const fn guid_from_bytes(bytes: [u8; 16]) -> Guid {
    Guid {
        Data1: u32::from_le_bytes([bytes[0], bytes[1], bytes[2], bytes[3]]),
        Data2: u16::from_le_bytes([bytes[4], bytes[5]]),
        Data3: u16::from_le_bytes([bytes[6], bytes[7]]),
        Data4: [
            bytes[8], bytes[9], bytes[10], bytes[11], bytes[12], bytes[13], bytes[14], bytes[15],
        ],
    }
}

/// The in-memory byte form of a [`Guid`].
pub const fn guid_to_bytes(guid: &Guid) -> [u8; 16] {
    let a = guid.Data1.to_le_bytes();
    let b = guid.Data2.to_le_bytes();
    let c = guid.Data3.to_le_bytes();
    let d = guid.Data4;
    [
        a[0], a[1], a[2], a[3], b[0], b[1], c[0], c[1], d[0], d[1], d[2], d[3], d[4], d[5], d[6],
        d[7],
    ]
}
