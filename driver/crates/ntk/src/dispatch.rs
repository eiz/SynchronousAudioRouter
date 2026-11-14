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
//! A request names a file in two ways, as the file it is for and, for a
//! create request, as the parent the new file is opened relative to; both
//! are looked at here.
//!
//! Nor does a claimed file carry anything of the handler's. The framework
//! keeps its own object in a file's context slot (`FsContext`) and reads
//! that slot of any file it is handed, also by routes this layer does not
//! see, such as a handle inside one of its requests. So the slot of a
//! claimed file is never written: it stays empty from create to close, and
//! a file the framework is handed anyway looks to it like a file with no
//! object of its own. The handler's context for the file is kept in a table
//! of this module's instead ([`OpenFiles`]), by the file object's address.

use alloc::vec::Vec;
use core::{
    ffi::c_void,
    sync::atomic::{AtomicBool, AtomicPtr, AtomicUsize, Ordering},
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
    irp::{DispatchResult, Irp},
    sync::Mutex,
};

const MAJOR_FUNCTIONS: usize = IRP_MJ_MAXIMUM_FUNCTION as usize + 1;

type DispatchFn = unsafe extern "C" fn(PDEVICE_OBJECT, PIRP) -> NTSTATUS;

/// The framework's dispatch routines, saved when the hooks go in. Written
/// once during DriverEntry (before any request can arrive), read-only after.
static ORIGINAL_DISPATCH: [AtomicUsize; MAJOR_FUNCTIONS] =
    [const { AtomicUsize::new(0) }; MAJOR_FUNCTIONS];
static ORIGINAL_UNLOAD: AtomicUsize = AtomicUsize::new(0);
static INSTALLED: AtomicBool = AtomicBool::new(false);
/// Set once the driver's unload routine is this module's, which also
/// unregisters registry callbacks nobody dropped.
static UNLOAD_HOOKED: AtomicBool = AtomicBool::new(false);

/// The installed handler's [`OpenFiles`], as the context pointer of a
/// `Shared<OpenFiles<H::Context>>`. Stored once during DriverEntry, before
/// the dispatch routines go in, and taken back by the unload routine.
static OPEN_FILES: AtomicPtr<c_void> = AtomicPtr::new(core::ptr::null_mut());

/// The claimed files that are open, each with the handler's context for it,
/// by the address of its file object.
///
/// An address names one file from the create request that opened it to its
/// close request: the file object exists at least until its close request
/// has been handled. An entry is made before the create request is
/// completed, which is before any other request for the file can come, and
/// taken out by the close request, which is the last. So a request finds
/// the entry of its own file, or none.
///
/// The lock is a fast mutex. It is taken at `PASSIVE_LEVEL`, by every
/// create, cleanup, close and device control request on a claimed file (the
/// host's real-time thread's requests included), and held only while the
/// vector is searched, added to or taken from; the vector and the contexts
/// are in non-paged pool, like every allocation of this crate. A context is
/// cloned under the lock and used, or released, after it has been let go.
struct OpenFiles<C> {
    entries: Mutex<Vec<(usize, Shared<C>)>>,
}

impl<C> OpenFiles<C> {
    fn try_new() -> Result<Self, NtStatus> {
        Ok(Self {
            entries: Mutex::try_new(Vec::new())?,
        })
    }

    /// Enters a file that is being opened. If there is no room, the context
    /// is released, after the lock has been let go.
    fn insert(&self, file: usize, context: Shared<C>) -> Result<(), NtStatus> {
        let mut entries = self.entries.lock();
        if entries.try_reserve(1).is_err() {
            drop(entries);
            drop(context);
            return Err(NtStatus::INSUFFICIENT_RESOURCES);
        }
        entries.push((file, context));
        Ok(())
    }

    /// A new reference to the context of an open file.
    fn get(&self, file: usize) -> Option<Shared<C>> {
        let entries = self.entries.lock();
        entries
            .iter()
            .find(|(key, _)| *key == file)
            .map(|(_, context)| context.clone())
    }

    /// Takes a file out. The caller releases the context, with the lock let
    /// go.
    fn remove(&self, file: usize) -> Option<Shared<C>> {
        let mut entries = self.entries.lock();
        let position = entries.iter().position(|(key, _)| *key == file)?;
        Some(entries.swap_remove(position).1)
    }
}

/// The installed handler's table of open files.
///
/// # Safety
///
/// `H` must be the handler that was installed, and the result must not be
/// used once the unload routine has begun. Both hold in `dispatch::<H>`.
unsafe fn open_files<'a, H: FileHandler>() -> Option<&'a OpenFiles<H::Context>> {
    let raw = OPEN_FILES.load(Ordering::Acquire);
    // SAFETY: a non-null pointer came from
    // `Shared::<OpenFiles<H::Context>>::into_context` in
    // `install_file_handler::<H>` (one handler per driver), and that
    // reference is taken back only by the unload routine, after the last
    // use of the result by the caller's contract.
    (!raw.is_null()).then(|| unsafe { Shared::<OpenFiles<H::Context>>::borrow_context(raw) })
}

/// A driver's own handling of the files it claims on a device.
pub trait FileHandler: 'static {
    /// State of one open file.
    type Context: Send + Sync + 'static;

    /// Whether a file opened with this name (relative to the device)
    /// belongs to the handler. Must depend on the name only.
    fn claims(file_name: &[u16]) -> bool;

    /// A claimed file is being opened. The returned context is kept for the
    /// file until its close request, outside the file object: a claimed
    /// file's `FsContext` stays empty.
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
///
/// Fails with `STATUS_INSUFFICIENT_RESOURCES`, having installed nothing, if
/// the table of open files cannot be allocated.
pub fn install_file_handler<H: FileHandler>(driver: &mut DriverObject) -> Result<(), NtStatus> {
    assert!(
        !INSTALLED.swap(true, Ordering::AcqRel),
        "file handler installed twice"
    );
    let files = Shared::try_new(OpenFiles::<H::Context>::try_new()?)?;
    OPEN_FILES.store(files.into_context(), Ordering::Release);
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
    UNLOAD_HOOKED.store(true, Ordering::Release);
    Ok(())
}

/// Whether the driver's unload routine is this module's: it runs
/// [`crate::registry`]'s clean-up of callbacks that were never dropped.
pub(crate) fn unload_routine_installed() -> bool {
    UNLOAD_HOOKED.load(Ordering::Acquire)
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

    // The file, by the address of its file object, if `H` claims it. The
    // file object itself is not written: its `FsContext` stays empty.
    let claimed = irp
        .file()
        .filter(|file| H::claims(file.name()))
        .map(|file| file.as_raw() as usize);
    let Some(file) = claimed else {
        return forward(device, irp).into_raw();
    };
    // SAFETY: this routine is put in place only by
    // `install_file_handler::<H>`, so `H` is the installed handler, and
    // `files` is not used after it returns, which is before the unload
    // routine can begin.
    let Some(files) = (unsafe { open_files::<H>() }) else {
        // Cannot happen: the table is in place before this routine is.
        return irp.complete(NtStatus::INVALID_DEVICE_REQUEST).into_raw();
    };
    // SAFETY: the I/O manager passes a valid device object, which outlives
    // the request.
    let device_ref = unsafe { DeviceObject::from_raw(device) };

    let result = match irp.major_function() {
        IRP_MJ_CREATE => {
            // The file is entered before its create request completes; no
            // other request for it can come before that.
            match H::create(device_ref).and_then(|context| files.insert(file, context)) {
                Ok(()) => irp.complete(NtStatus::SUCCESS),
                Err(status) => irp.complete(status),
            }
        }
        IRP_MJ_CLEANUP => {
            if let Some(context) = files.get(file) {
                H::cleanup(device_ref, &context);
            }
            irp.complete(NtStatus::SUCCESS)
        }
        IRP_MJ_CLOSE => {
            // Close is the last request for a file object. The file is taken
            // out, and its context released, before the request completes:
            // only after that can the object's address be another file's.
            drop(files.remove(file));
            irp.complete(NtStatus::SUCCESS)
        }
        IRP_MJ_DEVICE_CONTROL => match files.get(file) {
            Some(context) => H::device_control(device_ref, &context, irp),
            None => irp.complete(NtStatus::INVALID_DEVICE_REQUEST),
        },
        _ => irp.complete(NtStatus::INVALID_DEVICE_REQUEST),
    };
    result.into_raw()
}

unsafe extern "C" fn unload<H: FileHandler>(driver: *mut DRIVER_OBJECT) {
    H::unload();
    // A registry callback whose value was leaked or forgotten would be
    // called into this image after it has gone.
    crate::registry::unregister_remaining();
    // Plug and Play does not unload the driver while its device has open
    // handles (section 11 of the specification): every claimed file has
    // been closed, and the table is empty.
    let files = OPEN_FILES.swap(core::ptr::null_mut(), Ordering::AcqRel);
    if !files.is_null() {
        // SAFETY: the pointer came from
        // `Shared::<OpenFiles<H::Context>>::into_context` in
        // `install_file_handler::<H>` (one handler per driver), and the swap
        // takes it back exactly once. No dispatch routine runs from here on,
        // so nothing borrows it.
        drop(unsafe { Shared::<OpenFiles<H::Context>>::from_context(files) });
    }
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
