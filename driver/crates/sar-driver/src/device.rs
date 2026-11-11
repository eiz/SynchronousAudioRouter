// Copyright (c) 2026 Itty Bitty Binaries, Inc.
// SPDX-License-Identifier: MIT

//! Driver-wide and per-device state, and the routing of requests between
//! the control interface and kernel streaming (sections 3 and 4).
#![forbid(unsafe_code)]

use alloc::collections::VecDeque;

use ks::device::{Device, DeviceHandler, RegistryPath};
use ntk::{
    alloc_ext::Shared,
    device::{DeviceObject, DriverObject},
    dispatch::FileHandler,
    interface::DeviceInterface,
    irp::{DispatchResult, Irp},
    registry::{DriverHandle, RegistryCallback},
    sync::{Global, Mutex},
};
use ntstatus::NtStatus;
use sar_core::{
    control::{is_control_file_name, CONTROL_INTERFACE_CLASS, CONTROL_REFERENCE_STRING},
    endpoint::WideString,
};

use crate::{control::ControlHandle, publish::PublishJob, regfilter::RoutingFilter};

/// State that exists once per loaded driver.
pub struct DriverState {
    pub handle: DriverHandle,
    /// The application-routing registry filter, once a host has started it.
    /// It stays until the driver unloads (section 10, "Lifetime").
    pub registry_filter: Mutex<Option<RegistryCallback<RoutingFilter>>>,
}

pub static DRIVER: Global<DriverState> = Global::new();

/// State of the SAR device.
pub struct DeviceState {
    /// The control device interface, while the device is started.
    control_interface: Mutex<Option<DeviceInterface>>,
    /// Endpoints waiting to be published, oldest first (section 5: several
    /// create requests "are finished in the order they were made").
    pub publish_queue: Mutex<VecDeque<PublishJob>>,
}

/// The type the kernel-facing traits are implemented on.
pub struct Sar;

/// The body of `DriverEntry`.
pub fn driver_entry(driver: &mut DriverObject, registry_path: &RegistryPath) -> Result<(), NtStatus> {
    let state = Shared::try_new(DriverState {
        handle: DriverHandle::new(driver),
        registry_filter: Mutex::try_new(None)?,
    })?;

    // Kernel streaming owns the device object, Plug and Play, power and the
    // dispatch of every request (section 4). All pool this driver allocates
    // is non-executable (`ExAllocatePool2`).
    ks::device::initialize_driver::<Sar>(driver, registry_path)?;

    // In front of it: requests on files opened with the control reference
    // string are handled here, everything else is passed on unchanged.
    driver.disable_fast_io();
    ntk::dispatch::install_file_handler::<Sar>(driver);

    drop(DRIVER.set(state));
    Ok(())
}

impl DeviceHandler for Sar {
    type Context = DeviceState;

    fn add(_device: &Device) -> Result<Shared<DeviceState>, NtStatus> {
        Shared::try_new(DeviceState {
            control_interface: Mutex::try_new(None)?,
            publish_queue: Mutex::try_new(VecDeque::new())?,
        })
    }

    /// "While the device is started, the driver publishes one device
    /// interface for control" (section 3).
    fn start(device: &Device, state: &Shared<DeviceState>) -> Result<(), NtStatus> {
        let reference =
            WideString::<64>::from_ascii(CONTROL_REFERENCE_STRING).ok_or(NtStatus::UNSUCCESSFUL)?;
        let class = ntk::guid_from_bytes(CONTROL_INTERFACE_CLASS);
        let interface =
            DeviceInterface::register(device.physical_device_object(), &class, Some(reference.as_units()))?;
        interface.set_enabled(true)?;
        let previous = state.control_interface.lock().replace(interface);
        drop(previous);
        Ok(())
    }

    fn stop(_device: &Device, state: &Shared<DeviceState>) {
        let interface = state.control_interface.lock().take();
        if let Some(interface) = interface {
            let _ = interface.set_enabled(false);
        }
    }
}

impl FileHandler for Sar {
    type Context = ControlHandle;

    /// A file is a control handle exactly when its name is a backslash and
    /// the control reference string, compared without regard to case (A.1).
    fn claims(file_name: &[u16]) -> bool {
        is_control_file_name(file_name)
    }

    /// Each open is independent: its own section, endpoints and
    /// notification queue (section 5, "Opening and access").
    fn create(_device: &DeviceObject) -> Result<Shared<ControlHandle>, NtStatus> {
        ControlHandle::try_new()
    }

    fn cleanup(device: &DeviceObject, control: &Shared<ControlHandle>) {
        control.close(device);
    }

    fn device_control(device: &DeviceObject, control: &Shared<ControlHandle>, irp: Irp) -> DispatchResult {
        crate::control::dispatch(device, control, irp)
    }

    /// "On unload it stops registry filtering if it was started and releases
    /// what it holds" (section 3).
    fn unload() {
        drop(DRIVER.take());
    }
}
