// Copyright (c) 2026 Itty Bitty Binaries, Inc.
// SPDX-License-Identifier: MIT

//! Taking over the requests for some of a device's files.
//!
//! A driver built on a framework (here, kernel streaming) lets the framework
//! own the dispatch table. [`install_file_handler`] puts a thin layer in
//! front of it: requests for files whose name the handler claims are
//! handled by the driver, everything else goes to the framework unchanged.
//!
//! One thing must never go to the framework: a file the handler claimed.
//! What such a file keeps in its context slot is the handler's, and the
//! framework would read it as an object of its own. A request names a file
//! in two ways, as the file it is for and, for a create request, as the
//! parent the new file is opened relative to; both are looked at here.

use core::{
    ffi::c_void,
    sync::atomic::{AtomicBool, AtomicUsize, Ordering},
};

use ntstatus::NtStatus;
use wdk_sys::{
    DRIVER_OBJECT, IRP_MJ_CLEANUP, IRP_MJ_CLOSE, IRP_MJ_CREATE, IRP_MJ_DEVICE_CONTROL,
    IRP_MJ_MAXIMUM_FUNCTION, IRP_MJ_PNP, IRP_MJ_POWER, IRP_MJ_SYSTEM_CONTROL, NTSTATUS,
    PDEVICE_OBJECT, PIRP,
};

use crate::{
    alloc_ext::Shared,
    device::{DeviceObject, DriverObject},
    irp::{DispatchResult, FileRef, Irp},
};

const MAJOR_FUNCTIONS: usize = IRP_MJ_MAXIMUM_FUNCTION as usize + 1;

type DispatchFn = unsafe extern "C" fn(PDEVICE_OBJECT, PIRP) -> NTSTATUS;

/// The framework's dispatch routines, saved when the hooks go in. Written
/// once during DriverEntry (before any request can arrive), read-only after.
static ORIGINAL_DISPATCH: [AtomicUsize; MAJOR_FUNCTIONS] =
    [const { AtomicUsize::new(0) }; MAJOR_FUNCTIONS];
static ORIGINAL_UNLOAD: AtomicUsize = AtomicUsize::new(0);
static INSTALLED: AtomicBool = AtomicBool::new(false);

/// A driver's own handling of the files it claims on a device.
pub trait FileHandler: 'static {
    /// State of one open file.
    type Context: Send + Sync + 'static;

    /// Whether a file opened with this name (relative to the device)
    /// belongs to the handler. Must depend on the name only.
    fn claims(file_name: &[u16]) -> bool;

    /// A claimed file is being opened. The returned context is attached to
    /// the file object.
    ///
    /// Nothing is ever opened relative to a claimed file: such a create
    /// request is refused with `STATUS_OBJECT_NAME_NOT_FOUND` before the
    /// handler or the framework sees it, whatever name it carries.
    fn create(device: &DeviceObject) -> Result<Shared<Self::Context>, NtStatus>;

    /// The last handle to the file has been closed (`IRP_MJ_CLEANUP`).
    fn cleanup(device: &DeviceObject, context: &Shared<Self::Context>);

    /// A device control request on the file. Runs on the requesting thread.
    fn device_control(
        device: &DeviceObject,
        context: &Shared<Self::Context>,
        irp: Irp,
    ) -> DispatchResult;

    /// The driver is being unloaded. Runs before the framework's own unload
    /// routine.
    fn unload();
}

/// Installs `H` in front of the framework's dispatch routines and unload
/// routine. Call once, in `DriverEntry`, after the framework has set up the
/// driver object.
///
/// For every major function except Plug and Play, power and system control,
/// requests on files that `H` claims are handled here: create, cleanup,
/// close and device control by `H`, anything else is refused with
/// `STATUS_INVALID_DEVICE_REQUEST`. All other requests are forwarded.
pub fn install_file_handler<H: FileHandler>(driver: &mut DriverObject) {
    assert!(
        !INSTALLED.swap(true, Ordering::AcqRel),
        "file handler installed twice"
    );
    let object: &mut DRIVER_OBJECT = driver.object();
    for (major, (slot, saved)) in object
        .MajorFunction
        .iter_mut()
        .zip(&ORIGINAL_DISPATCH)
        .enumerate()
    {
        if [IRP_MJ_PNP, IRP_MJ_POWER, IRP_MJ_SYSTEM_CONTROL].contains(&(major as u32)) {
            continue;
        }
        saved.store(slot.map_or(0, |f| f as usize), Ordering::Release);
        *slot = Some(dispatch::<H>);
    }
    let original_unload = object.DriverUnload.map_or(0, |f| f as usize);
    ORIGINAL_UNLOAD.store(original_unload, Ordering::Release);
    object.DriverUnload = Some(unload::<H>);
}

fn forward(device: PDEVICE_OBJECT, irp: Irp) -> DispatchResult {
    let major = irp.major_function() as usize;
    let raw = irp.into_raw();
    let original = ORIGINAL_DISPATCH
        .get(major)
        .map_or(0, |slot| slot.load(Ordering::Acquire));
    if original == 0 {
        // Cannot happen: the I/O manager fills every dispatch slot. Fail the
        // request rather than drop it.
        // SAFETY: `raw` is the IRP whose ownership we just took back.
        return unsafe { Irp::from_raw(raw) }.complete(NtStatus::INVALID_DEVICE_REQUEST);
    }
    // SAFETY: `original` was read from this driver object's dispatch table
    // in `install_file_handler`, so it is a valid dispatch routine; it is
    // called with the device and IRP exactly as the I/O manager passed them,
    // and takes over the IRP.
    let original: DispatchFn = unsafe { core::mem::transmute::<usize, DispatchFn>(original) };
    // SAFETY: see above.
    DispatchResult(unsafe { original(device, raw) })
}

/// The context pointer slot (`FsContext`) of a file. For a claimed file it
/// belongs to this module: the framework never sees such a file.
fn context_slot(file: &FileRef<'_>) -> *mut *mut c_void {
    // SAFETY: the file object is valid; this only computes a field address.
    unsafe { core::ptr::addr_of_mut!((*file.as_raw()).FsContext) }
}

unsafe extern "C" fn dispatch<H: FileHandler>(device: PDEVICE_OBJECT, irp: PIRP) -> NTSTATUS {
    // SAFETY: the I/O manager calls a dispatch routine with a valid IRP that
    // the driver now owns, on the requesting thread.
    let irp = unsafe { Irp::from_raw(irp) };
    // A file may not be opened relative to one that `H` claims: not as
    // another claimed file, which would have no meaning, and above all not
    // as a file of the framework's, which would take the parent's context
    // for its own. The parent is claimed exactly when its name is, like
    // any file.
    let below_claimed = irp.major_function() == IRP_MJ_CREATE
        && irp
            .file()
            .and_then(|file| file.related())
            .is_some_and(|parent| H::claims(parent.name()));
    if below_claimed {
        return irp.complete(NtStatus::OBJECT_NAME_NOT_FOUND).into_raw();
    }

    // The slot of the file's context, if `H` claims the file.
    let claimed = irp
        .file()
        .filter(|file| H::claims(file.name()))
        .map(|file| context_slot(&file));
    let Some(slot) = claimed else {
        return forward(device, irp).into_raw();
    };
    // SAFETY: the I/O manager passes a valid device object, which outlives
    // the request.
    let device_ref = unsafe { DeviceObject::from_raw(device) };

    let result = match irp.major_function() {
        IRP_MJ_CREATE => match H::create(device_ref) {
            Ok(context) => {
                // SAFETY: `slot` points into the file object being created;
                // no other request for it can run before create completes.
                unsafe { *slot = context.into_context() };
                irp.complete(NtStatus::SUCCESS)
            }
            Err(status) => irp.complete(status),
        },
        IRP_MJ_CLEANUP => {
            // SAFETY: for a claimed file, FsContext is either null or the
            // pointer stored by the create branch above for this same `H`
            // (claims() depends only on the immutable file name, and the
            // framework never touches claimed files). The file object's
            // reference is released only by the close request, which the
            // I/O manager sends after every other request has finished.
            if let Some(context) = unsafe { borrow_context::<H>(slot) } {
                H::cleanup(device_ref, &context);
            }
            irp.complete(NtStatus::SUCCESS)
        }
        IRP_MJ_CLOSE => {
            // SAFETY: close is the last request for a file object, so
            // nothing else reads the slot concurrently; the pointer is the
            // one stored at create (see above) and is taken back once.
            unsafe {
                let context = core::ptr::replace(slot, core::ptr::null_mut());
                if !context.is_null() {
                    drop(Shared::<H::Context>::from_context(context));
                }
            }
            irp.complete(NtStatus::SUCCESS)
        }
        IRP_MJ_DEVICE_CONTROL => {
            // SAFETY: as for cleanup.
            match unsafe { borrow_context::<H>(slot) } {
                Some(context) => H::device_control(device_ref, &context, irp),
                None => irp.complete(NtStatus::INVALID_DEVICE_REQUEST),
            }
        }
        _ => irp.complete(NtStatus::INVALID_DEVICE_REQUEST),
    };
    result.into_raw()
}

/// A new reference to the context stored in a claimed file's slot.
///
/// # Safety
///
/// `slot` must be the context slot of a file claimed by `H`, between its
/// create and close requests.
unsafe fn borrow_context<H: FileHandler>(slot: *mut *mut c_void) -> Option<Shared<H::Context>> {
    // SAFETY: the slot is valid and only written by create and close, which
    // do not run concurrently with other requests on the same file object.
    let context = unsafe { *slot };
    if context.is_null() {
        None
    } else {
        // SAFETY: a non-null slot holds a `Shared<H::Context>` context
        // pointer whose reference is still in place.
        Some(unsafe { Shared::<H::Context>::clone_from_context(context) })
    }
}

unsafe extern "C" fn unload<H: FileHandler>(driver: *mut DRIVER_OBJECT) {
    H::unload();
    let original = ORIGINAL_UNLOAD.load(Ordering::Acquire);
    if original != 0 {
        // SAFETY: `original` is the unload routine the framework had
        // installed in this driver object; it is called exactly as the I/O
        // manager would have called it.
        unsafe {
            let original: unsafe extern "C" fn(*mut DRIVER_OBJECT) =
                core::mem::transmute::<usize, unsafe extern "C" fn(*mut DRIVER_OBJECT)>(original);
            original(driver);
        }
    }
}
