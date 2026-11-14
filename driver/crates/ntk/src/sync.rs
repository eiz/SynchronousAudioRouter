// Copyright (c) 2026 Itty Bitty Binaries, Inc.
// SPDX-License-Identifier: MIT

//! Locks.

use alloc::boxed::Box;
use core::{
    cell::UnsafeCell,
    marker::PhantomData,
    mem::MaybeUninit,
    ops::{Deref, DerefMut},
    sync::atomic::{AtomicBool, AtomicUsize, Ordering},
};

use ntstatus::NtStatus;
use wdk_sys::{
    ntddk::{
        ExAcquireRundownProtection, ExReInitializeRundownProtection, ExReleaseRundownProtection,
        ExAcquireFastMutex, ExReleaseFastMutex, ExRundownCompleted,
        ExWaitForRundownProtectionRelease, KeAcquireSpinLockRaiseToDpc, PsGetCurrentThreadId,
        KeEnterCriticalRegion, KeInitializeEvent, KeLeaveCriticalRegion,
        KeReleaseSpinLock, KeSetEvent, KeWaitForSingleObject,
    },
    EX_RUNDOWN_REF, KEVENT, KIRQL, KSPIN_LOCK,
    _EX_RUNDOWN_REF__bindgen_ty_1 as EX_RUNDOWN_REF_VALUE,
};

use crate::alloc_ext::{try_box, Shared};

/// A mutual-exclusion lock built on a fast mutex (`FAST_MUTEX`). It is
/// taken at or below `APC_LEVEL`, and the holder runs at `APC_LEVEL` until
/// it lets go.
///
/// That is the point of the kind. At `APC_LEVEL` a thread may read and
/// write memory, pageable memory included, and allocate; it may not make
/// the calls that have to wait for another part of the system (a registry
/// call, a handle, a view of a section, a device interface, anything that
/// waits for its own I/O to complete), and Driver Verifier stops the
/// machine when one is made. So what may be done under one of these locks
/// is what the driver is supposed to do under a lock anyway (section 14 of
/// the specification), and a violation shows as a stop, not as a slow
/// request.
///
/// A fast mutex cannot be taken twice by one thread: the second attempt
/// would wait for ever. This type refuses instead. Locking it again on the
/// thread that already holds it panics.
pub struct Mutex<T> {
    // The kernel object lives in its own allocation: a FAST_MUTEX contains
    // an event, with list links, and must never move, while a `Mutex<T>`
    // may.
    raw: Box<UnsafeCell<MaybeUninit<wdk_sys::FAST_MUTEX>>>,
    /// The id of the thread that holds the lock, or zero.
    owner: AtomicUsize,
    value: UnsafeCell<T>,
}

// SAFETY: the lock serialises all access to `value`; the kernel object may
// be used from any thread.
unsafe impl<T: Send> Send for Mutex<T> {}
// SAFETY: as above.
unsafe impl<T: Send> Sync for Mutex<T> {}

impl<T> Mutex<T> {
    pub fn try_new(value: T) -> Result<Self, NtStatus> {
        let raw = try_box(UnsafeCell::new(MaybeUninit::<wdk_sys::FAST_MUTEX>::uninit()))?;
        // SAFETY: `raw` is writable storage for a FAST_MUTEX at its final
        // address. This is what wdm.h's inline ExInitializeFastMutex does:
        // Count = FM_LOCK_BIT (1), no owner, no contention, and a
        // synchronisation event that is not signalled.
        unsafe {
            let fast = raw.get().cast::<wdk_sys::FAST_MUTEX>();
            core::ptr::addr_of_mut!((*fast).Count).write(1);
            core::ptr::addr_of_mut!((*fast).Owner).write(core::ptr::null_mut());
            core::ptr::addr_of_mut!((*fast).Contention).write(0);
            core::ptr::addr_of_mut!((*fast).OldIrql).write(0);
            KeInitializeEvent(
                core::ptr::addr_of_mut!((*fast).Event),
                wdk_sys::_EVENT_TYPE::SynchronizationEvent,
                0,
            );
        }
        Ok(Self {
            raw,
            owner: AtomicUsize::new(0),
            value: UnsafeCell::new(value),
        })
    }

    fn raw(&self) -> *mut wdk_sys::FAST_MUTEX {
        self.raw.get().cast::<wdk_sys::FAST_MUTEX>()
    }

    /// Waits for the lock. Must be called at or below `APC_LEVEL`. The
    /// thread is at `APC_LEVEL` until the guard is dropped; guards of
    /// several locks must be dropped in the reverse of the order in which
    /// they were taken, since each restores the level it found.
    pub fn lock(&self) -> MutexGuard<'_, T> {
        // SAFETY: no pointer is involved; a thread id is never zero.
        let me = unsafe { PsGetCurrentThreadId() } as usize;
        if self.owner.load(Ordering::Acquire) == me {
            // Only this thread can have stored its own id, so it holds the
            // lock. Waiting would never end, and a second guard must not
            // exist.
            panic!("ntk::sync::Mutex locked recursively");
        }
        // SAFETY: `raw` is an initialised FAST_MUTEX in non-paged memory
        // that outlives the call; the caller is at or below APC_LEVEL and
        // does not hold it (checked above).
        unsafe { ExAcquireFastMutex(self.raw()) };
        self.owner.store(me, Ordering::Release);
        MutexGuard {
            mutex: self,
            _same_thread: PhantomData,
        }
    }
}

/// Proof of holding a [`Mutex`]. It stays on the thread that took the
/// lock, which is the one that has to release it.
pub struct MutexGuard<'a, T> {
    mutex: &'a Mutex<T>,
    _same_thread: PhantomData<*mut ()>,
}

impl<T> Deref for MutexGuard<'_, T> {
    type Target = T;

    fn deref(&self) -> &T {
        // SAFETY: the guard proves the lock is held by this thread.
        unsafe { &*self.mutex.value.get() }
    }
}

impl<T> DerefMut for MutexGuard<'_, T> {
    fn deref_mut(&mut self) -> &mut T {
        // SAFETY: the guard proves the lock is held, and `&mut self` that no
        // other borrow through this guard exists.
        unsafe { &mut *self.mutex.value.get() }
    }
}

impl<T> Drop for MutexGuard<'_, T> {
    fn drop(&mut self) {
        self.mutex.owner.store(0, Ordering::Release);
        // SAFETY: this thread owns the mutex (the guard exists), and is at
        // the APC_LEVEL the acquisition left it at.
        unsafe { ExReleaseFastMutex(self.mutex.raw()) };
    }
}

/// Something that happens once and can be waited for: a kernel notification
/// event that starts unset and stays set.
///
/// Waiting for one is not holding a lock: the waiter holds nothing, and
/// whoever will set the event is not kept from anything.
pub struct Completion {
    // Boxed for the same reason as a mutex: the kernel object must not move.
    raw: Box<UnsafeCell<MaybeUninit<KEVENT>>>,
}

// SAFETY: a kernel event may be set and waited on from any thread.
unsafe impl Send for Completion {}
// SAFETY: as above.
unsafe impl Sync for Completion {}

impl Completion {
    pub fn try_new() -> Result<Self, NtStatus> {
        let raw = try_box(UnsafeCell::new(MaybeUninit::<KEVENT>::uninit()))?;
        // SAFETY: `raw` is writable storage for a KEVENT at its final
        // address; KeInitializeEvent initialises it, not signalled.
        unsafe {
            KeInitializeEvent(
                raw.get().cast::<KEVENT>(),
                wdk_sys::_EVENT_TYPE::NotificationEvent,
                0,
            );
        }
        Ok(Self { raw })
    }

    /// Marks the thing as done and wakes everyone waiting. May be called
    /// more than once, at or below `DISPATCH_LEVEL`.
    pub fn set(&self) {
        // SAFETY: an initialised event that outlives the call; the caller
        // does not wait on it right after (Wait is false).
        unsafe {
            let _ = KeSetEvent(self.raw.get().cast::<KEVENT>(), 0, 0);
        }
    }

    /// Returns once [`set`](Self::set) has been called. Must be called at
    /// `PASSIVE_LEVEL` and, since it may wait, with no lock held.
    pub fn wait(&self) {
        // SAFETY: an initialised event that outlives the call. A
        // non-alertable kernel-mode wait without timeout returns only when
        // the event is signalled.
        let _ = unsafe {
            KeWaitForSingleObject(
                self.raw.get().cast(),
                wdk_sys::_KWAIT_REASON::Executive,
                wdk_sys::_MODE::KernelMode as i8,
                0,
                core::ptr::null_mut(),
            )
        };
    }
}

/// Run-down protection (`EX_RUNDOWN_REF`): a gate that any number of threads
/// may be inside at once, and that can be shut in such a way that the one
/// shutting it knows when everybody has left.
///
/// It serialises nothing and nobody waits to get in: entering either
/// succeeds at once or, once the gate is being shut, fails at once.
pub struct Rundown {
    raw: UnsafeCell<EX_RUNDOWN_REF>,
    /// Whether the gate has been shut and not opened again.
    shut: AtomicBool,
}

// SAFETY: the structure is only ever handed to the run-down routines, which
// use interlocked operations on it.
unsafe impl Send for Rundown {}
// SAFETY: as above.
unsafe impl Sync for Rundown {}

impl Rundown {
    /// An open gate with nobody inside: the state
    /// `ExInitializeRundownProtection` produces, which is all zero. (Being a
    /// constant lets it be a `static`; the structure holds no pointer to
    /// itself and may be moved while nobody is inside.)
    pub const fn new() -> Self {
        Self {
            raw: UnsafeCell::new(EX_RUNDOWN_REF {
                __bindgen_anon_1: EX_RUNDOWN_REF_VALUE { Count: 0 },
            }),
            shut: AtomicBool::new(false),
        }
    }

    /// Enters, unless the gate has been shut. The gate cannot finish
    /// shutting until the result is dropped.
    ///
    /// Must be called at `PASSIVE_LEVEL`. Until the result is dropped the
    /// thread is in a critical region, so that it cannot be suspended while
    /// somebody waits for it to leave.
    pub fn enter(&self) -> Option<RundownGuard<'_>> {
        // SAFETY: entering a critical region is always allowed at
        // PASSIVE_LEVEL; it is left again below or in the guard's
        // destructor. `raw` is an initialised run-down structure in
        // non-paged memory.
        unsafe {
            KeEnterCriticalRegion();
            if ExAcquireRundownProtection(self.raw.get()) == 0 {
                KeLeaveCriticalRegion();
                return None;
            }
        }
        Some(RundownGuard {
            gate: self,
            _same_thread: PhantomData,
        })
    }

    /// Shuts the gate: nobody enters from now on, and the call returns when
    /// everybody who was inside has left. Shutting a gate that is shut does
    /// nothing.
    ///
    /// Must be called at or below `APC_LEVEL`, and never at the same time
    /// as another `shut` or [`open`](Self::open) of the same gate.
    pub fn shut(&self) {
        if self.shut.swap(true, Ordering::AcqRel) {
            return;
        }
        // SAFETY: an initialised run-down structure; the caller's contract
        // rules out a concurrent wait or re-initialisation.
        unsafe {
            ExWaitForRundownProtectionRelease(self.raw.get());
            ExRundownCompleted(self.raw.get());
        }
    }

    /// Opens a gate that was shut. Opening one that is open does nothing.
    ///
    /// Must be called at or below `APC_LEVEL`, and never at the same time
    /// as a [`shut`](Self::shut) or another `open` of the same gate.
    pub fn open(&self) {
        if !self.shut.swap(false, Ordering::AcqRel) {
            return;
        }
        // SAFETY: the structure was run down by `shut` (the flag was set),
        // which is what re-initialising it requires.
        unsafe { ExReInitializeRundownProtection(self.raw.get()) };
    }
}

impl Default for Rundown {
    fn default() -> Self {
        Self::new()
    }
}

/// Proof of being inside a [`Rundown`] gate. It stays on the thread that
/// entered: leaving the critical region is that thread's to do.
pub struct RundownGuard<'a> {
    gate: &'a Rundown,
    _same_thread: PhantomData<*mut ()>,
}

impl Drop for RundownGuard<'_> {
    fn drop(&mut self) {
        // SAFETY: releases the protection `enter` acquired and leaves the
        // critical region it entered, once each, on the same thread (the
        // guard is not `Send`).
        unsafe {
            ExReleaseRundownProtection(self.gate.raw.get());
            KeLeaveCriticalRegion();
        }
    }
}

/// A spin lock. Acquiring it raises the IRQL to `DISPATCH_LEVEL`, so the
/// code under it must be short, must not block, and must touch only
/// non-paged memory (anything allocated through this crate qualifies).
pub struct SpinLock<T> {
    raw: UnsafeCell<KSPIN_LOCK>,
    value: UnsafeCell<T>,
}

// SAFETY: the lock serialises all access to `value`.
unsafe impl<T: Send> Send for SpinLock<T> {}
// SAFETY: as above.
unsafe impl<T: Send> Sync for SpinLock<T> {}

impl<T> SpinLock<T> {
    /// A released lock. A `KSPIN_LOCK` is a machine word whose released
    /// state is zero (what `KeInitializeSpinLock` stores), so this can be a
    /// constant and the lock may be moved while it is not held.
    pub const fn new(value: T) -> Self {
        Self {
            raw: UnsafeCell::new(0),
            value: UnsafeCell::new(value),
        }
    }

    /// Acquires the lock. Must be called at or below `DISPATCH_LEVEL`.
    pub fn lock(&self) -> SpinLockGuard<'_, T> {
        // SAFETY: `raw` is a valid spin lock in non-paged memory.
        let old_irql = unsafe { KeAcquireSpinLockRaiseToDpc(self.raw.get()) };
        SpinLockGuard {
            lock: self,
            old_irql,
            _same_thread: PhantomData,
        }
    }
}

/// Proof of holding a [`SpinLock`]. It stays on the thread, and so on the
/// processor, that took the lock: releasing it restores that processor's
/// IRQL. It may be shared with another thread only if `T` may.
pub struct SpinLockGuard<'a, T> {
    lock: &'a SpinLock<T>,
    old_irql: KIRQL,
    _same_thread: PhantomData<*mut ()>,
}

// SAFETY: a shared guard hands out only `&T`, which other threads may hold
// at the same time exactly when `T: Sync`.
unsafe impl<T: Sync> Sync for SpinLockGuard<'_, T> {}

impl<T> Deref for SpinLockGuard<'_, T> {
    type Target = T;

    fn deref(&self) -> &T {
        // SAFETY: the guard proves the lock is held.
        unsafe { &*self.lock.value.get() }
    }
}

impl<T> DerefMut for SpinLockGuard<'_, T> {
    fn deref_mut(&mut self) -> &mut T {
        // SAFETY: the guard proves the lock is held exclusively.
        unsafe { &mut *self.lock.value.get() }
    }
}

impl<T> Drop for SpinLockGuard<'_, T> {
    fn drop(&mut self) {
        // SAFETY: the lock is held by this guard and `old_irql` is the IRQL
        // it was acquired at.
        unsafe { KeReleaseSpinLock(self.lock.raw.get(), self.old_irql) };
    }
}

/// A slot for driver-wide state: set once the driver has initialised,
/// emptied when it unloads, readable from anywhere in between.
pub struct Global<T> {
    slot: SpinLock<Option<Shared<T>>>,
}

impl<T: Send + Sync> Global<T> {
    pub const fn new() -> Self {
        Self {
            slot: SpinLock::new(None),
        }
    }

    /// Stores `value` and returns what was there before.
    pub fn set(&self, value: Shared<T>) -> Option<Shared<T>> {
        // The previous value is handed back rather than dropped here, so
        // that its destructor never runs under the spin lock.
        self.slot.lock().replace(value)
    }

    /// A new reference to the stored value.
    pub fn get(&self) -> Option<Shared<T>> {
        self.slot.lock().clone()
    }

    /// Empties the slot.
    pub fn take(&self) -> Option<Shared<T>> {
        self.slot.lock().take()
    }
}

impl<T: Send + Sync> Default for Global<T> {
    fn default() -> Self {
        Self::new()
    }
}
