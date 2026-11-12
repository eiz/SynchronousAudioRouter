// Copyright (c) 2026 Itty Bitty Binaries, Inc.
// SPDX-License-Identifier: MIT

//! Pagefile-backed shared memory sections and views of them.

use core::{
    ffi::c_void,
    ptr::NonNull,
    sync::atomic::{AtomicU32, Ordering},
};

use ntstatus::NtStatus;
use ntx_sys::seh::SarShimProbeAndLockPages;
use wdk_sys::{
    ntddk::{
        IoAllocateMdl, IoFreeMdl, MmMapLockedPagesSpecifyCache, MmMapViewInSystemSpaceEx,
        MmUnlockPages, MmUnmapLockedPages, MmUnmapViewInSystemSpace, ObReferenceObjectByHandle,
        ObfDereferenceObject, ZwClose, ZwCreateSection, ZwMapViewOfSection, ZwUnmapViewOfSection,
    },
    MdlMappingNoExecute, HANDLE, LARGE_INTEGER, MDL, OBJECT_ATTRIBUTES, OBJ_KERNEL_HANDLE,
    PAGE_READWRITE, SECTION_ALL_ACCESS, SEC_COMMIT,
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

    /// Maps `size` bytes starting at `offset` into system address space.
    /// `offset` must be a multiple of 65,536. The view is pageable: it is
    /// only good for getting at the section's pages in order to lock them
    /// ([`SystemView::lock_pages`]).
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

/// A view of a section in system address space. Like any view it is
/// pageable, so nothing is ever read or written through it; it exists so
/// that the pages behind it can be locked.
pub struct SystemView {
    base: NonNull<u8>,
    size: usize,
}

// SAFETY: system address space is valid in every context; the view is only
// ever handed to the memory manager.
unsafe impl Send for SystemView {}
// SAFETY: as above.
unsafe impl Sync for SystemView {}

impl SystemView {
    /// Makes the pages behind `length` bytes of the view, starting at byte
    /// `offset`, resident and locks them in physical memory: they cannot be
    /// paged out until the result is dropped, whatever becomes of this view
    /// or of any other. Both numbers must be multiples of the page size.
    ///
    /// Fails with `STATUS_INSUFFICIENT_RESOURCES` if the pages cannot be
    /// locked. Must be called at `PASSIVE_LEVEL`.
    pub fn lock_pages(&self, offset: usize, length: usize) -> Result<LockedPages, NtStatus> {
        const PAGE_MASK: usize = 4096 - 1;
        let in_view = offset.checked_add(length).is_some_and(|end| end <= self.size);
        let whole_pages = (offset | length) & PAGE_MASK == 0;
        let length32 = u32::try_from(length)
            .ok()
            .filter(|_| in_view && whole_pages && length != 0);
        let Some(length32) = length32 else {
            return Err(NtStatus::INVALID_PARAMETER);
        };
        // SAFETY: the range lies inside the live view (checked above). No
        // IRP is involved; the MDL belongs to the caller until freed.
        let mdl = unsafe {
            IoAllocateMdl(
                self.base.as_ptr().add(offset).cast(),
                length32,
                0,
                0,
                core::ptr::null_mut(),
            )
        };
        let Some(mdl) = NonNull::new(mdl) else {
            return Err(NtStatus::INSUFFICIENT_RESOURCES);
        };
        // SAFETY: a fresh, unlocked MDL over valid (pageable) kernel
        // addresses, at PASSIVE_LEVEL. The shim turns the exception
        // MmProbeAndLockPages raises on failure into a status.
        let status = unsafe {
            SarShimProbeAndLockPages(
                mdl.as_ptr(),
                wdk_sys::_MODE::KernelMode as i8,
                wdk_sys::_LOCK_OPERATION::IoModifyAccess,
            )
        };
        if !NtStatus(status).is_success() {
            // SAFETY: the MDL was allocated above and is not locked.
            unsafe { IoFreeMdl(mdl.as_ptr()) };
            return Err(NtStatus::INSUFFICIENT_RESOURCES);
        }
        Ok(LockedPages { mdl, length })
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

/// Physical pages of a section, locked in memory (an MDL in the locked
/// state). The lock is on the pages themselves, not on any view of them: it
/// holds while views come and go in whatever process, and ends when this
/// value is dropped.
pub struct LockedPages {
    mdl: NonNull<MDL>,
    length: usize,
}

// SAFETY: a locked MDL may be used and released from any thread and process
// context; the type exposes no access to the memory.
unsafe impl Send for LockedPages {}
// SAFETY: as above.
unsafe impl Sync for LockedPages {}

impl LockedPages {
    /// Number of bytes locked.
    pub fn len(&self) -> usize {
        self.length
    }

    pub fn is_empty(&self) -> bool {
        self.length == 0
    }

    /// Maps the locked pages into system address space, not executable.
    /// Such a mapping is of physical pages that cannot go away, so an access
    /// through it cannot take a page fault of any kind.
    pub fn map(self) -> Result<LockedView, NtStatus> {
        // SAFETY: the MDL is locked and has no system mapping yet (this
        // method consumes the only value that could make one). With
        // BugCheckOnFailure false the routine returns null on failure.
        let base = unsafe {
            MmMapLockedPagesSpecifyCache(
                self.mdl.as_ptr(),
                wdk_sys::_MODE::KernelMode as i8,
                wdk_sys::_MEMORY_CACHING_TYPE::MmCached,
                core::ptr::null_mut(),
                0,
                wdk_sys::_MM_PAGE_PRIORITY::NormalPagePriority as u32 | MdlMappingNoExecute,
            )
        };
        match NonNull::new(base.cast::<u8>()) {
            Some(base) => Ok(LockedView { base, pages: self }),
            // Dropping `self` unlocks the pages again.
            None => Err(NtStatus::INSUFFICIENT_RESOURCES),
        }
    }
}

impl Drop for LockedPages {
    fn drop(&mut self) {
        // SAFETY: the MDL was locked by `SystemView::lock_pages` and is
        // unlocked and freed exactly once; any mapping of it has been
        // removed by `LockedView`'s destructor, which runs first.
        unsafe {
            MmUnlockPages(self.mdl.as_ptr());
            IoFreeMdl(self.mdl.as_ptr());
        }
    }
}

/// Locked pages of a section mapped into system address space, accessed as
/// 32-bit words. An access cannot fault, so it may be made under any lock
/// and at any IRQL.
///
/// The same memory is mapped writable into user processes, so it can change
/// at any moment. All access is therefore by volatile or atomic operations
/// on raw pointers; no reference to the memory is ever formed, and nothing
/// read from it may be trusted.
pub struct LockedView {
    base: NonNull<u8>,
    // After `base` on purpose: fields are dropped in order, and this type's
    // destructor removes the mapping before the pages are unlocked.
    pages: LockedPages,
}

// SAFETY: system address space is valid in every context, and all access is
// by volatile/atomic word operations that tolerate concurrent use.
unsafe impl Send for LockedView {}
// SAFETY: as above.
unsafe impl Sync for LockedView {}

impl LockedView {
    pub fn len(&self) -> usize {
        self.pages.len()
    }

    pub fn is_empty(&self) -> bool {
        self.pages.is_empty()
    }

    fn word(&self, offset: usize) -> *mut u32 {
        assert!(
            offset % 4 == 0 && offset.checked_add(4).is_some_and(|end| end <= self.pages.len()),
            "register access outside the mapped view"
        );
        // SAFETY: the offset was just checked to lie inside the mapping.
        unsafe { self.base.as_ptr().add(offset).cast::<u32>() }
    }

    /// Reads the word at byte `offset`. Panics if it is outside the view or
    /// misaligned.
    pub fn read_u32(&self, offset: usize) -> u32 {
        // SAFETY: `word` returns an aligned pointer inside the live mapping.
        unsafe { self.word(offset).read_volatile() }
    }

    /// Writes the word at byte `offset`.
    pub fn write_u32(&self, offset: usize, value: u32) {
        // SAFETY: `word` returns an aligned pointer inside the live mapping.
        unsafe { self.word(offset).write_volatile(value) }
    }

    /// Stores the word at byte `offset` with a sequentially consistent
    /// atomic store: a full memory barrier and a single store.
    pub fn publish_u32(&self, offset: usize, value: u32) {
        // SAFETY: `word` returns an aligned pointer inside the live mapping,
        // valid for the duration of the call; atomic access is the correct
        // way to touch memory that other processes modify concurrently.
        unsafe { AtomicU32::from_ptr(self.word(offset)) }.store(value, Ordering::SeqCst);
    }
}

impl Drop for LockedView {
    fn drop(&mut self) {
        // SAFETY: `base` is the address MmMapLockedPagesSpecifyCache
        // returned for this MDL, unmapped exactly once, before the MDL is
        // unlocked (the `pages` field is dropped after this runs).
        unsafe { MmUnmapLockedPages(self.base.as_ptr().cast(), self.pages.mdl.as_ptr()) };
    }
}
