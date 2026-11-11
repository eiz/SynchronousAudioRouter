// Copyright (c) 2026 Itty Bitty Binaries, Inc.
// SPDX-License-Identifier: MIT

//! Device and driver objects.

use wdk_sys::{DEVICE_OBJECT, DRIVER_OBJECT, PDEVICE_OBJECT, PDRIVER_OBJECT};

/// A device object owned by the I/O manager.
#[repr(transparent)]
pub struct DeviceObject(DEVICE_OBJECT);

impl DeviceObject {
    /// # Safety
    ///
    /// `raw` must point to a device object that stays valid for `'a`.
    pub unsafe fn from_raw<'a>(raw: PDEVICE_OBJECT) -> &'a Self {
        // SAFETY: `DeviceObject` is a transparent wrapper, and the caller
        // guarantees validity for `'a`. Only shared access is ever taken.
        unsafe { &*raw.cast::<Self>() }
    }

    pub fn as_raw(&self) -> PDEVICE_OBJECT {
        core::ptr::from_ref(self).cast_mut().cast()
    }
}

/// The driver object, as handed to `DriverEntry`.
pub struct DriverObject {
    raw: PDRIVER_OBJECT,
}

impl DriverObject {
    /// # Safety
    ///
    /// `raw` must be the driver object the I/O manager passed to this
    /// driver's `DriverEntry`, and the wrapper must not outlive that call.
    pub unsafe fn from_raw(raw: PDRIVER_OBJECT) -> Self {
        Self { raw }
    }

    pub fn as_raw(&self) -> PDRIVER_OBJECT {
        self.raw
    }

    pub(crate) fn object(&mut self) -> &mut DRIVER_OBJECT {
        // SAFETY: valid for the duration of DriverEntry by `from_raw`'s
        // contract, during which nothing else touches the dispatch table
        // (no device exists yet).
        unsafe { &mut *self.raw }
    }

    /// Drops the driver's fast I/O table, if a framework installed one, so
    /// that every request reaches the dispatch routines as an IRP.
    ///
    /// Fast I/O entry points receive the file object without going through
    /// the dispatch table, so a framework's fast I/O routine would be handed
    /// file objects that [`crate::dispatch`] claims for this driver's own
    /// handler and that the framework knows nothing about.
    pub fn disable_fast_io(&mut self) {
        self.object().FastIoDispatch = core::ptr::null_mut();
    }
}
