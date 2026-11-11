// Copyright (c) 2026 Itty Bitty Binaries, Inc.
// SPDX-License-Identifier: MIT

//! Kernel-independent logic of the SynchronousAudioRouter driver.
//!
//! Everything in this crate is plain computation over integers and byte
//! slices: the wire formats of appendix A of the specification, parameter
//! validation, section geometry, the cell allocator, the register update
//! rule, format matching, and the notification queue's state machine. It has
//! no kernel dependency and no `unsafe`, and is unit-tested on the build
//! host with `cargo test`.
//!
//! Section references (for example "A.5") are to
//! `docs/behavior.md`.
#![cfg_attr(not(test), no_std)]
#![forbid(unsafe_code)]

extern crate alloc;

pub mod buffer;
pub mod cells;
pub mod control;
pub mod endpoint;
pub mod format;
pub mod layout;
pub mod notify;
pub mod properties;
pub mod registers;
pub mod registry;
pub mod wire;

pub use ntstatus::{IoStatus, NtStatus};
