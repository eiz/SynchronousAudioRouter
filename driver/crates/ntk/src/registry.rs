// Copyright (c) 2026 Itty Bitty Binaries, Inc.
// SPDX-License-Identifier: MIT

//! Registry keys and registry callbacks.

use core::{
    ffi::c_void,
    sync::atomic::{AtomicI64, Ordering},
};

use ntstatus::NtStatus;
use ntx_sys::seh::SarShimGuardedWrite;
use wdk_sys::{
    ntddk::{
        CmCallbackGetKeyObjectIDEx, CmCallbackReleaseKeyObjectIDEx, CmRegisterCallbackEx,
        CmUnRegisterCallback, ZwClose, ZwCreateKey, ZwEnumerateValueKey, ZwOpenKey,
        ZwQueryValueKey, ZwSetValueKey,
    },
    HANDLE, LARGE_INTEGER, NTSTATUS, OBJECT_ATTRIBUTES, OBJ_CASE_INSENSITIVE, OBJ_KERNEL_HANDLE,
    PCUNICODE_STRING, REG_DWORD, REG_ENUMERATE_VALUE_KEY_INFORMATION, REG_OPTION_NON_VOLATILE,
    REG_QUERY_VALUE_KEY_INFORMATION, REG_SZ,
};

use crate::{
    alloc_ext::Shared,
    device::DriverObject,
    string::{units_of, UnicodeStr},
};

pub use wdk_sys::{KEY_CREATE_SUB_KEY, KEY_QUERY_VALUE, KEY_SET_VALUE};

/// An open registry key (a kernel handle).
pub struct RegistryKey {
    handle: HANDLE,
}

// SAFETY: a kernel handle is valid in every thread and process context.
unsafe impl Send for RegistryKey {}

fn attributes(root: HANDLE, name: &UnicodeStr<'_>) -> OBJECT_ATTRIBUTES {
    OBJECT_ATTRIBUTES {
        Length: core::mem::size_of::<OBJECT_ATTRIBUTES>() as u32,
        RootDirectory: root,
        ObjectName: name.as_ptr(),
        Attributes: OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE,
        SecurityDescriptor: core::ptr::null_mut(),
        SecurityQualityOfService: core::ptr::null_mut(),
    }
}

impl RegistryKey {
    pub(crate) fn from_handle(handle: HANDLE) -> Self {
        Self { handle }
    }

    /// Opens the key with the full kernel path `path`
    /// (`\REGISTRY\MACHINE\...`).
    pub fn open(path: &[u16], access: u32) -> Result<Self, NtStatus> {
        let name = UnicodeStr::new(path).ok_or(NtStatus::INVALID_PARAMETER)?;
        let mut attributes = attributes(core::ptr::null_mut(), &name);
        let mut handle: HANDLE = core::ptr::null_mut();
        // SAFETY: the attributes and the name they point to are live locals.
        let status = unsafe { ZwOpenKey(&mut handle, access, &mut attributes) };
        crate::check(status)?;
        Ok(Self { handle })
    }

    /// Opens the subkey `name` of this key, creating it if needed.
    pub fn create_subkey(&self, name: &[u16], access: u32) -> Result<Self, NtStatus> {
        let name = UnicodeStr::new(name).ok_or(NtStatus::INVALID_PARAMETER)?;
        let mut attributes = attributes(self.handle, &name);
        let mut handle: HANDLE = core::ptr::null_mut();
        // SAFETY: the attributes and the name they point to are live locals;
        // the root is this key's valid handle.
        let status = unsafe {
            ZwCreateKey(
                &mut handle,
                access,
                &mut attributes,
                0,
                core::ptr::null_mut(),
                REG_OPTION_NON_VOLATILE,
                core::ptr::null_mut(),
            )
        };
        crate::check(status)?;
        Ok(Self { handle })
    }

    fn set_value(&self, name: &[u16], kind: u32, data: &[u8]) -> Result<(), NtStatus> {
        let name = UnicodeStr::new(name).ok_or(NtStatus::INVALID_PARAMETER)?;
        let size = u32::try_from(data.len()).map_err(|_| NtStatus::INVALID_PARAMETER)?;
        // SAFETY: the handle is valid; the name and data are live for the
        // call and the registry only reads them.
        let status = unsafe {
            ZwSetValueKey(
                self.handle,
                name.as_ptr(),
                0,
                kind,
                if data.is_empty() {
                    core::ptr::null_mut()
                } else {
                    data.as_ptr().cast_mut().cast()
                },
                size,
            )
        };
        crate::check(status)
    }

    /// Writes a `REG_SZ` value. `text` must include its terminating NUL.
    pub fn set_string(&self, name: &[u16], text: &[u16]) -> Result<(), NtStatus> {
        if text.last() != Some(&0) {
            return Err(NtStatus::INVALID_PARAMETER);
        }
        // SAFETY: a `[u16]` may be viewed as twice as many initialised bytes.
        let bytes = unsafe { core::slice::from_raw_parts(text.as_ptr().cast::<u8>(), text.len() * 2) };
        self.set_value(name, REG_SZ, bytes)
    }

    /// Writes a `REG_DWORD` value.
    pub fn set_dword(&self, name: &[u16], value: u32) -> Result<(), NtStatus> {
        self.set_value(name, REG_DWORD, &value.to_le_bytes())
    }

    /// `ZwQueryValueKey` into `buffer`. Returns the status and the result
    /// length the registry reported (meaningful for success and for the
    /// buffer-size statuses).
    pub fn query_value(&self, name: &[u16], information_class: i32, buffer: &mut [u8]) -> (NtStatus, u32) {
        let Some(name) = UnicodeStr::new(name) else {
            return (NtStatus::INVALID_PARAMETER, 0);
        };
        let Ok(length) = u32::try_from(buffer.len()) else {
            return (NtStatus::INVALID_PARAMETER, 0);
        };
        let mut result_length = 0u32;
        // SAFETY: the handle is valid; `buffer` is writable for `length`
        // bytes (null when empty); an unknown information class is rejected
        // by the registry.
        let status = unsafe {
            ZwQueryValueKey(
                self.handle,
                name.as_ptr(),
                information_class,
                buffer_pointer(buffer),
                length,
                &mut result_length,
            )
        };
        (NtStatus(status), result_length)
    }

    /// `ZwEnumerateValueKey` into `buffer`; see [`query_value`](Self::query_value).
    pub fn enumerate_value(&self, index: u32, information_class: i32, buffer: &mut [u8]) -> (NtStatus, u32) {
        let Ok(length) = u32::try_from(buffer.len()) else {
            return (NtStatus::INVALID_PARAMETER, 0);
        };
        let mut result_length = 0u32;
        // SAFETY: as for `query_value`.
        let status = unsafe {
            ZwEnumerateValueKey(
                self.handle,
                index,
                information_class,
                buffer_pointer(buffer),
                length,
                &mut result_length,
            )
        };
        (NtStatus(status), result_length)
    }
}

/// The pointer to pass a kernel routine for an output buffer: null for an
/// empty one, whose slice pointer is dangling.
fn buffer_pointer(buffer: &mut [u8]) -> *mut c_void {
    if buffer.is_empty() {
        core::ptr::null_mut()
    } else {
        buffer.as_mut_ptr().cast()
    }
}

impl Drop for RegistryKey {
    fn drop(&mut self) {
        // SAFETY: closes the handle this value owns, once.
        unsafe {
            let _ = ZwClose(self.handle);
        }
    }
}

/// What a registry callback does with an operation it was shown before the
/// registry performs it.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum FilterDecision {
    /// Let the registry carry out the operation.
    PassThrough,
    /// The callback has produced the result; the caller sees success.
    Completed,
    /// Fail the operation with this status.
    Fail(NtStatus),
}

/// A value read (query by name, or enumeration by index) seen by a registry
/// callback before the registry performs it.
pub struct ValueRead<'a> {
    cookie: &'a AtomicI64,
    key_object: *mut c_void,
    /// Length in bytes of the value name of a query; `None` for an
    /// enumeration.
    name_length: Option<usize>,
    /// Index of an enumeration; `None` for a query.
    index: Option<u32>,
    information_class: i32,
    output: *mut c_void,
    output_length: u32,
    result_length: *mut u32,
}

impl ValueRead<'_> {
    /// For a query: the length in bytes of the value name asked for.
    pub fn value_name_length(&self) -> Option<usize> {
        self.name_length
    }

    /// For an enumeration: the index asked for.
    pub fn index(&self) -> Option<u32> {
        self.index
    }

    /// The `KEY_VALUE_INFORMATION_CLASS` the caller asked for.
    pub fn information_class(&self) -> i32 {
        self.information_class
    }

    /// Length of the caller's result buffer.
    pub fn output_length(&self) -> u32 {
        self.output_length
    }

    /// Calls `f` with the full kernel path of the key being read.
    pub fn with_key_path<R>(&self, f: impl FnOnce(&[u16]) -> R) -> Result<R, NtStatus> {
        let mut cookie = LARGE_INTEGER {
            QuadPart: self.cookie.load(Ordering::Acquire),
        };
        let mut name: PCUNICODE_STRING = core::ptr::null();
        // SAFETY: `key_object` is the key object pointer the registry passed
        // to the callback, valid for the duration of the callback; the
        // cookie identifies this driver's registration (an unknown cookie
        // makes the call fail).
        let status = unsafe {
            CmCallbackGetKeyObjectIDEx(&mut cookie, self.key_object, core::ptr::null_mut(), &mut name, 0)
        };
        crate::check(status)?;
        // SAFETY: on success `name` points to a string that stays valid
        // until it is released below.
        let result = f(unsafe { units_of(name) });
        // SAFETY: releases the name obtained above, once.
        unsafe { CmCallbackReleaseKeyObjectIDEx(name) };
        Ok(result)
    }

    /// Hands the caller a result: copies `data` (cut to the caller's buffer
    /// length) into the caller's buffer and stores `result_length`.
    ///
    /// The caller's buffers are user memory that the registry has probed
    /// but not captured, so the copies are made under an exception handler.
    pub fn write_result(&mut self, data: &[u8], result_length: u32) -> Result<(), NtStatus> {
        let count = data.len().min(self.output_length as usize);
        if count != 0 && !self.output.is_null() {
            // SAFETY: `data` is valid for `count` bytes; the destination is
            // the caller's output buffer of at least `count` bytes, which
            // the registry probed; the shim catches a fault during the copy.
            let status = unsafe { SarShimGuardedWrite(self.output, data.as_ptr().cast(), count) };
            crate::check(status)?;
        }
        if !self.result_length.is_null() {
            // SAFETY: as above, for the four-byte result length.
            let status = unsafe {
                SarShimGuardedWrite(
                    self.result_length.cast(),
                    core::ptr::from_ref(&result_length).cast(),
                    core::mem::size_of::<u32>(),
                )
            };
            crate::check(status)?;
        }
        Ok(())
    }
}

/// A driver's registry filter.
pub trait RegistryFilter: Send + Sync + 'static {
    /// A value read is about to happen, on the thread and in the process
    /// that asked for it, at `PASSIVE_LEVEL`.
    fn pre_value_read(&self, read: &mut ValueRead<'_>) -> FilterDecision;
}

struct Registration<F> {
    cookie: AtomicI64,
    filter: F,
}

/// A registered registry callback. Dropping it unregisters the callback and
/// waits for calls in progress to finish.
pub struct RegistryCallback<F: RegistryFilter> {
    registration: Shared<Registration<F>>,
    /// The reference handed to the kernel as the callback's context.
    kernel_context: *mut c_void,
}

// SAFETY: `kernel_context` is a `Shared<Registration<F>>` reference in
// pointer form; with `F: Send + Sync` it may be released from any thread.
unsafe impl<F: RegistryFilter> Send for RegistryCallback<F> {}
// SAFETY: as above; the type has no methods that take `&self`.
unsafe impl<F: RegistryFilter> Sync for RegistryCallback<F> {}

impl<F: RegistryFilter> RegistryCallback<F> {
    /// Registers `filter` at the given altitude (a decimal string).
    pub fn register(driver: DriverHandle, altitude: &[u16], filter: F) -> Result<Self, NtStatus> {
        let altitude = UnicodeStr::new(altitude).ok_or(NtStatus::INVALID_PARAMETER)?;
        let registration = Shared::try_new(Registration {
            cookie: AtomicI64::new(0),
            filter,
        })?;
        let kernel_context = registration.clone().into_context();
        let mut cookie = LARGE_INTEGER { QuadPart: 0 };
        // SAFETY: `callback::<F>` matches EX_CALLBACK_FUNCTION and
        // interprets its context as the `Shared<Registration<F>>` reference
        // passed here, which stays in place until the callback has been
        // unregistered in `drop`. The driver object pointer identifies this
        // driver.
        let status = unsafe {
            CmRegisterCallbackEx(
                Some(callback::<F>),
                altitude.as_ptr().cast_const(),
                driver.0,
                kernel_context,
                &mut cookie,
                core::ptr::null_mut(),
            )
        };
        if let Err(status) = crate::check(status) {
            // SAFETY: registration failed, so the callback will never use
            // the context reference; take it back, once.
            drop(unsafe { Shared::<Registration<F>>::from_context(kernel_context) });
            return Err(status);
        }
        // SAFETY: reading the field the kernel just initialised.
        registration.cookie.store(unsafe { cookie.QuadPart }, Ordering::Release);
        Ok(Self {
            registration,
            kernel_context,
        })
    }
}

impl<F: RegistryFilter> Drop for RegistryCallback<F> {
    fn drop(&mut self) {
        let cookie = LARGE_INTEGER {
            QuadPart: self.registration.cookie.load(Ordering::Acquire),
        };
        // SAFETY: the cookie is the one CmRegisterCallbackEx returned.
        // CmUnRegisterCallback returns only after every call of the callback
        // has finished, so afterwards nothing uses the kernel's context
        // reference and it can be taken back, once.
        unsafe {
            let _ = CmUnRegisterCallback(cookie);
            drop(Shared::<Registration<F>>::from_context(self.kernel_context));
        }
    }
}

/// The driver object pointer, kept for registering callbacks after
/// `DriverEntry` has returned. A driver object lives until the driver has
/// been unloaded.
#[derive(Clone, Copy)]
pub struct DriverHandle(*mut c_void);

// SAFETY: the pointer is an opaque identity token passed back to the kernel.
unsafe impl Send for DriverHandle {}
// SAFETY: as above.
unsafe impl Sync for DriverHandle {}

impl DriverHandle {
    pub fn new(driver: &DriverObject) -> Self {
        Self(driver.as_raw().cast())
    }
}

unsafe extern "C" fn callback<F: RegistryFilter>(
    context: *mut c_void,
    argument1: *mut c_void,
    argument2: *mut c_void,
) -> NTSTATUS {
    if argument2.is_null() {
        return NtStatus::SUCCESS.0;
    }
    // SAFETY: `context` is the reference handed to CmRegisterCallbackEx,
    // which stays in place until after CmUnRegisterCallback has waited for
    // this call to return.
    let registration = unsafe { Shared::<Registration<F>>::borrow_context(context) };
    // The first argument is a REG_NOTIFY_CLASS value passed as a pointer.
    let class = argument1 as usize as i32;

    let mut read = if class == wdk_sys::_REG_NOTIFY_CLASS::RegNtPreQueryValueKey {
        // SAFETY: for this notification class the registry passes a valid
        // REG_QUERY_VALUE_KEY_INFORMATION that lives for the call.
        let info = unsafe { &*argument2.cast::<REG_QUERY_VALUE_KEY_INFORMATION>() };
        // SAFETY: ValueName is an input parameter; since Windows 8 the
        // registry hands callbacks a captured kernel copy (or null).
        let name = unsafe { units_of(info.ValueName.cast_const()) };
        ValueRead {
            cookie: &registration.cookie,
            key_object: info.Object,
            name_length: Some(name.len() * 2),
            index: None,
            information_class: info.KeyValueInformationClass,
            output: info.KeyValueInformation,
            output_length: info.Length,
            result_length: info.ResultLength,
        }
    } else if class == wdk_sys::_REG_NOTIFY_CLASS::RegNtPreEnumerateValueKey {
        // SAFETY: for this notification class the registry passes a valid
        // REG_ENUMERATE_VALUE_KEY_INFORMATION that lives for the call.
        let info = unsafe { &*argument2.cast::<REG_ENUMERATE_VALUE_KEY_INFORMATION>() };
        ValueRead {
            cookie: &registration.cookie,
            key_object: info.Object,
            name_length: None,
            index: Some(info.Index),
            information_class: info.KeyValueInformationClass,
            output: info.KeyValueInformation,
            output_length: info.Length,
            result_length: info.ResultLength,
        }
    } else {
        return NtStatus::SUCCESS.0;
    };

    match registration.filter.pre_value_read(&mut read) {
        FilterDecision::PassThrough => NtStatus::SUCCESS.0,
        FilterDecision::Completed => NtStatus::CALLBACK_BYPASS.0,
        // A success code here would mean "carry on"; only a failure or
        // warning status fails the operation.
        FilterDecision::Fail(status) if !status.is_success() => status.0,
        FilterDecision::Fail(_) => NtStatus::UNSUCCESSFUL.0,
    }
}
