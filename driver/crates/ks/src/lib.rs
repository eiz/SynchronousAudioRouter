// Copyright (c) 2026 Itty Bitty Binaries, Inc.
// SPDX-License-Identifier: MIT

//! Safe wrappers over kernel streaming (AVStream), the framework that owns
//! this driver's device object and presents its filters to Windows.
//!
//! This crate is the FFI boundary for `ks-sys`. A driver describes its
//! device with [`device::DeviceHandler`], its filters with plain data
//! ([`descriptor::FilterSpec`]) and its behavior with
//! [`filter::FilterHandler`]; the crate builds the descriptor structures
//! AVStream wants, installs C-ABI trampolines in them, and turns the raw
//! pointers AVStream passes to those trampolines back into safe references.
//!
//! # Context pointers
//!
//! AVStream objects carry an untyped `Context` pointer, and a new object
//! inherits its parent's (a filter factory starts with the device's, a
//! filter with its factory's, a pin with its filter's). Every pointer this
//! crate stores there addresses a `#[repr(C)]` block that begins with a
//! [`context::Header`] naming what it is, and every trampoline checks that
//! header before using the block. A driver cannot store anything else: the
//! wrappers do not expose the `Context` fields.
//!
//! As in `ntk`, the safe API assumes it is called at `PASSIVE_LEVEL`, which
//! is where AVStream makes all the callbacks used here.
#![no_std]
#![deny(unsafe_op_in_unsafe_fn)]
#![deny(clippy::undocumented_unsafe_blocks)]

extern crate alloc;

mod context;
pub mod descriptor;
pub mod device;
pub mod filter;

pub use ks_sys::guids;
pub use ntstatus::{IoStatus, NtStatus};

/// Property and event identifiers, pin flags and other integer constants
/// from `ks.h` and `ksmedia.h`, with the Rust types the safe API uses.
pub mod ids {
    use ks_sys as sys;

    pub const KSPROPERTY_PIN_GLOBALCINSTANCES: u32 = sys::KSPROPERTY_PIN::KSPROPERTY_PIN_GLOBALCINSTANCES as u32;
    pub const KSPROPERTY_PIN_PHYSICALCONNECTION: u32 = sys::KSPROPERTY_PIN::KSPROPERTY_PIN_PHYSICALCONNECTION as u32;
    pub const KSPROPERTY_PIN_NAME: u32 = sys::KSPROPERTY_PIN::KSPROPERTY_PIN_NAME as u32;
    pub const KSPROPERTY_PIN_PROPOSEDATAFORMAT: u32 = sys::KSPROPERTY_PIN::KSPROPERTY_PIN_PROPOSEDATAFORMAT as u32;

    pub const KSPROPERTY_AUDIO_CHANNEL_CONFIG: u32 = sys::KSPROPERTY_AUDIO::KSPROPERTY_AUDIO_CHANNEL_CONFIG as u32;

    pub const KSPROPERTY_RTAUDIO_BUFFER: u32 = sys::KSPROPERTY_RTAUDIO::KSPROPERTY_RTAUDIO_BUFFER as u32;
    pub const KSPROPERTY_RTAUDIO_HWLATENCY: u32 = sys::KSPROPERTY_RTAUDIO::KSPROPERTY_RTAUDIO_HWLATENCY as u32;
    pub const KSPROPERTY_RTAUDIO_POSITIONREGISTER: u32 = sys::KSPROPERTY_RTAUDIO::KSPROPERTY_RTAUDIO_POSITIONREGISTER as u32;
    pub const KSPROPERTY_RTAUDIO_CLOCKREGISTER: u32 = sys::KSPROPERTY_RTAUDIO::KSPROPERTY_RTAUDIO_CLOCKREGISTER as u32;
    pub const KSPROPERTY_RTAUDIO_BUFFER_WITH_NOTIFICATION: u32 = sys::KSPROPERTY_RTAUDIO::KSPROPERTY_RTAUDIO_BUFFER_WITH_NOTIFICATION as u32;
    pub const KSPROPERTY_RTAUDIO_REGISTER_NOTIFICATION_EVENT: u32 = sys::KSPROPERTY_RTAUDIO::KSPROPERTY_RTAUDIO_REGISTER_NOTIFICATION_EVENT as u32;
    pub const KSPROPERTY_RTAUDIO_UNREGISTER_NOTIFICATION_EVENT: u32 = sys::KSPROPERTY_RTAUDIO::KSPROPERTY_RTAUDIO_UNREGISTER_NOTIFICATION_EVENT as u32;
    pub const KSPROPERTY_RTAUDIO_QUERY_NOTIFICATION_SUPPORT: u32 = sys::KSPROPERTY_RTAUDIO::KSPROPERTY_RTAUDIO_QUERY_NOTIFICATION_SUPPORT as u32;
    pub const KSPROPERTY_RTAUDIO_PACKETCOUNT: u32 = sys::KSPROPERTY_RTAUDIO::KSPROPERTY_RTAUDIO_PACKETCOUNT as u32;
    pub const KSPROPERTY_RTAUDIO_PRESENTATION_POSITION: u32 = sys::KSPROPERTY_RTAUDIO::KSPROPERTY_RTAUDIO_PRESENTATION_POSITION as u32;

    pub const KSEVENT_PINCAPS_FORMATCHANGE: u32 = sys::KSEVENT_PINCAPS_CHANGENOTIFICATIONS::KSEVENT_PINCAPS_FORMATCHANGE as u32;

    pub const KSINTERFACE_STANDARD_LOOPED_STREAMING: u32 = sys::KSINTERFACE_STANDARD::KSINTERFACE_STANDARD_LOOPED_STREAMING as u32;

    pub const KSPIN_FLAG_DO_NOT_INITIATE_PROCESSING: u32 = sys::KSPIN_FLAG_DO_NOT_INITIATE_PROCESSING;
    pub const KSPIN_FLAG_FRAMES_NOT_REQUIRED_FOR_PROCESSING: u32 = sys::KSPIN_FLAG_FRAMES_NOT_REQUIRED_FOR_PROCESSING;
    pub const KSPIN_FLAG_PROCESS_IF_ANY_IN_RUN_STATE: u32 = sys::KSPIN_FLAG_PROCESS_IF_ANY_IN_RUN_STATE;
    pub const KSPIN_FLAG_FIXED_FORMAT: u32 = sys::KSPIN_FLAG_FIXED_FORMAT;
    pub const KSPIN_FLAG_DO_NOT_USE_STANDARD_TRANSPORT: u32 = sys::KSPIN_FLAG_DO_NOT_USE_STANDARD_TRANSPORT;
    pub const KSPIN_FLAG_RENDERER: u32 = sys::KSPIN_FLAG_RENDERER_VALUE;

    /// The filter itself as the end of a topology connection.
    pub const KSFILTER_NODE: u32 = sys::KSFILTER_NODE;

    /// Sizes of the request and value structures of the properties, as the
    /// headers define them; used as the minimum sizes kernel streaming
    /// enforces.
    pub mod size {
        use core::mem::size_of;

        use ks_sys as sys;

        pub const KSPROPERTY: u32 = size_of::<sys::KSPROPERTY>() as u32;
        pub const KSP_PIN: u32 = size_of::<sys::KSP_PIN>() as u32;
        pub const KSNODEPROPERTY: u32 = size_of::<sys::KSNODEPROPERTY>() as u32;
        pub const KSPIN_CINSTANCES: u32 = size_of::<sys::KSPIN_CINSTANCES>() as u32;
        pub const KSAUDIO_CHANNEL_CONFIG: u32 = size_of::<sys::KSAUDIO_CHANNEL_CONFIG>() as u32;
        pub const KSRTAUDIO_BUFFER_PROPERTY: u32 = size_of::<sys::KSRTAUDIO_BUFFER_PROPERTY>() as u32;
        pub const KSRTAUDIO_BUFFER_PROPERTY_WITH_NOTIFICATION: u32 =
            size_of::<sys::KSRTAUDIO_BUFFER_PROPERTY_WITH_NOTIFICATION>() as u32;
        pub const KSRTAUDIO_BUFFER: u32 = size_of::<sys::KSRTAUDIO_BUFFER>() as u32;
        pub const KSRTAUDIO_HWREGISTER_PROPERTY: u32 = size_of::<sys::KSRTAUDIO_HWREGISTER_PROPERTY>() as u32;
        pub const KSRTAUDIO_HWREGISTER: u32 = size_of::<sys::KSRTAUDIO_HWREGISTER>() as u32;
        pub const KSRTAUDIO_HWLATENCY: u32 = size_of::<sys::KSRTAUDIO_HWLATENCY>() as u32;
        pub const KSRTAUDIO_NOTIFICATION_EVENT_PROPERTY: u32 =
            size_of::<sys::KSRTAUDIO_NOTIFICATION_EVENT_PROPERTY>() as u32;
        pub const KSAUDIO_PRESENTATION_POSITION: u32 = size_of::<sys::KSAUDIO_PRESENTATION_POSITION>() as u32;
        pub const KSEVENTDATA: u32 = size_of::<sys::KSEVENTDATA>() as u32;
        pub const BOOL: u32 = size_of::<sys::BOOL>() as u32;
        pub const ULONG: u32 = 4;
    }
}
