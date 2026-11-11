// Copyright (c) 2026 Itty Bitty Binaries, Inc.
// SPDX-License-Identifier: MIT

//! SynchronousAudioRouter.sys
//!
//! A clean-room implementation of the SynchronousAudioRouter kernel driver,
//! written from `spec/driver-behavior.md`. Section references in comments
//! ("section 8", "A.5") are to that document.
//!
//! The driver is an AVStream minidriver. It lets a host program create
//! virtual audio endpoints at run time, gives the host and the endpoints'
//! clients shared memory to pass audio through, relays the clients'
//! notification events to the host, and can steer processes towards SAR's
//! replacement audio device enumerator. It never reads or changes audio
//! sample values: it has no mapping of the audio area at all.
//!
//! Layout of the crate:
//!
//! * [`entry`]: `DriverEntry`, the only module that contains `unsafe`.
//! * [`device`]: driver-wide and per-device state, and the hooks that route
//!   requests either to the control interface or to kernel streaming.
//! * [`control`]: the control interface (section 5).
//! * [`section`]: the shared section (section 6).
//! * [`endpoint`], [`filters`], [`publish`]: endpoints as Windows sees them
//!   (section 7) and streams (section 8).
//! * [`regfilter`]: application routing support (section 10).
//!
//! All of the logic that needs no kernel API lives in the `sar-core` crate.
#![no_std]
// `unsafe` is denied for the whole crate; `entry` alone opts out. Every
// other module additionally forbids it, which cannot be overridden.
#![deny(unsafe_code)]
#![deny(unsafe_op_in_unsafe_fn)]
// The library target carries the image's name, which is not snake case.
#![allow(non_snake_case)]

extern crate alloc;

mod control;
mod device;
mod endpoint;
mod entry;
mod filters;
mod publish;
mod regfilter;
mod section;
