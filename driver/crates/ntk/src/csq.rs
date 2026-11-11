// Copyright (c) 2026 Itty Bitty Binaries, Inc.
// SPDX-License-Identifier: MIT

//! A cancel-safe queue of pending IRPs with driver state under the same
//! lock.
//!
//! Parking an IRP correctly means dealing with cancellation racing against
//! everything else; Windows provides the cancel-safe queue (`IO_CSQ`)
//! framework for that, and this module wraps it. The queue's spin lock also
//! protects a value of the driver's choosing, so that "look at my state and
//! park the request if there is nothing to hand out" is one atomic step.

use alloc::boxed::Box;
use core::{
    cell::UnsafeCell,
    ffi::c_void,
    mem::MaybeUninit,
    ptr::{addr_of_mut, NonNull},
};

use ntstatus::NtStatus;
use wdk_sys::{
    ntddk::{
        IoCsqInitializeEx, IoCsqInsertIrpEx, IoCsqRemoveNextIrp, IofCompleteRequest,
        KeAcquireSpinLockRaiseToDpc, KeReleaseSpinLock,
    },
    IO_CSQ, IO_NO_INCREMENT, IRP, KIRQL, KSPIN_LOCK, LIST_ENTRY, NTSTATUS, PIO_CSQ, PIRP, PKIRQL,
};

use crate::{
    alloc_ext::try_box,
    irp::{DispatchResult, Irp, PendingIrp},
};

/// The queue's heap block. `csq` is the first field so that the `PIO_CSQ`
/// the kernel passes to the callbacks is also a pointer to the block.
#[repr(C)]
struct Inner<S> {
    csq: UnsafeCell<MaybeUninit<IO_CSQ>>,
    lock: UnsafeCell<KSPIN_LOCK>,
    /// Head of the list of parked IRPs, linked through
    /// `Irp->Tail.Overlay.ListEntry`. Protected by `lock`.
    head: UnsafeCell<LIST_ENTRY>,
    /// Driver state. Protected by `lock`.
    state: UnsafeCell<S>,
}

/// A queue of pending IRPs plus a value `S`, both guarded by one spin lock.
///
/// A parked IRP that its issuer cancels is completed with
/// `STATUS_CANCELLED` by the queue itself.
pub struct IrpQueue<S> {
    // Boxed: the kernel keeps the IO_CSQ's address in every parked IRP, and
    // the list head is self-referential.
    inner: Box<Inner<S>>,
}

// SAFETY: everything inside is guarded by the spin lock; `S: Send` because
// the state is reached from whichever thread takes the lock.
unsafe impl<S: Send> Send for IrpQueue<S> {}
// SAFETY: as above.
unsafe impl<S: Send> Sync for IrpQueue<S> {}

struct LockGuard<'a, S> {
    inner: &'a Inner<S>,
    old_irql: KIRQL,
}

impl<S> Drop for LockGuard<'_, S> {
    fn drop(&mut self) {
        // SAFETY: the lock is held by this guard and `old_irql` is the IRQL
        // it was acquired at.
        unsafe { KeReleaseSpinLock(self.inner.lock.get(), self.old_irql) };
    }
}

impl<S> Inner<S> {
    fn lock(&self) -> LockGuard<'_, S> {
        // SAFETY: `lock` is a valid spin lock in non-paged memory.
        let old_irql = unsafe { KeAcquireSpinLockRaiseToDpc(self.lock.get()) };
        LockGuard { inner: self, old_irql }
    }

    fn csq(&self) -> PIO_CSQ {
        self.csq.get().cast::<IO_CSQ>()
    }
}

/// The insert decision travelling through `IoCsqInsertIrpEx`'s
/// `InsertContext` to [`insert_irp`].
struct InsertRequest<'a, S> {
    may_park: &'a mut dyn FnMut(&mut S) -> bool,
}

impl<S: Send> IrpQueue<S> {
    pub fn try_new(state: S) -> Result<Self, NtStatus> {
        let inner = try_box(Inner {
            csq: UnsafeCell::new(MaybeUninit::uninit()),
            lock: UnsafeCell::new(0),
            head: UnsafeCell::new(LIST_ENTRY {
                Flink: core::ptr::null_mut(),
                Blink: core::ptr::null_mut(),
            }),
            state: UnsafeCell::new(state),
        })?;
        // SAFETY: the block is at its final address. An empty list head
        // points at itself. IoCsqInitializeEx fills in the IO_CSQ with the
        // callbacks below, all of which match the prototypes it expects and
        // recover the block from the IO_CSQ pointer (its first field).
        let status = unsafe {
            let head = inner.head.get();
            (*head).Flink = head;
            (*head).Blink = head;
            IoCsqInitializeEx(
                inner.csq(),
                Some(insert_irp::<S>),
                Some(remove_irp),
                Some(peek_next_irp::<S>),
                Some(acquire_lock::<S>),
                Some(release_lock::<S>),
                Some(complete_canceled_irp),
            )
        };
        crate::check(status)?;
        Ok(Self { inner })
    }

    /// Runs `f` on the state under the queue's lock, at `DISPATCH_LEVEL`:
    /// `f` must be short, must not block and may touch only non-paged
    /// memory.
    pub fn with_state<R>(&self, f: impl FnOnce(&mut S) -> R) -> R {
        let _guard = self.inner.lock();
        // SAFETY: the lock is held, so this is the only reference to the
        // state.
        f(unsafe { &mut *self.inner.state.get() })
    }

    /// Parks `irp` if `may_park` agrees, deciding under the queue's lock.
    ///
    /// On `Ok` the request is pending (it may already have been completed
    /// as cancelled) and the dispatch routine returns the result. On `Err`
    /// the request was not parked and is handed back untouched.
    ///
    /// `may_park` runs under the lock, at `DISPATCH_LEVEL`.
    pub fn park_if(&self, irp: Irp, mut may_park: impl FnMut(&mut S) -> bool) -> Result<DispatchResult, Irp> {
        let raw = irp.into_raw();
        let mut request = InsertRequest::<S> {
            may_park: &mut may_park,
        };
        // SAFETY: the queue is initialised; `raw` is an IRP this driver
        // owns. `request` outlives the call and is only used by
        // `insert_irp::<S>` during it. If the insert callback refuses, the
        // routine leaves the IRP untouched and returns its status.
        let status = unsafe {
            IoCsqInsertIrpEx(
                self.inner.csq(),
                raw,
                core::ptr::null_mut(),
                core::ptr::from_mut(&mut request).cast(),
            )
        };
        if NtStatus(status).is_success() {
            // Parked and marked pending by the queue. The IRP must not be
            // touched again here: a cancellation may already have
            // completed it.
            Ok(DispatchResult(NtStatus::PENDING.0))
        } else {
            // SAFETY: the queue did not take the IRP, so ownership is still
            // ours, on the same thread it arrived on.
            Err(unsafe { Irp::from_raw(raw) })
        }
    }

    /// Removes the oldest parked IRP.
    pub fn pop(&self) -> Option<PendingIrp> {
        // SAFETY: the queue is initialised.
        let raw = unsafe { IoCsqRemoveNextIrp(self.inner.csq(), core::ptr::null_mut()) };
        NonNull::new(raw).map(|raw| {
            // SAFETY: a non-null result is an IRP that has been removed from
            // the queue with its cancel routine cleared, so this driver owns
            // it again; it was marked pending when it was parked.
            unsafe { PendingIrp::from_raw(raw) }
        })
    }
}

impl<S> Drop for IrpQueue<S> {
    fn drop(&mut self) {
        // Nothing may stay parked in a queue that is going away.
        loop {
            // SAFETY: as in `pop`.
            let raw = unsafe { IoCsqRemoveNextIrp(self.inner.csq(), core::ptr::null_mut()) };
            match NonNull::new(raw) {
                // SAFETY: as in `pop`.
                Some(raw) => unsafe { PendingIrp::from_raw(raw) }.complete(NtStatus::CANCELLED),
                None => break,
            }
        }
    }
}

/// # Safety
///
/// `csq` must be the `IO_CSQ` at the start of a live `Inner<S>`.
unsafe fn inner_of<'a, S>(csq: PIO_CSQ) -> &'a Inner<S> {
    // SAFETY: `csq` is the first field of a `#[repr(C)]` `Inner<S>`, so the
    // pointers coincide; the block outlives every callback because the
    // queue is drained before it is freed.
    unsafe { &*csq.cast::<Inner<S>>() }
}

/// The list entry inside an IRP.
///
/// # Safety
///
/// `irp` must be a valid IRP.
unsafe fn list_entry(irp: PIRP) -> *mut LIST_ENTRY {
    // SAFETY: valid IRP; `Tail.Overlay.ListEntry` is the field drivers use
    // to queue an IRP they own.
    unsafe { addr_of_mut!((*irp).Tail.Overlay.__bindgen_anon_2.ListEntry) }
}

/// The IRP containing a list entry.
///
/// # Safety
///
/// `entry` must be the `Tail.Overlay.ListEntry` of an IRP.
unsafe fn irp_of(entry: *mut LIST_ENTRY) -> PIRP {
    // SAFETY: the offset is computed from the same field path as
    // `list_entry`, on a zeroed IRP used only for address arithmetic.
    unsafe {
        let probe = MaybeUninit::<IRP>::zeroed();
        let base = probe.as_ptr();
        let field = core::ptr::addr_of!((*base).Tail.Overlay.__bindgen_anon_2.ListEntry);
        let offset = field.cast::<u8>().offset_from(base.cast::<u8>());
        entry.cast::<u8>().offset(-offset).cast::<IRP>()
    }
}

unsafe extern "C" fn insert_irp<S>(csq: PIO_CSQ, irp: PIRP, insert_context: *mut c_void) -> NTSTATUS {
    // SAFETY: called by IoCsqInsertIrpEx with the queue lock held, the
    // queue's own IO_CSQ, and the InsertRequest that `park_if` passed.
    unsafe {
        let inner = inner_of::<S>(csq);
        let request = &mut *insert_context.cast::<InsertRequest<'_, S>>();
        if !(request.may_park)(&mut *inner.state.get()) {
            return NtStatus::UNSUCCESSFUL.0;
        }
        // InsertTailList
        let head = inner.head.get();
        let entry = list_entry(irp);
        let last = (*head).Blink;
        (*entry).Flink = head;
        (*entry).Blink = last;
        (*last).Flink = entry;
        (*head).Blink = entry;
    }
    NtStatus::SUCCESS.0
}

unsafe extern "C" fn remove_irp(_csq: PIO_CSQ, irp: PIRP) {
    // SAFETY: called with the queue lock held for an IRP that is in the
    // list. This is RemoveEntryList.
    unsafe {
        let entry = list_entry(irp);
        let next = (*entry).Flink;
        let previous = (*entry).Blink;
        (*previous).Flink = next;
        (*next).Blink = previous;
    }
}

unsafe extern "C" fn peek_next_irp<S>(csq: PIO_CSQ, irp: PIRP, _peek_context: *mut c_void) -> PIRP {
    // SAFETY: called with the queue lock held; `irp` is null or an IRP in
    // the list. Returns the entry after it (or the first), null at the end.
    unsafe {
        let inner = inner_of::<S>(csq);
        let head = inner.head.get();
        let next = if irp.is_null() {
            (*head).Flink
        } else {
            (*list_entry(irp)).Flink
        };
        if next == head {
            core::ptr::null_mut()
        } else {
            irp_of(next)
        }
    }
}

unsafe extern "C" fn acquire_lock<S>(csq: PIO_CSQ, irql: PKIRQL) {
    // SAFETY: called by the framework with the queue's IO_CSQ and a valid
    // place to store the previous IRQL.
    unsafe {
        let inner = inner_of::<S>(csq);
        *irql = KeAcquireSpinLockRaiseToDpc(inner.lock.get());
    }
}

unsafe extern "C" fn release_lock<S>(csq: PIO_CSQ, irql: KIRQL) {
    // SAFETY: called by the framework to undo `acquire_lock` with the IRQL
    // that call stored.
    unsafe {
        let inner = inner_of::<S>(csq);
        KeReleaseSpinLock(inner.lock.get(), irql);
    }
}

unsafe extern "C" fn complete_canceled_irp(_csq: PIO_CSQ, irp: PIRP) {
    // SAFETY: the framework hands over a cancelled IRP that it has removed
    // from the queue; completing it is this callback's job.
    unsafe {
        (*irp).IoStatus.__bindgen_anon_1.Status = NtStatus::CANCELLED.0;
        (*irp).IoStatus.Information = 0;
        IofCompleteRequest(irp, IO_NO_INCREMENT as i8);
    }
}
