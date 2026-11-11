// Copyright (c) 2026 Itty Bitty Binaries, Inc.
// SPDX-License-Identifier: MIT

//! The driver's entry point: the one place in this crate that has to be
//! `unsafe`, because the I/O manager calls it with raw pointers and the
//! symbol has to be exported under a fixed name.
#![allow(unsafe_code)]

use ks::device::RegistryPath;
use ntk::device::DriverObject;
use wdk_sys::{NTSTATUS, PCUNICODE_STRING, PDRIVER_OBJECT};

/// `DriverEntry`, called once by the I/O manager when the driver is loaded
/// (through `GsDriverEntry`, which first sets up the stack cookie).
///
/// # Safety
///
/// Must only be called by the I/O manager, with the driver object and
/// registry path of this driver.
#[export_name = "DriverEntry"]
pub unsafe extern "system" fn driver_entry(driver: PDRIVER_OBJECT, registry_path: PCUNICODE_STRING) -> NTSTATUS {
    // SAFETY: these are the arguments the I/O manager passed to DriverEntry,
    // and both wrappers are dropped before this function returns.
    let (mut driver, registry_path) = unsafe {
        (
            DriverObject::from_raw(driver),
            RegistryPath::from_raw(registry_path),
        )
    };
    match crate::device::driver_entry(&mut driver, &registry_path) {
        Ok(()) => ntstatus::NtStatus::SUCCESS.0,
        Err(status) => status.0,
    }
}
