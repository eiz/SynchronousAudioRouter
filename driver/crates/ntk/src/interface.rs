// Copyright (c) 2026 Itty Bitty Binaries, Inc.
// SPDX-License-Identifier: MIT

//! Device interfaces.

use ntstatus::NtStatus;
use wdk_sys::{
    ntddk::{
        IoOpenDeviceInterfaceRegistryKey, IoRegisterDeviceInterface, IoSetDeviceInterfaceState,
        RtlFreeUnicodeString,
    },
    HANDLE, UNICODE_STRING,
};

use crate::{device::DeviceObject, registry::RegistryKey, string::UnicodeStr, Guid};

/// One instance of a device interface class on a device: the symbolic link
/// that `IoRegisterDeviceInterface` returns. Registering an instance that
/// already exists returns the existing one.
pub struct DeviceInterface {
    link: UNICODE_STRING,
}

// SAFETY: the string buffer is owned by this value and only read.
unsafe impl Send for DeviceInterface {}
// SAFETY: as above.
unsafe impl Sync for DeviceInterface {}

impl DeviceInterface {
    /// Registers (or looks up) the interface instance of `class` with the
    /// given reference string on the device whose physical device object is
    /// `pdo`. Must run at `PASSIVE_LEVEL`.
    pub fn register(pdo: &DeviceObject, class: &Guid, reference: Option<&[u16]>) -> Result<Self, NtStatus> {
        let reference = match reference {
            Some(units) => Some(UnicodeStr::new(units).ok_or(NtStatus::INVALID_PARAMETER)?),
            None => None,
        };
        let mut link = UNICODE_STRING {
            Length: 0,
            MaximumLength: 0,
            Buffer: core::ptr::null_mut(),
        };
        // SAFETY: `pdo` is a valid device object; the class GUID, the
        // optional reference string and the out-parameter are live for the
        // call. On success the routine allocates `link`'s buffer.
        let status = unsafe {
            IoRegisterDeviceInterface(
                pdo.as_raw(),
                class,
                reference.as_ref().map_or(core::ptr::null_mut(), UnicodeStr::as_ptr),
                &mut link,
            )
        };
        // STATUS_OBJECT_NAME_EXISTS (already registered) is a success code.
        crate::check(status)?;
        Ok(Self { link })
    }

    fn link_ptr(&self) -> *mut UNICODE_STRING {
        core::ptr::from_ref(&self.link).cast_mut()
    }

    /// The kernel path of the interface's symbolic link.
    pub fn symbolic_link(&self) -> &[u16] {
        // SAFETY: `link` is a valid string owned by `self`.
        unsafe { crate::string::units_of(&self.link) }
    }

    /// Switches the interface on or off.
    pub fn set_enabled(&self, enabled: bool) -> Result<(), NtStatus> {
        // SAFETY: `link` is the string IoRegisterDeviceInterface returned;
        // the routine only reads it.
        let status = unsafe { IoSetDeviceInterfaceState(self.link_ptr(), u8::from(enabled)) };
        crate::check(status)
    }

    /// Opens the registry key for the interface instance's parameters.
    pub fn open_parameters_key(&self, access: u32) -> Result<RegistryKey, NtStatus> {
        let mut handle: HANDLE = core::ptr::null_mut();
        // SAFETY: as for `set_enabled`; `handle` receives a kernel handle
        // (the caller is kernel code at PASSIVE_LEVEL).
        let status = unsafe { IoOpenDeviceInterfaceRegistryKey(self.link_ptr(), access, &mut handle) };
        crate::check(status)?;
        Ok(RegistryKey::from_handle(handle))
    }
}

impl Drop for DeviceInterface {
    fn drop(&mut self) {
        // SAFETY: frees the buffer IoRegisterDeviceInterface allocated, once.
        unsafe { RtlFreeUnicodeString(&mut self.link) };
    }
}
