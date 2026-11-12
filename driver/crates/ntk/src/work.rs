// Copyright (c) 2026 Itty Bitty Binaries, Inc.
// SPDX-License-Identifier: MIT

//! System worker threads.

use alloc::boxed::Box;
use core::{ffi::c_void, ptr::NonNull};

use ntstatus::NtStatus;
use wdk_sys::{
    ntddk::{IoAllocateWorkItem, IoFreeWorkItem, IoQueueWorkItemEx},
    PIO_WORKITEM, _IO_WORKITEM as IO_WORKITEM,
};

use crate::{alloc_ext::Shared, device::DeviceObject};

/// Something to run on a system worker thread.
pub trait WorkRoutine: 'static {
    type Context: Send + Sync + 'static;

    /// Runs at `PASSIVE_LEVEL` in the system process. `device` is the
    /// device object the work item was allocated for.
    fn run(device: &DeviceObject, context: &Self::Context);
}

/// An allocated, not yet queued work item. Allocating it up front means
/// that queuing cannot fail later, when it may be too late to report an
/// error.
pub struct WorkItem {
    raw: NonNull<IO_WORKITEM>,
}

// SAFETY: an unqueued work item is plain memory owned by this value.
unsafe impl Send for WorkItem {}
// SAFETY: nothing can be done with a shared reference to one: queuing and
// dropping both take the value.
unsafe impl Sync for WorkItem {}

impl WorkItem {
    /// Allocates a work item tied to `device`: the device object (and with
    /// it the driver image) stays loaded until the routine has returned.
    pub fn try_new(device: &DeviceObject) -> Result<Self, NtStatus> {
        // SAFETY: `device` is a valid device object.
        let raw = unsafe { IoAllocateWorkItem(device.as_raw()) };
        NonNull::new(raw)
            .map(|raw| Self { raw })
            .ok_or(NtStatus::INSUFFICIENT_RESOURCES)
    }

    /// Queues `R::run(&context)` to the delayed work queue.
    pub fn queue<R: WorkRoutine>(self, context: Shared<R::Context>) {
        let raw = self.raw.as_ptr();
        core::mem::forget(self);
        // SAFETY: `raw` is an unqueued work item; `routine::<R>` matches
        // IO_WORKITEM_ROUTINE_EX, takes back the context reference passed
        // here exactly once and frees the work item.
        unsafe {
            IoQueueWorkItemEx(
                raw,
                Some(routine::<R>),
                wdk_sys::_WORK_QUEUE_TYPE::DelayedWorkQueue,
                context.into_context(),
            );
        }
    }
}

impl WorkItem {
    /// Queues the dropping of `value` to the delayed work queue: its
    /// destructor runs on a system worker thread, at `PASSIVE_LEVEL` and
    /// with no lock of the caller's held, however little the caller may be
    /// able to afford just now. May be called up to `DISPATCH_LEVEL`.
    pub fn queue_drop<T: Send + 'static>(self, value: Box<T>) {
        let raw = self.raw.as_ptr();
        core::mem::forget(self);
        // SAFETY: `raw` is an unqueued work item; `drop_routine::<T>`
        // matches IO_WORKITEM_ROUTINE_EX, takes back the box passed here
        // exactly once and frees the work item.
        unsafe {
            IoQueueWorkItemEx(
                raw,
                Some(drop_routine::<T>),
                wdk_sys::_WORK_QUEUE_TYPE::DelayedWorkQueue,
                Box::into_raw(value).cast(),
            );
        }
    }
}

impl Drop for WorkItem {
    fn drop(&mut self) {
        // SAFETY: the item was allocated by IoAllocateWorkItem and never
        // queued (queuing forgets `self`).
        unsafe { IoFreeWorkItem(self.raw.as_ptr()) };
    }
}

unsafe extern "C" fn routine<R: WorkRoutine>(io_object: *mut c_void, context: *mut c_void, item: PIO_WORKITEM) {
    // SAFETY: `context` is the reference `queue` converted with
    // `into_context`, delivered exactly once.
    let context = unsafe { Shared::<R::Context>::from_context(context) };
    // SAFETY: the I/O object of a work item from IoAllocateWorkItem is the
    // device object it was allocated for, which the I/O manager keeps
    // referenced until this routine returns.
    let device = unsafe { DeviceObject::from_raw(io_object.cast()) };
    R::run(device, &context);
    drop(context);
    // SAFETY: the work item passed to its own routine may be freed there;
    // nothing else refers to it.
    unsafe { IoFreeWorkItem(item) };
}

unsafe extern "C" fn drop_routine<T>(_io_object: *mut c_void, context: *mut c_void, item: PIO_WORKITEM) {
    // SAFETY: `context` is the box `queue_drop` converted with `into_raw`,
    // delivered exactly once.
    drop(unsafe { Box::from_raw(context.cast::<T>()) });
    // SAFETY: the work item passed to its own routine may be freed there;
    // nothing else refers to it.
    unsafe { IoFreeWorkItem(item) };
}
