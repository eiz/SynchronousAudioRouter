// Copyright (c) 2026 Itty Bitty Binaries, Inc.
// SPDX-License-Identifier: MIT

//! Filter factories, filters and pins.
//!
//! # Who owns a factory's memory
//!
//! Everything AVStream reads on behalf of a factory (the filter descriptor
//! and the tables it points to) and everything the trampolines reach through
//! a `Context` pointer is one allocation, the factory's *block*. The
//! factory's filters and their pins point into the same block: AVStream
//! gives a new filter its factory's descriptor and `Context`, and a new pin
//! its filter's.
//!
//! The block belongs to AVStream's object bags. It is put in the factory's
//! bag when the factory is created, and in the bag of every filter and pin
//! when AVStream creates one. To AVStream an item that is in several bags is
//! still one item: it counts the bags, and calls the item's free routine
//! once, when the item has left the last of them ("Object Bags" in the
//! Windows driver documentation). A bag is emptied when its object is
//! deleted. So `free_block` runs when the factory and every filter and pin
//! made from it are gone, and not before: nothing AVStream still has can
//! point into the block then.
//!
//! It follows that this crate keeps no reference count of its own on a
//! block, and must not take one per bag: the free routine runs once per
//! item, not once per bag. (Taking one per bag is how every block, and with
//! it the driver's state for every filter, used to stay allocated for good.)
//!
//! When the factory object itself goes is a separate matter, and the handle
//! has to know it: see [`FilterFactory`].

use alloc::{boxed::Box, vec::Vec};
use core::{
    ffi::c_void,
    marker::PhantomData,
    sync::atomic::{AtomicBool, Ordering},
};

use ks_sys as sys;
use ntk::{
    alloc_ext::{try_box, try_vec_from_slice, Shared},
    work::WorkItem,
    Guid,
};
use ntstatus::{IoStatus, NtStatus};
use wdk_sys::{IRP, IRP_MJ_DEVICE_CONTROL, NTSTATUS, PIRP};

use crate::{
    context::{self, Header, Kind},
    descriptor::{FilterSpec, Storage},
    device::{DeviceGuard, FactoryCalls},
};

/// A driver's behavior for the filters of one kind of filter factory.
///
/// All callbacks run at `PASSIVE_LEVEL`. `Context` is the value given to
/// [`FilterFactory::create`]; it stays alive as long as the factory or any
/// filter or pin created from it exists, and the factory's reference to it
/// is released when the last of them has gone.
pub trait FilterHandler: 'static {
    type Context: Send + Sync + 'static;

    /// A filter is being opened. An error fails the open.
    fn filter_create(context: &Self::Context) -> Result<(), NtStatus>;

    /// A filter opened successfully is being closed.
    fn filter_close(context: &Self::Context);

    /// A stream is being opened on a streaming pin. An error fails the open.
    fn pin_create(context: &Self::Context, pin: &Pin) -> Result<(), NtStatus>;

    /// A stream opened successfully is being closed.
    fn pin_close(context: &Self::Context, pin: &Pin);

    /// A stream is moving between the `KSSTATE` values `from` and `to`.
    fn pin_set_state(context: &Self::Context, pin: &Pin, to: u32, from: u32) -> Result<(), NtStatus>;

    /// A property request the filter's description declared. `data` is the
    /// value buffer: the caller's value for a set, the space for the answer
    /// for a get (possibly empty).
    fn property(context: &Self::Context, target: Target<'_>, call: &PropertyCall<'_>, data: &mut [u8]) -> IoStatus;

    /// Data-range intersection on pin `pin_id`: the format that lies in the
    /// pin's range and in `caller_range`.
    fn intersect(context: &Self::Context, pin_id: u32, caller_range: &[u8]) -> Result<DataFormat, NtStatus>;
}

/// What a property request is addressed to.
pub enum Target<'a> {
    Filter,
    Pin(&'a Pin),
    /// A topology node; the request carries the node id.
    Node,
}

/// A property request.
///
/// It does not say whether the request is marked as coming from user or
/// from kernel mode, on purpose. That mark is not the caller's identity:
/// Windows converts the requests of 32-bit processes on the way down and
/// passes them on as kernel-mode requests, with the process's own handle
/// values and sizes still in them. A handler has to treat everything in a
/// request as the requesting process's, always.
pub struct PropertyCall<'a> {
    /// The property set.
    pub set: Guid,
    /// The property within the set.
    pub id: u32,
    /// Whether this is a set (otherwise a get).
    pub is_set: bool,
    /// The whole request: the 24-byte `KSPROPERTY` and what follows it.
    pub request: &'a [u8],
}

/// A data format returned from intersection: up to 128 bytes.
pub struct DataFormat {
    bytes: [u8; Self::CAPACITY],
    len: usize,
}

impl DataFormat {
    pub const CAPACITY: usize = 128;

    /// `None` if `bytes` is longer than [`Self::CAPACITY`].
    pub fn new(bytes: &[u8]) -> Option<Self> {
        if bytes.len() > Self::CAPACITY {
            return None;
        }
        let mut out = Self {
            bytes: [0; Self::CAPACITY],
            len: bytes.len(),
        };
        out.bytes[..bytes.len()].copy_from_slice(bytes);
        Some(out)
    }

    pub fn as_bytes(&self) -> &[u8] {
        &self.bytes[..self.len]
    }
}

/// An open stream (`KSPIN`).
#[repr(transparent)]
pub struct Pin(sys::KSPIN);

impl Pin {
    /// The pin's index in its filter's description.
    pub fn id(&self) -> u32 {
        self.0.Id
    }

    /// A value that identifies this stream among the open ones.
    pub fn key(&self) -> usize {
        core::ptr::from_ref(self) as usize
    }

    /// The format the stream was opened with: a `KSDATAFORMAT` and whatever
    /// follows it, `FormatSize` bytes in all.
    ///
    /// Empty unless the pin was declared with `KSPIN_FLAG_FIXED_FORMAT`.
    /// Only then can the format not be changed (and its memory replaced)
    /// while the slice is borrowed: "Any attempt to set the data format
    /// returns STATUS_INVALID_DEVICE_REQUEST."
    pub fn connection_format(&self) -> &[u8] {
        let descriptor = self.0.Descriptor;
        // SAFETY: a pin's Descriptor is null or points to its descriptor in
        // the filter's description, which lives at least as long as the pin
        // (in this crate, in the factory's block).
        let fixed = unsafe { descriptor.as_ref() }
            .is_some_and(|descriptor| descriptor.Flags & sys::KSPIN_FLAG_FIXED_FORMAT != 0);
        let format = self.0.ConnectionFormat;
        if !fixed || format.is_null() {
            return &[];
        }
        // SAFETY: AVStream validated the connection request before creating
        // the pin: ConnectionFormat points to a format of FormatSize bytes
        // (at least a KSDATAFORMAT) that lives as long as the pin, and with a
        // fixed format it is not replaced while the pin exists.
        unsafe {
            // FormatSize is the first field of a KSDATAFORMAT.
            let size = format.cast::<u32>().read() as usize;
            if size < core::mem::size_of::<sys::KSDATAFORMAT>() {
                return &[];
            }
            core::slice::from_raw_parts(format.cast::<u8>(), size)
        }
    }
}

/// The block behind the `Context` of a factory and of everything created
/// from it. Owned by AVStream's object bags; see the module documentation.
#[repr(C)]
struct FactoryBlock<H: FilterHandler> {
    header: Header,
    user: Shared<H::Context>,
    /// The descriptor AVStream points into; freed with the block.
    _storage: Box<Storage>,
    /// What frees the block; see `free_block`.
    release: Option<WorkItem>,
}

// SAFETY: a block is plain memory plus the handler's context, which is
// `Send + Sync`. The pointers inside the descriptor point into the
// descriptor's own allocations and are never followed by this crate; by the
// time a block changes threads (to be freed) AVStream has let go of it.
unsafe impl<H: FilterHandler> Send for FactoryBlock<H> {}

struct FactoryState {
    /// Cleared when AVStream destroys the factory object.
    alive: AtomicBool,
}

/// A filter factory created at run time.
///
/// AVStream may destroy the factory object on its own (when the device
/// stops). After [`delete`](Self::delete) it destroys the object at once if
/// no filter is open, and otherwise when the last open filter closes. The
/// handle is told through an item of its own in the factory's bag, and turns
/// its methods into no-ops from then on.
///
/// # The device mutex
///
/// Creating a factory and deleting it need the device mutex (Microsoft's
/// page for `KsCreateFilterFactory`; "Device Mutex in AVStream"), and the
/// caller proves that it holds it. The other calls neither need it nor
/// mind it: their reference pages name no mutex, and they return while
/// another thread holds it. A driver may therefore make them inside a
/// longer hold, so that Windows sees several of them as one step, or
/// outside any. What they do need is the factory to exist for the duration
/// of the call, and that is arranged here:
///
/// * The handle cannot be copied, the calls take it by `&mut` and `delete`
///   consumes it. So no call can overlap the driver's own deletion of the
///   factory, or another call on it.
/// * AVStream's own destruction of factories, when the device stops, waits
///   for calls that are under way and refuses later ones
///   ([`FactoryCalls`]).
pub struct FilterFactory {
    raw: sys::PKSFILTERFACTORY,
    /// The device the factory was created on, by address: the device whose
    /// mutex `delete` needs.
    device: usize,
    state: Shared<FactoryState>,
    calls: FactoryCalls,
}

// SAFETY: the raw pointer is only dereferenced as described above; the
// handle itself is plain data and may move between threads. (It is not
// `Sync`, and needs not be: every use takes it by `&mut` or by value.)
unsafe impl Send for FilterFactory {}

impl FilterFactory {
    /// Adds a filter factory to the device: filters described by `spec`,
    /// opened under the reference string `reference` (which must end with a
    /// NUL and contain no other), with behavior `H` and state `context`.
    ///
    /// The factory is freed by AVStream when the device stops, unless it is
    /// deleted first. Its device interfaces are registered but off; see
    /// [`set_enabled`](Self::set_enabled).
    ///
    /// The caller holds the device mutex, which creating a factory needs.
    pub fn create<H: FilterHandler>(
        guard: &DeviceGuard<'_>,
        spec: &FilterSpec<'_>,
        reference: &[u16],
        context: Shared<H::Context>,
    ) -> Result<Self, NtStatus> {
        if reference.last() != Some(&0) || reference[..reference.len() - 1].contains(&0) {
            return Err(NtStatus::INVALID_PARAMETER);
        }
        let device = guard.device();
        let calls = device.factory_calls().ok_or(NtStatus::INVALID_DEVICE_REQUEST)?;
        let storage = Storage::build::<H>(spec)?;
        let descriptor = storage.descriptor();
        let state = Shared::try_new(FactoryState {
            alive: AtomicBool::new(true),
        })?;
        let release = WorkItem::try_new(device.functional_device_object())?;
        let block = try_box(FactoryBlock::<H> {
            header: Header::new::<H>(Kind::Factory),
            user: context,
            _storage: storage,
            release: Some(release),
        })?;
        let gone = state.clone().into_context();

        let fdo = device.functional_device_object();
        let mut raw: sys::PKSFILTERFACTORY = core::ptr::null_mut();
        // SAFETY: the device mutex is held (`guard`). The descriptor lives
        // in `block`, which is handed to the factory's object bag below and
        // so outlives the factory (if that fails, the factory is deleted
        // before the block is). The reference string is NUL-terminated and
        // only read.
        let status = unsafe {
            sys::KsCreateFilterFactory(
                fdo.as_raw(),
                descriptor,
                reference.as_ptr().cast_mut(),
                core::ptr::null_mut(),
                sys::KSCREATE_ITEM_FREEONSTOP,
                None,
                None,
                &mut raw,
            )
        };
        if let Err(status) = NtStatus(status).into_result() {
            // SAFETY: the reference was not handed to a bag; it is taken
            // back once.
            drop(unsafe { Shared::<FactoryState>::from_context(gone) });
            return Err(status);
        }

        let block = Box::into_raw(block).cast::<c_void>();
        // SAFETY: `raw` is the factory just created; the device mutex is
        // still held, so AVStream cannot have created a filter from it or
        // destroyed it, and its bag may be added to. Two items go in:
        //
        // * the block, which AVStream from then on owns and frees through
        //   `free_block::<H>` (module documentation). If it cannot be added,
        //   nothing of ours is in a bag. The factory is then deleted, and
        //   AVStream destroys a factory that has no filter before that call
        //   returns; only after it is the block freed here.
        // * a reference to the handle's state, in this bag only, so that
        //   `factory_gone` runs exactly when AVStream destroys the factory
        //   object, however long filters keep the block. If it cannot be
        //   added, deleting the factory leaves the block to AVStream.
        //
        // Only then is the Context published (until that store the factory
        // carries the device's context, which the trampolines reject).
        let failed = unsafe {
            let status = sys::KsAddItemToObjectBag((*raw).Bag, block, Some(free_block::<H>));
            if !NtStatus(status).is_success() {
                let _ = sys::KsDeleteFilterFactory(raw);
                Some((status, Some(block)))
            } else {
                let status = sys::KsAddItemToObjectBag((*raw).Bag, gone, Some(factory_gone));
                if !NtStatus(status).is_success() {
                    let _ = sys::KsDeleteFilterFactory(raw);
                    Some((status, None))
                } else {
                    (*raw).Context = block;
                    None
                }
            }
        };
        if let Some((status, block)) = failed {
            // SAFETY: neither failure left the state's reference in a bag,
            // and the first left the block in none; each is taken back once.
            unsafe {
                drop(Shared::<FactoryState>::from_context(gone));
                if let Some(block) = block {
                    drop(Box::from_raw(block.cast::<FactoryBlock<H>>()));
                }
            }
            return Err(NtStatus(status));
        }
        Ok(Self {
            raw,
            device: device.as_raw() as usize,
            state,
            calls,
        })
    }

    /// Runs a kernel streaming call on the factory without the device
    /// mutex, if the factory still exists.
    fn call<R>(&mut self, call: impl FnOnce(sys::PKSFILTERFACTORY) -> R) -> Result<R, NtStatus> {
        let raw = self.raw;
        let state = &self.state;
        self.calls
            .run(|| {
                // Looked at inside the gate: AVStream destroys a factory
                // the driver has not deleted only when the device stops,
                // and a stop waits at the gate.
                state.alive.load(Ordering::Acquire).then(|| call(raw))
            })
            .flatten()
            .ok_or(NtStatus::NOT_FOUND)
    }

    /// The kernel path of the factory's device interface in its first
    /// category.
    pub fn symbolic_link(&mut self) -> Result<Vec<u16>, NtStatus> {
        self.call(|raw| {
            // SAFETY: the factory exists for the duration of the call (type
            // documentation).
            let link = unsafe { sys::KsFilterFactoryGetSymbolicLink(raw) };
            if link.is_null() {
                return Err(NtStatus::NOT_FOUND);
            }
            // SAFETY: a non-null result points to the factory's own
            // UNICODE_STRING, valid while the factory is.
            let units = unsafe {
                let s = &*link;
                if s.Buffer.is_null() {
                    &[][..]
                } else {
                    core::slice::from_raw_parts(s.Buffer, usize::from(s.Length / 2))
                }
            };
            try_vec_from_slice(units)
        })?
    }

    /// Has AVStream write the factory's cached filter data (the `FilterData`
    /// value and the medium cache that Windows keeps with the factory's
    /// device interfaces) from the factory's own descriptor.
    pub fn update_cache_data(&mut self) -> Result<(), NtStatus> {
        // SAFETY: the factory exists for the duration of the call (type
        // documentation). A null descriptor is allowed: AVStream then reads
        // the factory's own, which lives in the factory's block and so as
        // long as the factory.
        let status = self.call(|raw| unsafe { sys::KsFilterFactoryUpdateCacheData(raw, core::ptr::null()) })?;
        NtStatus(status).into_result()
    }

    /// Switches the factory's device interfaces on or off.
    pub fn set_enabled(&mut self, enabled: bool) -> Result<(), NtStatus> {
        // SAFETY: the factory exists for the duration of the call (type
        // documentation).
        let status =
            self.call(|raw| unsafe { sys::KsFilterFactorySetDeviceClassesState(raw, u8::from(enabled)) })?;
        NtStatus(status).into_result()
    }

    /// Deletes the factory: no new filter can be opened from it. Filters
    /// that are open keep working until they are closed. The caller holds
    /// the device mutex, which deleting a factory needs.
    ///
    /// # Panics
    ///
    /// If `guard` is not for the device the factory was created on: its
    /// mutex would not be held.
    pub fn delete(self, guard: &DeviceGuard<'_>) {
        assert!(
            guard.device().as_raw() as usize == self.device,
            "filter factory deleted under another device's mutex"
        );
        if !self.state.alive.load(Ordering::Acquire) {
            return;
        }
        // SAFETY: the factory exists: it is alive, the handle is consumed
        // here so it has not been deleted before, and AVStream destroys a
        // factory that has not been deleted only under the device mutex,
        // which is held (`guard`, checked above to be for the factory's own
        // device). It is deleted exactly once.
        unsafe {
            let _ = sys::KsDeleteFilterFactory(self.raw);
        }
    }
}

/// Free routine of the item that is in a factory's bag only: AVStream is
/// destroying the factory object, so the handle's pointer to it is dead.
unsafe extern "C" fn factory_gone(data: *mut c_void) {
    // SAFETY: `data` is the reference `FilterFactory::create` put in the
    // factory's bag, delivered exactly once.
    let state = unsafe { Shared::<FactoryState>::from_context(data) };
    state.alive.store(false, Ordering::Release);
}

/// Free routine of a factory's block: it has left the last bag it was in,
/// so the factory and every filter and pin created from it have been
/// deleted.
///
/// AVStream calls this while it destroys the last of those objects, as a
/// rule with the device mutex held, and every open and close of a filter on
/// the device waits for that. Freeing a block means freeing the descriptor
/// tables and, often, the last reference to the driver's state for the
/// factory, with whatever that state owns. So the block is not freed here:
/// it is handed to a system worker thread, through a work item that was
/// allocated with the block (which is why this cannot fail), and that keeps
/// the device, and with it the driver, in place until the block has been
/// freed.
unsafe extern "C" fn free_block<H: FilterHandler>(data: *mut c_void) {
    // SAFETY: `data` is the block `FilterFactory::create` gave to the
    // factory's bag. AVStream calls an item's free routine once, when no bag
    // holds the item any more; the bags that held it are those of the
    // objects whose descriptor and Context point into it, and all of them
    // are gone.
    let mut block = unsafe { Box::from_raw(data.cast::<FactoryBlock<H>>()) };
    match block.release.take() {
        Some(release) => release.queue_drop(block),
        // Cannot happen: a block gets its work item when it is created.
        None => drop(block),
    }
}

/// The factory block behind an AVStream object's `Context`, if it is one
/// created for `H`.
///
/// # Safety
///
/// `context` must be the `Context` of a live AVStream filter or pin of this
/// driver (or null).
unsafe fn block_of<'a, H: FilterHandler>(context: *mut c_void) -> Option<&'a FactoryBlock<H>> {
    // SAFETY: by the crate invariant a non-null Context is one of this
    // crate's tagged blocks, kept alive by the bag of the object's parent
    // (and, once `share_with_bag` has run, by the object's own).
    if unsafe { context::is::<H>(context, Kind::Factory) } {
        // SAFETY: the header check proves the type.
        Some(unsafe { &*context.cast::<FactoryBlock<H>>() })
    } else {
        None
    }
}

/// Puts the block an AVStream object's `Context` points to in that object's
/// own bag as well, so that the block cannot be freed while the object
/// exists. This takes no reference: the block stays one item, which AVStream
/// frees once, when the last bag that holds it is emptied.
///
/// # Safety
///
/// `context` must have passed `block_of::<H>`, `bag` must be the object bag
/// of the object that carries it, and the mutex that guards that bag (for a
/// filter or a pin, the filter control mutex) must be held.
unsafe fn share_with_bag<H: FilterHandler>(context: *mut c_void, bag: sys::KSOBJECT_BAG) -> Result<(), NtStatus> {
    // SAFETY: the block is live (the bag of the object's parent holds it)
    // and already has `free_block::<H>` as its free routine; the caller
    // holds the bag's mutex.
    let status = unsafe { sys::KsAddItemToObjectBag(bag, context, Some(free_block::<H>)) };
    NtStatus(status).into_result()
}

fn status_of(result: Result<(), NtStatus>) -> NTSTATUS {
    match result {
        Ok(()) => NtStatus::SUCCESS.0,
        // An error value that is really a success code would make AVStream
        // carry on with an object the handler refused.
        Err(status) if status.is_success() => NtStatus::UNSUCCESSFUL.0,
        Err(status) => status.0,
    }
}

/// The C-ABI entry points AVStream calls, generic over the handler.
pub(crate) mod trampolines {
    use super::*;

    struct Tables<H>(PhantomData<H>);

    impl<H: FilterHandler> Tables<H> {
        const FILTER: sys::KSFILTER_DISPATCH = sys::KSFILTER_DISPATCH {
            Create: Some(filter_create::<H>),
            Close: Some(filter_close::<H>),
            Process: None,
            Reset: None,
        };
        const PIN: sys::KSPIN_DISPATCH = sys::KSPIN_DISPATCH {
            Create: Some(pin_create::<H>),
            Close: Some(pin_close::<H>),
            Process: Some(pin_process),
            Reset: None,
            SetDataFormat: None,
            SetDeviceState: Some(pin_set_device_state::<H>),
            Connect: None,
            Disconnect: None,
            Clock: core::ptr::null(),
            Allocator: core::ptr::null(),
        };
    }

    pub(crate) fn filter_dispatch<H: FilterHandler>() -> *const sys::KSFILTER_DISPATCH {
        let table: &'static sys::KSFILTER_DISPATCH = &Tables::<H>::FILTER;
        table
    }

    pub(crate) fn pin_dispatch<H: FilterHandler>() -> *const sys::KSPIN_DISPATCH {
        let table: &'static sys::KSPIN_DISPATCH = &Tables::<H>::PIN;
        table
    }

    unsafe extern "C" fn filter_create<H: FilterHandler>(filter: sys::PKSFILTER, _irp: PIRP) -> NTSTATUS {
        // SAFETY: AVStream passes the new, valid KSFILTER, whose Context it
        // copied from the parent factory.
        let (context, bag) = unsafe { ((*filter).Context, (*filter).Bag) };
        // SAFETY: `context` is a live filter's Context.
        let Some(block) = (unsafe { block_of::<H>(context) }) else {
            return NtStatus::NOT_FOUND.0;
        };
        // SAFETY: `context` passed `block_of`, `bag` is this filter's bag,
        // and AVStream holds the filter control mutex while it calls a
        // filter's Create dispatch.
        if let Err(status) = unsafe { share_with_bag::<H>(context, bag) } {
            return status.0;
        }
        status_of(H::filter_create(&block.user))
    }

    unsafe extern "C" fn filter_close<H: FilterHandler>(filter: sys::PKSFILTER, _irp: PIRP) -> NTSTATUS {
        // SAFETY: AVStream passes a valid KSFILTER whose create succeeded,
        // so its bag holds the block.
        if let Some(block) = unsafe { block_of::<H>((*filter).Context) } {
            H::filter_close(&block.user);
        }
        NtStatus::SUCCESS.0
    }

    unsafe extern "C" fn pin_create<H: FilterHandler>(pin: sys::PKSPIN, _irp: PIRP) -> NTSTATUS {
        // SAFETY: AVStream passes the new, valid KSPIN, whose Context it
        // copied from the parent filter.
        let (context, bag) = unsafe { ((*pin).Context, (*pin).Bag) };
        // SAFETY: `context` is a live pin's Context.
        let Some(block) = (unsafe { block_of::<H>(context) }) else {
            return NtStatus::NOT_FOUND.0;
        };
        // SAFETY: `context` passed `block_of`, `bag` is this pin's bag, and
        // AVStream holds the parent filter's control mutex while it calls a
        // pin's Create dispatch.
        if let Err(status) = unsafe { share_with_bag::<H>(context, bag) } {
            return status.0;
        }
        // SAFETY: `Pin` is a transparent wrapper of the valid KSPIN.
        let pin = unsafe { &*pin.cast::<Pin>() };
        status_of(H::pin_create(&block.user, pin))
    }

    unsafe extern "C" fn pin_close<H: FilterHandler>(pin: sys::PKSPIN, _irp: PIRP) -> NTSTATUS {
        // SAFETY: AVStream passes a valid KSPIN whose create succeeded, so
        // its bag holds the block.
        if let Some(block) = unsafe { block_of::<H>((*pin).Context) } {
            // SAFETY: transparent wrapper of the valid KSPIN.
            H::pin_close(&block.user, unsafe { &*pin.cast::<Pin>() });
        }
        NtStatus::SUCCESS.0
    }

    unsafe extern "C" fn pin_set_device_state<H: FilterHandler>(
        pin: sys::PKSPIN,
        to: sys::KSSTATE::Type,
        from: sys::KSSTATE::Type,
    ) -> NTSTATUS {
        // SAFETY: as for `pin_close`.
        let Some(block) = (unsafe { block_of::<H>((*pin).Context) }) else {
            return NtStatus::NOT_FOUND.0;
        };
        // SAFETY: transparent wrapper of the valid KSPIN.
        let pin = unsafe { &*pin.cast::<Pin>() };
        status_of(H::pin_set_state(&block.user, pin, to as u32, from as u32))
    }

    /// The streaming pin moves no data through kernel streaming, so there
    /// is nothing to process. `STATUS_PENDING` tells AVStream not to call
    /// again until something asks for processing, which nothing does.
    unsafe extern "C" fn pin_process(_pin: sys::PKSPIN) -> NTSTATUS {
        NtStatus::PENDING.0
    }

    /// Lengths of the request and value buffers of the property request in
    /// `irp`.
    ///
    /// # Safety
    ///
    /// `irp` must be the valid IRP AVStream passed to a property handler.
    unsafe fn property_lengths(irp: PIRP) -> Option<(usize, usize)> {
        // SAFETY: valid IRP with a valid current stack location; a property
        // request is a device control request, for which DeviceIoControl is
        // the active member of Parameters.
        unsafe {
            let stack = (*irp).Tail.Overlay.__bindgen_anon_2.__bindgen_anon_1.CurrentStackLocation;
            if u32::from((*stack).MajorFunction) != IRP_MJ_DEVICE_CONTROL {
                return None;
            }
            let p = &(*stack).Parameters.DeviceIoControl;
            Some((p.InputBufferLength as usize, p.OutputBufferLength as usize))
        }
    }

    /// Common part of the property trampolines.
    ///
    /// # Safety
    ///
    /// `irp`, `request` and `data` must be the arguments AVStream passed to
    /// a property handler, and `context` the Context of the live filter or
    /// pin the request is for.
    unsafe fn property<H: FilterHandler>(
        irp: PIRP,
        request: sys::PKSIDENTIFIER,
        data: *mut c_void,
        context: *mut c_void,
        target: Target<'_>,
    ) -> NTSTATUS {
        // SAFETY: `context` is a live object's Context.
        let Some(block) = (unsafe { block_of::<H>(context) }) else {
            return NtStatus::NOT_FOUND.0;
        };
        // SAFETY: `irp` is the handler's IRP.
        let Some((request_length, data_length)) = (unsafe { property_lengths(irp) }) else {
            return NtStatus::INVALID_DEVICE_REQUEST.0;
        };
        if request.is_null() || request_length < core::mem::size_of::<sys::KSIDENTIFIER>() {
            return NtStatus::INVALID_PARAMETER.0;
        }
        // SAFETY: AVStream hands the handler its own aligned copy of the
        // request, InputBufferLength bytes long, and a value buffer of
        // OutputBufferLength bytes (both in kernel memory that lives until
        // the IRP completes; the two do not overlap). That is so for a
        // request marked as coming from kernel mode as well, which is how
        // the converted request of a 32-bit process arrives.
        let (request_bytes, identifier) = unsafe {
            (
                core::slice::from_raw_parts(request.cast::<u8>().cast_const(), request_length),
                &(*request).__bindgen_anon_1.__bindgen_anon_1,
            )
        };
        let is_set = identifier.Flags & sys::KSPROPERTY_TYPE_SET != 0;
        let data_bytes = if data.is_null() || data_length == 0 {
            &mut [][..]
        } else {
            // For a set the value buffer holds the caller's value. For
            // anything else it is space for the answer, which nothing says
            // AVStream has filled: it is zeroed before the handler sees it,
            // so that neither a handler that reads it nor one that reports
            // more than it wrote can see or hand out what was in that
            // memory before.
            if !is_set {
                // SAFETY: the value buffer, as above: `data_length` writable
                // bytes that no reference covers yet.
                unsafe { core::ptr::write_bytes(data.cast::<u8>(), 0, data_length) };
            }
            // SAFETY: the value buffer, as above, and every byte of it
            // initialised: by the caller for a set, by the line above
            // otherwise.
            unsafe { core::slice::from_raw_parts_mut(data.cast::<u8>(), data_length) }
        };
        let call = PropertyCall {
            set: identifier.Set,
            id: identifier.Id,
            is_set,
            request: request_bytes,
        };
        let outcome = H::property(&block.user, target, &call, data_bytes);
        let mut information = outcome.information;
        if !outcome.status.is_error() && data_length != 0 {
            // AVStream copies `Information` bytes of the value buffer back
            // to the caller; never more than the buffer holds.
            information = information.min(data_length);
        }
        // SAFETY: a property handler reports its byte count through the
        // IRP's status block; the IRP is valid and owned by AVStream, which
        // completes it with the returned status.
        unsafe { set_information(irp, information) };
        outcome.status.0
    }

    /// # Safety
    ///
    /// `irp` must be a valid IRP.
    unsafe fn set_information(irp: *mut IRP, information: usize) {
        // SAFETY: valid IRP by the caller's contract.
        unsafe { (*irp).IoStatus.Information = information as u64 };
    }

    pub(crate) unsafe extern "C" fn filter_property<H: FilterHandler>(
        irp: PIRP,
        request: sys::PKSIDENTIFIER,
        data: *mut c_void,
    ) -> NTSTATUS {
        // SAFETY: AVStream calls a filter's property handler with the IRP of
        // a request on that filter.
        let filter = unsafe { sys::KsGetFilterFromIrp(irp) };
        if filter.is_null() {
            return NtStatus::INVALID_DEVICE_REQUEST.0;
        }
        // SAFETY: arguments forwarded from AVStream; the filter is live for
        // the duration of its own request.
        unsafe { property::<H>(irp, request, data, (*filter).Context, Target::Filter) }
    }

    pub(crate) unsafe extern "C" fn node_property<H: FilterHandler>(
        irp: PIRP,
        request: sys::PKSIDENTIFIER,
        data: *mut c_void,
    ) -> NTSTATUS {
        // A node request arrives on a filter or on one of its pins;
        // KsGetFilterFromIrp resolves both to the filter.
        // SAFETY: as for `filter_property`.
        let filter = unsafe { sys::KsGetFilterFromIrp(irp) };
        if filter.is_null() {
            return NtStatus::INVALID_DEVICE_REQUEST.0;
        }
        // SAFETY: as for `filter_property`.
        unsafe { property::<H>(irp, request, data, (*filter).Context, Target::Node) }
    }

    pub(crate) unsafe extern "C" fn pin_property<H: FilterHandler>(
        irp: PIRP,
        request: sys::PKSIDENTIFIER,
        data: *mut c_void,
    ) -> NTSTATUS {
        // SAFETY: AVStream calls a pin's property handler with the IRP of a
        // request on that pin.
        let pin = unsafe { sys::KsGetPinFromIrp(irp) };
        if pin.is_null() {
            return NtStatus::INVALID_DEVICE_REQUEST.0;
        }
        // SAFETY: arguments forwarded from AVStream; the pin is live for the
        // duration of its own request; `Pin` is a transparent wrapper.
        unsafe { property::<H>(irp, request, data, (*pin).Context, Target::Pin(&*pin.cast::<Pin>())) }
    }

    pub(crate) unsafe extern "C" fn intersect<H: FilterHandler>(
        _context: *mut c_void,
        irp: PIRP,
        pin: sys::PKSP_PIN,
        data_range: sys::PKSDATARANGE,
        _matching_data_range: sys::PKSDATARANGE,
        data_buffer_size: u32,
        data: *mut c_void,
        data_size: *mut u32,
    ) -> NTSTATUS {
        // SAFETY: AVStream calls the intersection handler with the IRP of a
        // request on the filter.
        let filter = unsafe { sys::KsGetFilterFromIrp(irp) };
        if filter.is_null() || pin.is_null() || data_range.is_null() || data_size.is_null() {
            return NtStatus::INVALID_PARAMETER.0;
        }
        // SAFETY: the filter is live for the duration of its own request.
        let Some(block) = (unsafe { block_of::<H>((*filter).Context) }) else {
            return NtStatus::NOT_FOUND.0;
        };
        // `pin` is the KSP_PIN at the start of AVStream's copy of the
        // request, which is as long as the IRP's input length, and
        // `data_range` is one of the caller's ranges inside that copy. How
        // long the range is, is the caller's word (its FormatSize): the
        // range is used only if all of it lies inside the request, whoever
        // the request claims to come from.
        // SAFETY: the IRP is the one AVStream passed to the handler.
        let Some((request_length, _)) = (unsafe { property_lengths(irp) }) else {
            return NtStatus::INVALID_DEVICE_REQUEST.0;
        };
        let (request_start, range_start) = (pin as usize, data_range as usize);
        let room = request_start
            .checked_add(request_length)
            .filter(|_| range_start >= request_start)
            .and_then(|request_end| request_end.checked_sub(range_start));
        let Some(room) = room.filter(|_| request_length >= core::mem::size_of::<sys::KSP_PIN>()) else {
            return NtStatus::INVALID_PARAMETER.0;
        };
        if room < core::mem::size_of::<sys::KSDATARANGE>() {
            return NtStatus::NO_MATCH.0;
        }
        // SAFETY: the request holds a whole KSP_PIN at `pin` and at least a
        // KSDATARANGE at `data_range` (both checked above), and no more of
        // the range is read than the request has room for.
        let (pin_id, caller_range) = unsafe {
            // FormatSize is the first field of a KSDATARANGE.
            let size = data_range.cast::<u32>().read() as usize;
            if size < core::mem::size_of::<sys::KSDATARANGE>() || size > room {
                return NtStatus::NO_MATCH.0;
            }
            (
                (*pin).PinId,
                core::slice::from_raw_parts(data_range.cast::<u8>().cast_const(), size),
            )
        };
        let format = match H::intersect(&block.user, pin_id, caller_range) {
            Ok(format) => format,
            Err(status) => return status_of(Err(status)),
        };
        let bytes = format.as_bytes();
        // SAFETY: `data_size` is AVStream's out-parameter for the size of
        // the format. `data` is a buffer of `data_buffer_size` bytes, and is
        // written only when it is large enough.
        unsafe {
            *data_size = bytes.len() as u32;
            if data_buffer_size == 0 {
                // A size query.
                return NtStatus::BUFFER_OVERFLOW.0;
            }
            if (data_buffer_size as usize) < bytes.len() || data.is_null() {
                return NtStatus::BUFFER_TOO_SMALL.0;
            }
            core::ptr::copy_nonoverlapping(bytes.as_ptr(), data.cast::<u8>(), bytes.len());
        }
        NtStatus::SUCCESS.0
    }
}
