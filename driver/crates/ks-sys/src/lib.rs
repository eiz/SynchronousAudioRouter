// Copyright (c) 2026 Itty Bitty Binaries, Inc.
// SPDX-License-Identifier: MIT

//! Raw bindings to kernel streaming (AVStream), the part of the WDK that
//! Microsoft's `wdk-sys` does not cover.
//!
//! Everything here is `unsafe` to use and mirrors `ks.h` / `ksmedia.h`
//! one to one. The safe layer is the `ks` crate.
#![no_std]
#![allow(non_upper_case_globals, non_camel_case_types, non_snake_case)]
#![allow(clippy::all)]

pub use wdk_sys::GUID;

#[allow(missing_docs, unused_imports, unsafe_op_in_unsafe_fn, dead_code)]
mod bindings {
    use wdk_sys::*;
    include!(concat!(env!("OUT_DIR"), "/ks.rs"));
}
pub use bindings::*;

/// GUID constants, read from the headers' `DEFINE_GUIDSTRUCT` lines.
pub mod guids {
    use wdk_sys::GUID;
    include!(concat!(env!("OUT_DIR"), "/guids.rs"));
}

/// `KSFILTER_DESCRIPTOR_VERSION` is `((ULONG)-1)` in ks.h, which bindgen
/// cannot evaluate.
pub const KSFILTER_DESCRIPTOR_VERSION: u32 = u32::MAX;

/// `KSFILTER_NODE` is `((ULONG)-1)` in ks.h: the filter itself as an endpoint
/// of a topology connection.
pub const KSFILTER_NODE: u32 = u32::MAX;

/// `KSPIN_FLAG_RENDERER` is defined in ks.h as the OR of two other flags.
pub const KSPIN_FLAG_RENDERER_VALUE: u32 =
    KSPIN_FLAG_PROCESS_IN_RUN_STATE_ONLY | KSPIN_FLAG_GENERATE_EOS_EVENTS;

/// `KsDeleteFilterFactory` is a macro in ks.h:
///
/// ```c
/// #define KsDeleteFilterFactory(FilterFactory) \
///     KsFreeObjectCreateItemsByContext( \
///         *(KSDEVICE_HEADER *)( \
///             KsFilterFactoryGetParentDevice(FilterFactory)->FunctionalDeviceObject-> \
///                 DeviceExtension), \
///         FilterFactory)
/// ```
///
/// where `KsFilterFactoryGetParentDevice` is an inline wrapper of `KsGetParent`.
///
/// # Safety
///
/// `filter_factory` must be a live filter factory created by
/// `KsCreateFilterFactory`, and the caller must hold the device mutex, as for
/// the C macro.
pub unsafe fn KsDeleteFilterFactory(filter_factory: PKSFILTERFACTORY) -> wdk_sys::NTSTATUS {
    // SAFETY: the caller guarantees a live factory; its parent is the KSDEVICE,
    // whose functional device object's extension starts with the KS device
    // header pointer (this is what the ks.h macro relies on).
    unsafe {
        let device = KsGetParent(filter_factory.cast()) as PKSDEVICE;
        let fdo = (*device).FunctionalDeviceObject;
        let header = *((*fdo).DeviceExtension as *mut KSDEVICE_HEADER);
        KsFreeObjectCreateItemsByContext(header, filter_factory.cast())
    }
}
