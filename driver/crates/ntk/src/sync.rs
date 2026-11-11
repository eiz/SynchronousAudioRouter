// Copyright (c) 2026 Itty Bitty Binaries, Inc.
// SPDX-License-Identifier: MIT

//! Locks.

use alloc::boxed::Box;
use core::{
    cell::UnsafeCell,
    mem::MaybeUninit,
    ops::{Deref, DerefMut},
    sync::atomic::{AtomicBool, Ordering},
};

use ntstatus::NtStatus;
use wdk_sys::{
    ntddk::{
        KeAcquireSpinLockRaiseToDpc, KeInitializeMutex, KeReleaseMutex, KeReleaseSpinLock,
        KeWaitForSingleObject,
    },
    KIRQL, KMUTEX, KSPIN_LOCK,
};

use crate::alloc_ext::{try_box, Shared};

/// A mutual-exclusion lock for use at `PASSIVE_LEVEL`, built on a kernel
/// mutex (`KMUTEX`). The holder stays at `PASSIVE_LEVEL` and may touch
/// pageable memory and call `Zw` routines.
///
/// A kernel mutex is recursive; this type is not. Locking it again on the
/// thread that already holds it panics, because handing out a second
/// `&mut T` would be unsound.
pub struct Mutex<T> {
    // The kernel object lives in its own allocation: a KMUTEX contains
    // list links and must never move, while a `Mutex<T>` may.
    raw: Box<UnsafeCell<MaybeUninit<KMUTEX>>>,
    held: AtomicBool,
    value: UnsafeCell<T>,
}

// SAFETY: the lock serialises all access to `value`; the kernel object may
// be used from any thread.
unsafe impl<T: Send> Send for Mutex<T> {}
// SAFETY: as above.
unsafe impl<T: Send> Sync for Mutex<T> {}

impl<T> Mutex<T> {
    pub fn try_new(value: T) -> Result<Self, NtStatus> {
        let raw = try_box(UnsafeCell::new(MaybeUninit::<KMUTEX>::uninit()))?;
        // SAFETY: `raw` is writable storage for a KMUTEX at its final
        // address; KeInitializeMutex initialises it.
        unsafe { KeInitializeMutex(raw.get().cast::<KMUTEX>(), 0) };
        Ok(Self {
            raw,
            held: AtomicBool::new(false),
            value: UnsafeCell::new(value),
        })
    }

    fn raw(&self) -> *mut KMUTEX {
        self.raw.get().cast::<KMUTEX>()
    }

    /// Waits for the lock. Must be called below `DISPATCH_LEVEL`.
    pub fn lock(&self) -> MutexGuard<'_, T> {
        // SAFETY: `raw` is an initialised KMUTEX that outlives the call. A
        // non-alertable kernel-mode wait without timeout returns only once
        // the mutex is owned (STATUS_SUCCESS, or STATUS_ABANDONED which
        // also grants ownership).
        let _ = unsafe {
            KeWaitForSingleObject(
                self.raw().cast(),
                wdk_sys::_KWAIT_REASON::Executive,
                wdk_sys::_MODE::KernelMode as i8,
                0,
                core::ptr::null_mut(),
            )
        };
        if self.held.swap(true, Ordering::Acquire) {
            // The kernel granted the mutex although it is held: this thread
            // already owns it. A second guard must not exist.
            panic!("ntk::sync::Mutex locked recursively");
        }
        MutexGuard { mutex: self }
    }
}

pub struct MutexGuard<'a, T> {
    mutex: &'a Mutex<T>,
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
        self.mutex.held.store(false, Ordering::Release);
        // SAFETY: this thread owns the mutex (the guard exists).
        unsafe { KeReleaseMutex(self.mutex.raw(), 0) };
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
        SpinLockGuard { lock: self, old_irql }
    }
}

pub struct SpinLockGuard<'a, T> {
    lock: &'a SpinLock<T>,
    old_irql: KIRQL,
}

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
