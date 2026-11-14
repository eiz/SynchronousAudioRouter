// Copyright (c) 2026 Itty Bitty Binaries, Inc.
// SPDX-License-Identifier: MIT

//! I/O request packets.
//!
//! An IRP handed to a dispatch routine must be completed exactly once, or
//! marked pending and completed later, or passed on. [`Irp`] is a linear
//! token for that obligation: its methods consume it, a dispatch function
//! can only produce its [`DispatchResult`] return value through one of them,
//! and an `Irp` that is dropped on an error path is completed with
//! `STATUS_UNSUCCESSFUL` instead of being leaked.

use core::{ffi::c_void, marker::PhantomData, ptr::NonNull};

use ntstatus::{IoStatus, NtStatus};
use ntx_sys::seh::{SarShimProbeAndRead, SarShimProbeAndWrite, SarShimProbeForWrite};
use wdk_sys::{
    ntddk::IofCompleteRequest, FILE_OBJECT, IO_NO_INCREMENT, IO_STACK_LOCATION, IRP,
    IRP_MJ_DEVICE_CONTROL, NTSTATUS, PIRP, SL_PENDING_RETURNED,
};

use crate::string::units_of;

/// `IRP_BUFFERED_IO`: the I/O manager allocated `AssociatedIrp.SystemBuffer`.
const IRP_BUFFERED_IO: u32 = 0x0000_0010;
/// The transfer-method bits of a device control code.
const METHOD_MASK: u32 = 3;
const METHOD_BUFFERED: u32 = 0;

/// What a dispatch routine returns. Only obtainable by disposing of the IRP.
#[must_use]
pub struct DispatchResult(pub(crate) NTSTATUS);

impl DispatchResult {
    pub(crate) fn into_raw(self) -> NTSTATUS {
        self.0
    }
}

fn current_stack<'a>(irp: NonNull<IRP>) -> &'a IO_STACK_LOCATION {
    // SAFETY: an IRP owned by a driver always has a valid current stack
    // location (this is the inline IoGetCurrentIrpStackLocation), and the
    // I/O manager does not modify it while the driver owns the IRP. The
    // union field read is the one the I/O manager initialised.
    unsafe { &*irp.as_ref().Tail.Overlay.__bindgen_anon_2.__bindgen_anon_1.CurrentStackLocation }
}

fn complete_raw(irp: NonNull<IRP>, status: IoStatus) -> NTSTATUS {
    let mut information = status.information;
    if !status.status.is_error() {
        // For a buffered request the I/O manager copies `Information` bytes
        // out of the system buffer; never let it copy more than the buffer
        // the caller supplied.
        if let Some(limit) = buffered_output_length(irp) {
            information = information.min(limit);
        }
    }
    // SAFETY: the caller owns the IRP (it holds the linear token), so it may
    // fill in the status block and complete it; the pointer is not used
    // afterwards.
    unsafe {
        let raw = irp.as_ptr();
        (*raw).IoStatus.__bindgen_anon_1.Status = status.status.0;
        (*raw).IoStatus.Information = information as u64;
        IofCompleteRequest(raw, IO_NO_INCREMENT as i8);
    }
    status.status.0
}

/// Output length of a METHOD_BUFFERED device control request, if `irp` is one.
fn buffered_output_length(irp: NonNull<IRP>) -> Option<usize> {
    let stack = current_stack(irp);
    if u32::from(stack.MajorFunction) != IRP_MJ_DEVICE_CONTROL {
        return None;
    }
    // SAFETY: for IRP_MJ_DEVICE_CONTROL the DeviceIoControl member is the
    // active one.
    let params = unsafe { &stack.Parameters.DeviceIoControl };
    (params.IoControlCode & METHOD_MASK == METHOD_BUFFERED)
        .then_some(params.OutputBufferLength as usize)
}

fn buffered_output<'a>(irp: NonNull<IRP>) -> &'a mut [u8] {
    let Some(length) = buffered_output_length(irp) else {
        return &mut [];
    };
    // SAFETY: the IRP is valid while the caller holds its token.
    let (flags, buffer) = unsafe {
        let raw = irp.as_ref();
        (raw.Flags, raw.AssociatedIrp.SystemBuffer)
    };
    if length == 0 || buffer.is_null() || flags & IRP_BUFFERED_IO == 0 {
        return &mut [];
    }
    // SAFETY: for a METHOD_BUFFERED request with IRP_BUFFERED_IO set, the
    // I/O manager allocated SystemBuffer with at least
    // max(InputBufferLength, OutputBufferLength) bytes, and it belongs to
    // the IRP until completion. The caller's `&mut` token makes the borrow
    // unique.
    unsafe { core::slice::from_raw_parts_mut(buffer.cast::<u8>(), length) }
}

/// An IRP inside the dispatch routine it was delivered to, on the thread
/// that delivered it. Not `Send`: the user-buffer methods are only
/// meaningful in the requesting thread's address space.
pub struct Irp {
    raw: NonNull<IRP>,
    _not_send: PhantomData<*mut ()>,
}

impl Irp {
    /// # Safety
    ///
    /// `raw` must be an IRP that the I/O manager just passed to this
    /// driver's dispatch routine, on the current thread, and that nothing
    /// else will complete.
    pub(crate) unsafe fn from_raw(raw: PIRP) -> Self {
        Self {
            // SAFETY: a dispatch routine is never called with a null IRP.
            raw: unsafe { NonNull::new_unchecked(raw) },
            _not_send: PhantomData,
        }
    }

    pub(crate) fn into_raw(self) -> PIRP {
        let raw = self.raw.as_ptr();
        core::mem::forget(self);
        raw
    }

    fn stack(&self) -> &IO_STACK_LOCATION {
        current_stack(self.raw)
    }

    /// The `IRP_MJ_*` code.
    pub fn major_function(&self) -> u32 {
        u32::from(self.stack().MajorFunction)
    }

    fn device_control(&self) -> Option<(u32, usize, usize, *mut c_void)> {
        if self.major_function() != IRP_MJ_DEVICE_CONTROL {
            return None;
        }
        // SAFETY: DeviceIoControl is the active member for this major code.
        let p = unsafe { &self.stack().Parameters.DeviceIoControl };
        Some((
            p.IoControlCode,
            p.InputBufferLength as usize,
            p.OutputBufferLength as usize,
            p.Type3InputBuffer,
        ))
    }

    /// The device control code (0 if this is not a device control request).
    pub fn control_code(&self) -> u32 {
        self.device_control().map_or(0, |d| d.0)
    }

    /// Length of the caller's input buffer.
    pub fn input_length(&self) -> usize {
        self.device_control().map_or(0, |d| d.1)
    }

    /// Length of the caller's output buffer.
    pub fn output_length(&self) -> usize {
        self.device_control().map_or(0, |d| d.2)
    }

    /// The file object the request is for.
    pub fn file(&self) -> Option<FileRef<'_>> {
        NonNull::new(self.stack().FileObject).map(|raw| FileRef {
            raw,
            _irp: PhantomData,
        })
    }

    /// Reads the first `N` bytes of the input buffer of a "neither"-method
    /// request straight from the caller's address space (A.3): the pointer
    /// must lie in user space, be aligned to `alignment` and be readable,
    /// otherwise the exception's status is returned
    /// (`STATUS_DATATYPE_MISALIGNMENT`, `STATUS_ACCESS_VIOLATION`).
    ///
    /// The caller checks the input length first.
    pub fn read_user_input<const N: usize>(&self, alignment: u32) -> Result<[u8; N], NtStatus> {
        let (_, _, _, pointer) = self
            .device_control()
            .ok_or(NtStatus::INVALID_DEVICE_REQUEST)?;
        let mut out = [0u8; N];
        // SAFETY: `out` is valid for N bytes. The source is an untrusted
        // address; the shim probes it and copies under an exception handler.
        // We are in the requesting thread (this type is not Send) below
        // DISPATCH_LEVEL.
        let status = unsafe {
            SarShimProbeAndRead(out.as_mut_ptr().cast(), pointer.cast_const(), N, alignment)
        };
        crate::check(status).map(|()| out)
    }

    fn user_buffer(&self) -> *mut c_void {
        // SAFETY: the IRP is valid while `self` exists.
        unsafe { self.raw.as_ref() }.UserBuffer
    }

    /// Checks that `length` bytes at the caller's output pointer of a
    /// "neither"-method request lie in user space, are aligned to
    /// `alignment` and are writable.
    pub fn probe_user_output(&self, length: usize, alignment: u32) -> Result<(), NtStatus> {
        // SAFETY: the destination is an untrusted address that the shim only
        // probes, under an exception handler, in the requesting thread.
        let status = unsafe { SarShimProbeForWrite(self.user_buffer(), length, alignment) };
        crate::check(status)
    }

    /// Writes `bytes` to the caller's output pointer of a "neither"-method
    /// request, with the same checks as [`probe_user_output`](Self::probe_user_output).
    pub fn write_user_output(&self, bytes: &[u8], alignment: u32) -> Result<(), NtStatus> {
        // SAFETY: `bytes` is valid for its length. The destination is an
        // untrusted address; the shim probes it and copies under an
        // exception handler, in the requesting thread.
        let status = unsafe {
            SarShimProbeAndWrite(
                self.user_buffer(),
                bytes.as_ptr().cast(),
                bytes.len(),
                alignment,
            )
        };
        crate::check(status)
    }

    /// The output buffer of a buffered-method device control request: the
    /// system buffer, as long as the caller's output length. Empty for any
    /// other kind of request.
    pub fn buffered_output(&mut self) -> &mut [u8] {
        buffered_output(self.raw)
    }

    /// Completes the request.
    pub fn complete(self, status: impl Into<IoStatus>) -> DispatchResult {
        let raw = self.raw;
        core::mem::forget(self);
        DispatchResult(complete_raw(raw, status.into()))
    }

    /// Marks the request pending. The dispatch routine returns the
    /// [`DispatchResult`]; whoever holds the [`PendingIrp`] completes it.
    pub fn pend(self) -> (PendingIrp, DispatchResult) {
        let raw = self.raw;
        core::mem::forget(self);
        // SAFETY: this is IoMarkIrpPending: set SL_PENDING_RETURNED in the
        // current stack location of an IRP this driver owns.
        unsafe {
            let stack = (*raw.as_ptr())
                .Tail
                .Overlay
                .__bindgen_anon_2
                .__bindgen_anon_1
                .CurrentStackLocation;
            (*stack).Control |= SL_PENDING_RETURNED as u8;
        }
        (
            PendingIrp { raw },
            DispatchResult(NtStatus::PENDING.0),
        )
    }
}

impl Drop for Irp {
    fn drop(&mut self) {
        // An IRP must never be forgotten: the caller would wait forever.
        let _ = complete_raw(self.raw, IoStatus::status(NtStatus::UNSUCCESSFUL));
    }
}

/// An IRP that has been marked pending and will be completed later,
/// possibly on another thread.
pub struct PendingIrp {
    raw: NonNull<IRP>,
}

// SAFETY: a pending IRP is owned by the driver and may be completed from any
// thread; the type exposes no access to the requestor's address space.
unsafe impl Send for PendingIrp {}

impl PendingIrp {
    /// # Safety
    ///
    /// `raw` must be an IRP that was marked pending, that this driver owns,
    /// and that nothing else will complete.
    pub(crate) unsafe fn from_raw(raw: NonNull<IRP>) -> Self {
        Self { raw }
    }

    /// See [`Irp::buffered_output`].
    pub fn buffered_output(&mut self) -> &mut [u8] {
        buffered_output(self.raw)
    }

    /// Completes the request.
    pub fn complete(self, status: impl Into<IoStatus>) {
        let raw = self.raw;
        core::mem::forget(self);
        let _ = complete_raw(raw, status.into());
    }
}

impl Drop for PendingIrp {
    fn drop(&mut self) {
        let _ = complete_raw(self.raw, IoStatus::status(NtStatus::UNSUCCESSFUL));
    }
}

/// The file object of a request.
pub struct FileRef<'a> {
    raw: NonNull<FILE_OBJECT>,
    _irp: PhantomData<&'a Irp>,
}

impl<'a> FileRef<'a> {
    pub(crate) fn as_raw(&self) -> *mut FILE_OBJECT {
        self.raw.as_ptr()
    }

    /// The file this one is being opened relative to, if any. Only
    /// meaningful while the file's create request is being handled: the
    /// I/O manager keeps the parent referenced for that long and no longer.
    pub(crate) fn related(&self) -> Option<FileRef<'a>> {
        // SAFETY: the file object is valid for 'a; RelatedFileObject is
        // null or a file object the I/O manager holds for the duration of
        // the create request.
        NonNull::new(unsafe { (*self.raw.as_ptr()).RelatedFileObject }).map(|raw| FileRef {
            raw,
            _irp: PhantomData,
        })
    }

    /// The name the file was opened with, relative to the device.
    pub fn name(&self) -> &'a [u16] {
        // SAFETY: the file object is valid for 'a and its FileName is set by
        // the I/O manager before the create request and not changed after.
        unsafe { units_of(core::ptr::addr_of!((*self.raw.as_ptr()).FileName)) }
    }
}
