// Copyright (c) 2026 Itty Bitty Binaries, Inc.
// SPDX-License-Identifier: MIT

//! Pagefile-backed shared memory sections and views of them.

use core::{
    ffi::c_void,
    ptr::NonNull,
    sync::atomic::{AtomicU32, Ordering},
};

use ntstatus::NtStatus;
use wdk_sys::{
    ntddk::{
        MmMapViewInSystemSpaceEx, MmUnmapViewInSystemSpace, ObReferenceObjectByHandle,
        ObfDereferenceObject, ZwClose, ZwCreateSection, ZwMapViewOfSection, ZwUnmapViewOfSection,
    },
    HANDLE, LARGE_INTEGER, OBJECT_ATTRIBUTES, OBJ_KERNEL_HANDLE, PAGE_READWRITE,
    SECTION_ALL_ACCESS, SEC_COMMIT,
};

use crate::object::Process;

/// The pseudo-handle for the current process (`NtCurrentProcess()`).
const CURRENT_PROCESS: HANDLE = usize::MAX as HANDLE;

/// A section of zero-initialised, pagefile-backed memory. It has no name.
pub struct Section {
    handle: HANDLE,
    object: NonNull<c_void>,
}

// SAFETY: a kernel handle and a referenced object pointer are valid in every
// thread and process context.
unsafe impl Send for Section {}
// SAFETY: as above; all methods take `&self` and only call thread-safe
// kernel routines.
unsafe impl Sync for Section {}

impl Section {
    /// Creates a readable and writable section of `size` bytes.
    pub fn create(size: u64) -> Result<Self, NtStatus> {
        let mut attributes = OBJECT_ATTRIBUTES {
            Length: core::mem::size_of::<OBJECT_ATTRIBUTES>() as u32,
            RootDirectory: core::ptr::null_mut(),
            ObjectName: core::ptr::null_mut(),
            Attributes: OBJ_KERNEL_HANDLE,
            SecurityDescriptor: core::ptr::null_mut(),
            SecurityQualityOfService: core::ptr::null_mut(),
        };
        let mut maximum_size = LARGE_INTEGER {
            QuadPart: i64::try_from(size).map_err(|_| NtStatus::INVALID_PARAMETER)?,
        };
        let mut handle: HANDLE = core::ptr::null_mut();
        // SAFETY: all pointers refer to initialised locals; a null file
        // handle asks for a pagefile-backed section.
        let status = unsafe {
            ZwCreateSection(
                &mut handle,
                SECTION_ALL_ACCESS,
                &mut attributes,
                &mut maximum_size,
                PAGE_READWRITE,
                SEC_COMMIT,
                core::ptr::null_mut(),
            )
        };
        crate::check(status)?;

        let mut object: *mut c_void = core::ptr::null_mut();
        // SAFETY: `handle` is the kernel handle just created. A null object
        // type is permitted for kernel-mode access.
        let status = unsafe {
            ObReferenceObjectByHandle(
                handle,
                SECTION_ALL_ACCESS,
                core::ptr::null_mut(),
                wdk_sys::_MODE::KernelMode as i8,
                &mut object,
                core::ptr::null_mut(),
            )
        };
        match (crate::check(status), NonNull::new(object)) {
            (Ok(()), Some(object)) => Ok(Self { handle, object }),
            (result, _) => {
                // SAFETY: closes the handle created above, once.
                unsafe {
                    let _ = ZwClose(handle);
                }
                Err(result.err().unwrap_or(NtStatus::UNSUCCESSFUL))
            }
        }
    }

    /// Maps `size` bytes starting at `offset`, readable and writable, into
    /// the process the calling thread runs in. `offset` must be a multiple
    /// of 65,536 (the system's allocation granularity).
    pub fn map_into_current_process(&self, offset: u64, size: usize) -> Result<UserView, NtStatus> {
        let process = Process::current()?;
        let mut base: *mut c_void = core::ptr::null_mut();
        let mut section_offset = LARGE_INTEGER {
            QuadPart: i64::try_from(offset).map_err(|_| NtStatus::INVALID_PARAMETER)?,
        };
        let mut view_size: u64 = size as u64;
        // SAFETY: `handle` is a valid section handle; the out-parameters
        // refer to initialised locals. The memory manager chooses the
        // address (base is null) inside the current process.
        let status = unsafe {
            ZwMapViewOfSection(
                self.handle,
                CURRENT_PROCESS,
                &mut base,
                0,
                0,
                &mut section_offset,
                &mut view_size,
                wdk_sys::_SECTION_INHERIT::ViewUnmap,
                0,
                PAGE_READWRITE,
            )
        };
        crate::check(status)?;
        Ok(UserView {
            process,
            base: base as usize,
        })
    }

    /// Maps `size` bytes starting at `offset` into system address space, so
    /// that the driver can reach them from any process context. `offset`
    /// must be a multiple of 65,536.
    pub fn map_into_system_space(&self, offset: u64, size: usize) -> Result<SystemView, NtStatus> {
        let mut base: *mut c_void = core::ptr::null_mut();
        let mut section_offset = LARGE_INTEGER {
            QuadPart: i64::try_from(offset).map_err(|_| NtStatus::INVALID_PARAMETER)?,
        };
        let mut view_size: u64 = size as u64;
        // SAFETY: `object` is a referenced section object; the
        // out-parameters refer to initialised locals.
        let status = unsafe {
            MmMapViewInSystemSpaceEx(
                self.object.as_ptr(),
                &mut base,
                &mut view_size,
                &mut section_offset,
                0,
            )
        };
        crate::check(status)?;
        let base = NonNull::new(base.cast::<u8>()).ok_or(NtStatus::UNSUCCESSFUL)?;
        if (view_size as usize) < size {
            // SAFETY: unmaps the view mapped just above.
            unsafe {
                let _ = MmUnmapViewInSystemSpace(base.as_ptr().cast());
            }
            return Err(NtStatus::UNSUCCESSFUL);
        }
        Ok(SystemView { base, size })
    }
}

impl Drop for Section {
    fn drop(&mut self) {
        // SAFETY: releases the reference and the handle acquired in
        // `create`, once. Existing views keep the memory alive on their own.
        unsafe {
            ObfDereferenceObject(self.object.as_ptr());
            let _ = ZwClose(self.handle);
        }
    }
}

/// A view of a section in a user process. Dropping it removes the view from
/// that process, from whatever context the drop happens in.
///
/// The driver never reads or writes through a user view; it only reports
/// its address.
pub struct UserView {
    process: Process,
    base: usize,
}

impl UserView {
    /// The address of the view in its process.
    pub fn address(&self) -> u64 {
        self.base as u64
    }

    /// Whether the view is in the process the calling thread runs in.
    pub fn is_in_current_process(&self) -> bool {
        self.process.is_current()
    }

    /// Gives up the view without unmapping it: it stays in the process
    /// until the process unmaps it or exits.
    pub fn leave_mapped(self) {
        let mut this = core::mem::ManuallyDrop::new(self);
        // SAFETY: `this` is never used again and its destructor is
        // suppressed, so the process reference is released exactly once.
        unsafe { core::ptr::drop_in_place(&mut this.process) };
    }
}

impl Drop for UserView {
    fn drop(&mut self) {
        // SAFETY: the handle names the process the view was mapped in.
        // Unmapping an address in a user process cannot affect kernel
        // memory; if the process has exited or already removed the view,
        // the call fails harmlessly.
        unsafe {
            let _ = ZwUnmapViewOfSection(self.process.handle(), self.base as *mut c_void);
        }
    }
}

/// A view of a section in system address space, accessed as 32-bit words.
///
/// The same memory is mapped writable into user processes, so it can change
/// at any moment. All access is therefore by volatile or atomic operations
/// on raw pointers; no reference to the memory is ever formed, and nothing
/// read from it may be trusted.
pub struct SystemView {
    base: NonNull<u8>,
    size: usize,
}

// SAFETY: system address space is valid in every context, and all access is
// by volatile/atomic word operations that tolerate concurrent use.
unsafe impl Send for SystemView {}
// SAFETY: as above.
unsafe impl Sync for SystemView {}

impl SystemView {
    pub fn len(&self) -> usize {
        self.size
    }

    pub fn is_empty(&self) -> bool {
        self.size == 0
    }

    fn word(&self, offset: usize) -> *mut u32 {
        assert!(
            offset % 4 == 0 && offset.checked_add(4).is_some_and(|end| end <= self.size),
            "register access outside the mapped view"
        );
        // SAFETY: the offset was just checked to lie inside the view.
        unsafe { self.base.as_ptr().add(offset).cast::<u32>() }
    }

    /// Reads the word at byte `offset`. Panics if it is outside the view or
    /// misaligned. Must be called below `DISPATCH_LEVEL` (the memory is
    /// pageable).
    pub fn read_u32(&self, offset: usize) -> u32 {
        // SAFETY: `word` returns an aligned pointer inside the live view.
        unsafe { self.word(offset).read_volatile() }
    }

    /// Writes the word at byte `offset`.
    pub fn write_u32(&self, offset: usize, value: u32) {
        // SAFETY: `word` returns an aligned pointer inside the live view.
        unsafe { self.word(offset).write_volatile(value) }
    }

    /// Stores the word at byte `offset` with a sequentially consistent
    /// atomic store: a full memory barrier and a single store.
    pub fn publish_u32(&self, offset: usize, value: u32) {
        // SAFETY: `word` returns an aligned pointer inside the live view,
        // valid for the duration of the call; atomic access is the correct
        // way to touch memory that other processes modify concurrently.
        unsafe { AtomicU32::from_ptr(self.word(offset)) }.store(value, Ordering::SeqCst);
    }
}

impl Drop for SystemView {
    fn drop(&mut self) {
        // SAFETY: `base` is the address MmMapViewInSystemSpaceEx returned
        // and is unmapped exactly once.
        unsafe {
            let _ = MmUnmapViewInSystemSpace(self.base.as_ptr().cast());
        }
    }
}
