// Copyright (c) 2026 Itty Bitty Binaries, Inc.
// SPDX-License-Identifier: MIT

//! The AVStream device.

use core::{
    ffi::c_void,
    marker::PhantomData,
    sync::atomic::{AtomicPtr, Ordering},
};

use ks_sys as sys;
use ntk::{
    alloc_ext::Shared,
    device::{DeviceObject, DriverObject},
    irp::FileRequest,
    sync::Rundown,
};
use ntstatus::NtStatus;
use wdk_sys::{DRIVER_OBJECT, NTSTATUS, PCUNICODE_STRING, PIRP};

use crate::context::{self, Header, Kind};

/// The driver object that was initialised through [`initialize_driver`].
static DRIVER: AtomicPtr<DRIVER_OBJECT> = AtomicPtr::new(core::ptr::null_mut());

/// A driver's handling of its AVStream device.
pub trait DeviceHandler: 'static {
    /// State of one device.
    type Context: Send + Sync + 'static;

    /// A device has been added. AVStream holds the device mutex.
    fn add(device: &Device) -> Result<Shared<Self::Context>, NtStatus>;

    /// Plug and Play is starting the device. AVStream holds the device
    /// mutex.
    fn start(device: &Device, context: &Shared<Self::Context>) -> Result<(), NtStatus>;

    /// Plug and Play is stopping or removing the device (also called for a
    /// surprise removal, so it may run more than once). AVStream holds the
    /// device mutex.
    fn stop(device: &Device, context: &Shared<Self::Context>);
}

/// The part of a device's block that does not depend on the handler.
#[repr(C)]
struct DeviceBlockHead {
    header: Header,
    /// The gate around every kernel streaming call this crate makes on one
    /// of the device's filter factories without holding the device mutex
    /// (see [`FactoryCalls`]).
    factory_calls: Shared<Rundown>,
}

#[repr(C)]
struct DeviceBlock<D: DeviceHandler> {
    head: DeviceBlockHead,
    context: Shared<D::Context>,
}

/// What keeps a filter factory in existence during a kernel streaming call
/// that is made on it without the device mutex.
///
/// AVStream destroys a factory of its own accord only when the device stops
/// (factories are created to be freed then). The calls that Microsoft
/// documents as needing the device mutex are made under it, which rules a
/// stop out. The others (switching a factory's device interfaces on or off,
/// writing its cached filter data, reading its symbolic link) can take
/// milliseconds each inside Plug and Play and the registry, and every open
/// and close of any filter on the device would wait for them. They are made
/// inside this gate instead: each call enters it, and the device's stop
/// shuts it and so waits, before AVStream frees anything, until the calls
/// that are under way have returned. Later calls are refused until the
/// device starts again.
#[derive(Clone)]
pub(crate) struct FactoryCalls(Shared<Rundown>);

impl FactoryCalls {
    /// Runs `call` inside the gate, or returns `None` if the device has
    /// stopped. Must be called at `PASSIVE_LEVEL`.
    pub(crate) fn run<R>(&self, call: impl FnOnce() -> R) -> Option<R> {
        let _inside = self.0.enter()?;
        Some(call())
    }
}

/// The registry path argument of `DriverEntry`.
pub struct RegistryPath(PCUNICODE_STRING);

impl RegistryPath {
    /// # Safety
    ///
    /// `raw` must be the registry path the I/O manager passed to this
    /// driver's `DriverEntry`, and the value must not outlive that call.
    pub unsafe fn from_raw(raw: PCUNICODE_STRING) -> Self {
        Self(raw)
    }
}

struct Tables<D>(PhantomData<D>);

impl<D: DeviceHandler> Tables<D> {
    const DISPATCH: sys::KSDEVICE_DISPATCH = sys::KSDEVICE_DISPATCH {
        Add: Some(add::<D>),
        Start: Some(start::<D>),
        PostStart: None,
        QueryStop: None,
        CancelStop: None,
        Stop: Some(stop::<D>),
        QueryRemove: None,
        CancelRemove: None,
        Remove: Some(stop::<D>),
        QueryCapabilities: None,
        SurpriseRemoval: Some(stop::<D>),
        QueryPower: None,
        SetPower: None,
        QueryInterface: None,
    };
    const DESCRIPTOR: sys::KSDEVICE_DESCRIPTOR = sys::KSDEVICE_DESCRIPTOR {
        Dispatch: &Self::DISPATCH,
        FilterDescriptorsCount: 0,
        FilterDescriptors: core::ptr::null(),
        Version: sys::KSDEVICE_DESCRIPTOR_VERSION,
        Flags: 0,
    };
}

/// Registers the driver with kernel streaming as an AVStream minidriver
/// with no static filters. AVStream takes over the driver object: device
/// creation, Plug and Play, power, and the dispatch of all requests.
pub fn initialize_driver<D: DeviceHandler>(
    driver: &mut DriverObject,
    registry_path: &RegistryPath,
) -> Result<(), NtStatus> {
    let descriptor: &'static sys::KSDEVICE_DESCRIPTOR = &Tables::<D>::DESCRIPTOR;
    // SAFETY: the driver object and registry path are the ones DriverEntry
    // received (contracts of `DriverObject::from_raw` and
    // `RegistryPath::from_raw`). The descriptor and the dispatch table it
    // points to are promoted constants that live as long as the image, and
    // the trampolines in them match the prototypes AVStream expects.
    let status = unsafe { sys::KsInitializeDriver(driver.as_raw(), registry_path.0.cast_mut(), descriptor) };
    NtStatus(status).into_result()?;
    DRIVER.store(driver.as_raw(), Ordering::Release);
    Ok(())
}

/// An AVStream device (`KSDEVICE`).
#[repr(transparent)]
pub struct Device(sys::KSDEVICE);

impl Device {
    /// # Safety
    ///
    /// `raw` must be a `KSDEVICE` that stays valid for `'a`.
    unsafe fn from_raw<'a>(raw: sys::PKSDEVICE) -> &'a Self {
        // SAFETY: transparent wrapper; validity is the caller's contract.
        unsafe { &*raw.cast::<Self>() }
    }

    pub(crate) fn as_raw(&self) -> sys::PKSDEVICE {
        core::ptr::from_ref(self).cast_mut().cast()
    }

    /// The AVStream device a request for one of its files was sent to, if
    /// the device belongs to the driver that called [`initialize_driver`].
    /// `None` for a request that is for no file.
    ///
    /// The device is lent for as long as the request is borrowed, and so
    /// not past its completion: see [`FileRequest`] for why the device
    /// cannot be removed in that time.
    pub fn of_request<R: FileRequest>(request: &R) -> Option<&Self> {
        let device_object = request.file_device()?;
        // SAFETY: the request is for a file of the device and has not been
        // completed, so the file is open and the device has not been
        // removed; that stays so while `request` is borrowed, which is as
        // long as the result may be used.
        unsafe { Self::of(device_object) }
    }

    /// The AVStream device behind a device object, if the device object
    /// belongs to the driver that called [`initialize_driver`].
    ///
    /// # Safety
    ///
    /// The device must not have been removed, and must not be removed while
    /// the returned reference is in use. AVStream frees the `KSDEVICE` when
    /// Plug and Play removes the device, and the device object can exist for
    /// longer than that (anything that holds a reference to it keeps it), so
    /// a valid device object alone is not enough.
    unsafe fn of(device_object: &DeviceObject) -> Option<&Self> {
        let driver = DRIVER.load(Ordering::Acquire);
        // SAFETY: `device_object` is a valid device object.
        let owner = unsafe { (*device_object.as_raw()).DriverObject };
        if driver.is_null() || owner != driver {
            return None;
        }
        // SAFETY: every device object of this driver was created by
        // AVStream's AddDevice (the driver never creates one itself), which
        // is what KsGetDeviceForDeviceObject requires; the device has not
        // been removed (the caller's contract), so what it reads is there.
        let raw = unsafe { sys::KsGetDeviceForDeviceObject(device_object.as_raw()) };
        // SAFETY: a non-null result is the KSDEVICE of that device object,
        // valid until the device is removed, which the caller holds off.
        (!raw.is_null()).then(|| unsafe { Self::from_raw(raw) })
    }

    /// The functional device object AVStream created.
    pub fn functional_device_object(&self) -> &DeviceObject {
        // SAFETY: AVStream keeps the pointer valid for the KSDEVICE's life.
        unsafe { DeviceObject::from_raw(self.0.FunctionalDeviceObject) }
    }

    /// The physical device object of the device node.
    pub fn physical_device_object(&self) -> &DeviceObject {
        // SAFETY: as above.
        unsafe { DeviceObject::from_raw(self.0.PhysicalDeviceObject) }
    }

    /// The context the handler `D` created in [`DeviceHandler::add`].
    pub fn context<D: DeviceHandler>(&self) -> Option<Shared<D::Context>> {
        let raw = self.0.Context;
        // SAFETY: the device's Context is null or the block `add::<D>`
        // stored, which the device's object bag keeps alive as long as the
        // KSDEVICE exists.
        if !unsafe { context::is::<D>(raw, Kind::Device) } {
            return None;
        }
        // SAFETY: the header check proves the block is a `DeviceBlock<D>`.
        let block = unsafe { &*raw.cast::<DeviceBlock<D>>() };
        Some(block.context.clone())
    }

    /// The gate for calls on this device's filter factories that are made
    /// without the device mutex.
    pub(crate) fn factory_calls(&self) -> Option<FactoryCalls> {
        // SAFETY: as in `context`.
        unsafe { head_of(self.0.Context) }.map(|head| FactoryCalls(head.factory_calls.clone()))
    }

    /// Takes the device mutex, which serialises changes to the device's
    /// filter factories with AVStream's own use of them.
    pub fn lock(&self) -> DeviceGuard<'_> {
        // SAFETY: valid KSDEVICE; released in the guard's destructor.
        unsafe { sys::KsAcquireDevice(self.as_raw()) };
        DeviceGuard { device: self }
    }
}

/// Proof that the device mutex is held.
pub struct DeviceGuard<'a> {
    device: &'a Device,
}

impl<'a> DeviceGuard<'a> {
    pub fn device(&self) -> &'a Device {
        self.device
    }
}

impl Drop for DeviceGuard<'_> {
    fn drop(&mut self) {
        // SAFETY: this guard acquired the mutex in `Device::lock`.
        unsafe { sys::KsReleaseDevice(self.device.as_raw()) };
    }
}

/// Bag callback: the device is going away, release its context block.
unsafe extern "C" fn free_block<D: DeviceHandler>(data: *mut c_void) {
    // SAFETY: `data` is the reference `add::<D>` put in the bag, delivered
    // exactly once.
    drop(unsafe { Shared::<DeviceBlock<D>>::from_context(data) });
}

unsafe extern "C" fn add<D: DeviceHandler>(device: sys::PKSDEVICE) -> NTSTATUS {
    // SAFETY: AVStream passes the new, valid KSDEVICE.
    let wrapper = unsafe { Device::from_raw(device) };
    let user = match D::add(wrapper) {
        Ok(user) => user,
        Err(status) => return status.0,
    };
    let factory_calls = match Shared::try_new(Rundown::new()) {
        Ok(factory_calls) => factory_calls,
        Err(status) => return status.0,
    };
    let block = match Shared::try_new(DeviceBlock::<D> {
        head: DeviceBlockHead {
            header: Header::new::<D>(Kind::Device),
            factory_calls,
        },
        context: user,
    }) {
        Ok(block) => block,
        Err(status) => return status.0,
    };
    let raw = block.into_context();
    // SAFETY: the device and its bag are valid. On success the bag owns the
    // reference and returns it through `free_block::<D>` when the device is
    // deleted; only then is the Context pointer published.
    unsafe {
        let status = sys::KsAddItemToObjectBag((*device).Bag, raw, Some(free_block::<D>));
        if !NtStatus(status).is_success() {
            drop(Shared::<DeviceBlock<D>>::from_context(raw));
            return status;
        }
        (*device).Context = raw;
    }
    NtStatus::SUCCESS.0
}

/// The handler-independent part of the block behind a device's `Context`.
///
/// # Safety
///
/// `context` must be null or the `Context` of a live KSDEVICE of this driver.
unsafe fn head_of<'a>(context: *mut c_void) -> Option<&'a DeviceBlockHead> {
    // SAFETY: a device's Context is null or the block `add` stored, which
    // the device's object bag keeps alive as long as the KSDEVICE exists.
    // Every such block begins with a `DeviceBlockHead` whatever its handler.
    if unsafe { context::is_kind(context, Kind::Device) } {
        // SAFETY: the kind check proves the block is a `DeviceBlock<_>`,
        // whose first field is the head.
        Some(unsafe { &*context.cast::<DeviceBlockHead>() })
    } else {
        None
    }
}

/// Runs `f` with the device and its context, if the context is in place.
///
/// # Safety
///
/// `device` must be the valid KSDEVICE AVStream passed to a device callback.
unsafe fn with_context<D: DeviceHandler, R>(
    device: sys::PKSDEVICE,
    f: impl FnOnce(&Device, &Shared<D::Context>) -> R,
) -> Option<R> {
    // SAFETY: valid by the caller's contract.
    let wrapper = unsafe { Device::from_raw(device) };
    wrapper.context::<D>().map(|context| f(wrapper, &context))
}

unsafe extern "C" fn start<D: DeviceHandler>(
    device: sys::PKSDEVICE,
    _irp: PIRP,
    _translated: wdk_sys::PCM_RESOURCE_LIST,
    _untranslated: wdk_sys::PCM_RESOURCE_LIST,
) -> NTSTATUS {
    // The device is starting (again): calls on its filter factories may be
    // made again. Start and stop are serialised by Plug and Play and by the
    // device mutex, which AVStream holds for both.
    // SAFETY: AVStream passes a valid KSDEVICE.
    if let Some(head) = unsafe { head_of((*device).Context) } {
        head.factory_calls.open();
    }
    // SAFETY: AVStream passes a valid KSDEVICE.
    match unsafe { with_context::<D, _>(device, |device, context| D::start(device, context)) } {
        Some(Ok(())) => NtStatus::SUCCESS.0,
        Some(Err(status)) => status.0,
        None => NtStatus::UNSUCCESSFUL.0,
    }
}

unsafe extern "C" fn stop<D: DeviceHandler>(device: sys::PKSDEVICE, _irp: PIRP) {
    // AVStream is about to free the device's filter factories. No call on
    // one of them may begin from here on, and those under way have to
    // return first (see `FactoryCalls`). They do not need the device mutex,
    // which AVStream holds here, so waiting for them cannot deadlock.
    // SAFETY: AVStream passes a valid KSDEVICE.
    if let Some(head) = unsafe { head_of((*device).Context) } {
        head.factory_calls.shut();
    }
    // SAFETY: AVStream passes a valid KSDEVICE.
    let _ = unsafe { with_context::<D, _>(device, |device, context| D::stop(device, context)) };
}
