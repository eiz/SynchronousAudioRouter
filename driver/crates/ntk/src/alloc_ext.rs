// Copyright (c) 2026 Itty Bitty Binaries, Inc.
// SPDX-License-Identifier: MIT

//! Fallible allocation.
//!
//! The standard `Box::new`, `Arc::new` and `Vec::push` abort when memory
//! runs out, which in a driver means a bugcheck. Kernel code has to report
//! `STATUS_INSUFFICIENT_RESOURCES` instead, so everything long-lived is
//! allocated through the functions here. All of them may be used up to
//! `DISPATCH_LEVEL` (the pool is non-paged).

use alloc::{alloc::Layout, boxed::Box, vec::Vec};
use core::{
    ffi::c_void,
    marker::PhantomData,
    ops::Deref,
    ptr::NonNull,
    sync::atomic::{AtomicUsize, Ordering},
};

use ntstatus::NtStatus;

/// `Box::new` that reports allocation failure.
pub fn try_box<T>(value: T) -> Result<Box<T>, NtStatus> {
    let layout = Layout::new::<T>();
    if layout.size() == 0 {
        // Boxing a zero-sized value does not allocate.
        return Ok(Box::new(value));
    }
    // SAFETY: the layout has a non-zero size.
    let raw = unsafe { alloc::alloc::alloc(layout) }.cast::<T>();
    if raw.is_null() {
        return Err(NtStatus::INSUFFICIENT_RESOURCES);
    }
    // SAFETY: `raw` is a fresh allocation of `T`'s layout from the global
    // allocator: valid for a write of `T`, and afterwards a valid `Box<T>`.
    unsafe {
        raw.write(value);
        Ok(Box::from_raw(raw))
    }
}

/// Copies a slice into a new vector, reporting allocation failure.
pub fn try_vec_from_slice<T: Copy>(items: &[T]) -> Result<Vec<T>, NtStatus> {
    let mut v = Vec::new();
    v.try_reserve_exact(items.len())
        .map_err(|_| NtStatus::INSUFFICIENT_RESOURCES)?;
    v.extend_from_slice(items);
    Ok(v)
}

/// A vector of `len` zero bytes, reporting allocation failure.
pub fn try_zeroed_bytes(len: usize) -> Result<Vec<u8>, NtStatus> {
    let mut v = Vec::new();
    v.try_reserve_exact(len)
        .map_err(|_| NtStatus::INSUFFICIENT_RESOURCES)?;
    v.resize(len, 0);
    Ok(v)
}

#[repr(C)]
struct Inner<T> {
    strong: AtomicUsize,
    value: T,
}

/// A thread-safe reference-counted pointer, like `alloc::sync::Arc`, with a
/// fallible constructor and conversion to and from the untyped context
/// pointers that kernel callbacks carry.
pub struct Shared<T> {
    inner: NonNull<Inner<T>>,
    _owns: PhantomData<Inner<T>>,
}

// SAFETY: a `Shared<T>` hands out `&T` to every holder and drops `T` on
// whichever thread releases the last reference, which is exactly what
// `T: Send + Sync` permits (the same bounds as `Arc`).
unsafe impl<T: Send + Sync> Send for Shared<T> {}
// SAFETY: as above.
unsafe impl<T: Send + Sync> Sync for Shared<T> {}

impl<T> Shared<T> {
    /// Allocates a new shared value with one reference.
    pub fn try_new(value: T) -> Result<Self, NtStatus> {
        let boxed = try_box(Inner {
            strong: AtomicUsize::new(1),
            value,
        })?;
        Ok(Self {
            inner: NonNull::from(Box::leak(boxed)),
            _owns: PhantomData,
        })
    }

    fn inner(&self) -> &Inner<T> {
        // SAFETY: `self` holds a reference, so the allocation is live.
        unsafe { self.inner.as_ref() }
    }

    /// Whether two pointers refer to the same allocation.
    pub fn ptr_eq(a: &Self, b: &Self) -> bool {
        a.inner == b.inner
    }

    /// Gives up this reference as an untyped pointer for a kernel context
    /// slot. The pointer is the address of the value itself (so a
    /// `#[repr(C)]` value can be inspected through it). The reference stays
    /// counted until [`Shared::from_context`] takes it back.
    pub fn into_context(self) -> *mut c_void {
        // SAFETY: the allocation is live; this only computes the address of
        // the `value` field.
        let raw = unsafe { core::ptr::addr_of_mut!((*self.inner.as_ptr()).value) };
        core::mem::forget(self);
        raw.cast()
    }

    /// The allocation behind a context pointer.
    ///
    /// # Safety
    ///
    /// `context` must come from `Shared::<T>::into_context` for this `T`.
    unsafe fn inner_of_context(context: *mut c_void) -> NonNull<Inner<T>> {
        // SAFETY: `context` is the address of the `value` field of a live
        // `Inner<T>`; stepping back by the field's offset gives the
        // allocation, which is non-null.
        unsafe {
            NonNull::new_unchecked(
                context
                    .cast::<u8>()
                    .sub(core::mem::offset_of!(Inner<T>, value))
                    .cast::<Inner<T>>(),
            )
        }
    }

    /// Takes back a reference given up with [`Shared::into_context`].
    ///
    /// # Safety
    ///
    /// `context` must come from `Shared::<T>::into_context` for this same
    /// `T`, and each such pointer may be taken back only once.
    pub unsafe fn from_context(context: *mut c_void) -> Self {
        Self {
            // SAFETY: forwarded from the caller's contract.
            inner: unsafe { Self::inner_of_context(context) },
            _owns: PhantomData,
        }
    }

    /// A new reference to the value behind a context pointer, leaving the
    /// context's own reference in place.
    ///
    /// # Safety
    ///
    /// `context` must come from `Shared::<T>::into_context` for this same
    /// `T`, and that reference must not have been taken back yet (and must
    /// not be taken back concurrently with this call).
    pub unsafe fn clone_from_context(context: *mut c_void) -> Self {
        // SAFETY: the context's reference keeps the allocation alive for the
        // duration of this call, by the caller's contract.
        let borrowed = core::mem::ManuallyDrop::new(unsafe { Self::from_context(context) });
        Self::clone(&borrowed)
    }

    /// Borrows the value behind a context pointer.
    ///
    /// # Safety
    ///
    /// As for [`Shared::clone_from_context`], and the context's reference
    /// must stay in place for the lifetime `'a` the caller picks.
    pub unsafe fn borrow_context<'a>(context: *mut c_void) -> &'a T {
        // SAFETY: the context pointer is the address of the value, which
        // the caller guarantees is live for `'a`.
        unsafe { &*context.cast::<T>() }
    }
}

impl<T> Clone for Shared<T> {
    fn clone(&self) -> Self {
        let previous = self.inner().strong.fetch_add(1, Ordering::Relaxed);
        // A count this large can only come from leaking references in a
        // loop; stop before it can wrap and free a live value.
        assert!(previous < usize::MAX / 2, "reference count overflow");
        Self {
            inner: self.inner,
            _owns: PhantomData,
        }
    }
}

impl<T> Deref for Shared<T> {
    type Target = T;

    fn deref(&self) -> &T {
        &self.inner().value
    }
}

impl<T> Drop for Shared<T> {
    fn drop(&mut self) {
        if self.inner().strong.fetch_sub(1, Ordering::Release) != 1 {
            return;
        }
        core::sync::atomic::fence(Ordering::Acquire);
        // SAFETY: the count reached zero, so this was the last reference;
        // the allocation came from `try_box` (a `Box<Inner<T>>`).
        drop(unsafe { Box::from_raw(self.inner.as_ptr()) });
    }
}
