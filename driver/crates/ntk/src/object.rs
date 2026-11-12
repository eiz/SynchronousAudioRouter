// Copyright (c) 2026 Itty Bitty Binaries, Inc.
// SPDX-License-Identifier: MIT

//! Kernel objects the driver holds references to: events and processes.

use core::{ffi::c_void, ptr::NonNull};

use ntstatus::NtStatus;
use wdk_sys::{
    ntddk::{
        IoGetCurrentProcess, ObOpenObjectByPointer, ObReferenceObjectByHandle,
        ObfDereferenceObject, ObfReferenceObject, ZwClose,
    },
    ExEventObjectType, PsProcessType, EVENT_MODIFY_STATE, HANDLE, OBJ_KERNEL_HANDLE, SYNCHRONIZE,
};

/// `PROCESS_VM_OPERATION`: the right needed to unmap a view from a process.
const PROCESS_VM_OPERATION: u32 = 0x0008;

/// A referenced kernel event object.
///
/// The driver never signals or waits on it; it only carries the event from
/// the process that registered it to the process that will signal it.
pub struct EventObject {
    object: NonNull<c_void>,
}

// SAFETY: a referenced kernel object may be used and released from any
// thread.
unsafe impl Send for EventObject {}
// SAFETY: as above; the type has no interior state of its own.
unsafe impl Sync for EventObject {}

impl EventObject {
    /// Resolves `handle` as a handle of the process the calling thread runs
    /// in, with that process's own rights: it must be in the process's own
    /// handle table, name an event object and grant the "modify state"
    /// right. Fails with the object manager's status otherwise.
    ///
    /// The lookup is always made with user access, and there is no way to
    /// ask for another. A handle value that arrives in a request is the
    /// requesting process's, whatever the request says about its origin
    /// (requests from 32-bit processes arrive marked as kernel-mode ones). A
    /// lookup with kernel access would skip the access check and accept the
    /// value of a kernel handle.
    pub fn from_user_handle(handle: u64) -> Result<Self, NtStatus> {
        let mut object: *mut c_void = core::ptr::null_mut();
        // SAFETY: `handle` is an untrusted value that the object manager
        // validates, as a user-mode caller's, against the handle table, the
        // required access and the event object type. `ExEventObjectType` is
        // a kernel export that points at the type object. `object` receives
        // a referenced pointer.
        let status = unsafe {
            ObReferenceObjectByHandle(
                handle as usize as HANDLE,
                EVENT_MODIFY_STATE,
                *ExEventObjectType,
                wdk_sys::_MODE::UserMode as i8,
                &mut object,
                core::ptr::null_mut(),
            )
        };
        crate::check(status)?;
        NonNull::new(object)
            .map(|object| Self { object })
            .ok_or(NtStatus::UNSUCCESSFUL)
    }

    /// Creates a handle to the event in the current process: not
    /// inheritable, granting exactly "modify state" and "synchronize".
    /// The process owns the handle from then on.
    pub fn open_handle_in_current_process(&self) -> Result<u64, NtStatus> {
        let mut handle: HANDLE = core::ptr::null_mut();
        // SAFETY: `object` is a referenced event object. No OBJ_KERNEL_HANDLE
        // attribute is passed, so the handle is created in the current
        // process's table; kernel access mode skips the access check, which
        // is the point: the driver vouches for the host.
        let status = unsafe {
            ObOpenObjectByPointer(
                self.object.as_ptr(),
                0,
                core::ptr::null_mut(),
                EVENT_MODIFY_STATE | SYNCHRONIZE,
                *ExEventObjectType,
                wdk_sys::_MODE::KernelMode as i8,
                &mut handle,
            )
        };
        crate::check(status)?;
        Ok(handle as usize as u64)
    }
}

impl Drop for EventObject {
    fn drop(&mut self) {
        // SAFETY: releases the reference taken in `from_user_handle`.
        unsafe { ObfDereferenceObject(self.object.as_ptr()) };
    }
}

/// A process, held by reference and by a kernel handle, so that a view can
/// be removed from it later from any context.
pub struct Process {
    object: NonNull<c_void>,
    handle: HANDLE,
}

// SAFETY: the referenced object and the kernel handle (valid in every
// process context) may be used and released from any thread.
unsafe impl Send for Process {}
// SAFETY: as above.
unsafe impl Sync for Process {}

impl Process {
    /// The process the calling thread is running in.
    pub fn current() -> Result<Self, NtStatus> {
        // SAFETY: always valid to call; returns the current process.
        let process = unsafe { IoGetCurrentProcess() }.cast::<c_void>();
        let object = NonNull::new(process).ok_or(NtStatus::UNSUCCESSFUL)?;
        let mut handle: HANDLE = core::ptr::null_mut();
        // SAFETY: `process` is the current process object, which is alive
        // while one of its threads runs. A kernel handle is requested.
        let status = unsafe {
            ObOpenObjectByPointer(
                process,
                OBJ_KERNEL_HANDLE,
                core::ptr::null_mut(),
                PROCESS_VM_OPERATION,
                *PsProcessType,
                wdk_sys::_MODE::KernelMode as i8,
                &mut handle,
            )
        };
        crate::check(status)?;
        // SAFETY: the object is alive (see above); take our own reference so
        // the pointer stays unique to this process for comparisons.
        unsafe { ObfReferenceObject(process) };
        Ok(Self { object, handle })
    }

    /// Whether this is the process the calling thread is running in.
    pub fn is_current(&self) -> bool {
        // SAFETY: always valid to call.
        let current = unsafe { IoGetCurrentProcess() }.cast::<c_void>();
        current == self.object.as_ptr()
    }

    pub(crate) fn handle(&self) -> HANDLE {
        self.handle
    }
}

impl Drop for Process {
    fn drop(&mut self) {
        // SAFETY: the handle and the reference were acquired in `current`
        // and are released exactly once.
        unsafe {
            let _ = ZwClose(self.handle);
            ObfDereferenceObject(self.object.as_ptr());
        }
    }
}
