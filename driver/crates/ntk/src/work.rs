// Copyright (c) 2026 Itty Bitty Binaries, Inc.
// SPDX-License-Identifier: MIT

//! System worker threads.

use alloc::boxed::Box;
use core::{ffi::c_void, ptr::NonNull};

use ntstatus::NtStatus;
use wdk_sys::{
    ntddk::{IoAllocateWorkItem, IoFreeWorkItem, IoQueueWorkItemEx, ObfDereferenceObject, ObfReferenceObject},
    DEVICE_OBJECT, PIO_WORKITEM, _IO_WORKITEM as IO_WORKITEM,
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
///
/// A work item refers to its device object, and the routine is handed it.
/// So the work item holds a reference to the device object from the moment
/// it is allocated until it is freed unqueued or its routine has run:
/// however long it waits, and whatever happens to the device meanwhile, the
/// device object it queues on and passes to the routine is still there.
pub struct WorkItem {
    raw: NonNull<IO_WORKITEM>,
    /// The device object, with a reference this value owns.
    device: NonNull<DEVICE_OBJECT>,
}

// SAFETY: an unqueued work item is plain memory owned by this value, and an
// object reference may be released on any thread.
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
        let raw = NonNull::new(raw).ok_or(NtStatus::INSUFFICIENT_RESOURCES)?;
        // SAFETY: `device` is a valid device object; the reference is this
        // value's, released when it is dropped or by the routine it queues.
        unsafe { ObfReferenceObject(device.as_raw().cast()) };
        Ok(Self {
            raw,
            device: NonNull::from(device).cast(),
        })
    }

    /// Queues `R::run(&context)` to the delayed work queue.
    pub fn queue<R: WorkRoutine>(self, context: Shared<R::Context>) {
        let raw = self.raw.as_ptr();
        core::mem::forget(self);
        // SAFETY: `raw` is an unqueued work item; `routine::<R>` matches
        // IO_WORKITEM_ROUTINE_EX, takes back the context reference passed
        // here exactly once and frees the work item. The reference to the
        // device object goes with it: the routine releases it.
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
        // exactly once and frees the work item. The reference to the device
        // object goes with it: the routine releases it.
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
        // queued (queuing forgets `self`). The reference to the device object
        // is this value's, released once.
        unsafe {
            IoFreeWorkItem(self.raw.as_ptr());
            ObfDereferenceObject(self.device.as_ptr().cast());
        }
    }
}

unsafe extern "C" fn routine<R: WorkRoutine>(io_object: *mut c_void, context: *mut c_void, item: PIO_WORKITEM) {
    // SAFETY: `context` is the reference `queue` converted with
    // `into_context`, delivered exactly once.
    let context = unsafe { Shared::<R::Context>::from_context(context) };
    // SAFETY: the I/O object of a work item from IoAllocateWorkItem is the
    // device object it was allocated for, which the work item's own
    // reference keeps in existence until it is released below.
    let device = unsafe { DeviceObject::from_raw(io_object.cast()) };
    R::run(device, &context);
    drop(context);
    // SAFETY: the work item passed to its own routine may be freed there;
    // nothing else refers to it. The reference to the device object is the
    // one `WorkItem::try_new` took and `queue` handed over, released once;
    // `device` is not used afterwards.
    unsafe {
        IoFreeWorkItem(item);
        ObfDereferenceObject(io_object);
    }
}

unsafe extern "C" fn drop_routine<T>(io_object: *mut c_void, context: *mut c_void, item: PIO_WORKITEM) {
    // SAFETY: `context` is the box `queue_drop` converted with `into_raw`,
    // delivered exactly once.
    drop(unsafe { Box::from_raw(context.cast::<T>()) });
    // SAFETY: as at the end of `routine`: the work item is freed in its own
    // routine, and the device object reference `queue_drop` handed over is
    // released once.
    unsafe {
        IoFreeWorkItem(item);
        ObfDereferenceObject(io_object);
    }
}
