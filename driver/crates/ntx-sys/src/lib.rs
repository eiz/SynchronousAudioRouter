// Copyright (c) 2026 Itty Bitty Binaries, Inc.
// SPDX-License-Identifier: MIT

//! Raw declarations the driver needs that Microsoft's `wdk-sys` does not
//! provide:
//!
//! * [`seh`]: the C shim that wraps user-memory access in `__try`/`__except`
//!   (see `shim/seh.c` for why that cannot be Rust);
//! * [`process`]: `ZwQueryInformationProcess`, which Microsoft documents but
//!   declares in no kernel header, and the buffer layout of its
//!   `ProcessMitigationPolicy` class.
#![no_std]
#![allow(non_snake_case, non_camel_case_types)]

pub mod seh {
    use core::ffi::c_void;

    use wdk_sys::NTSTATUS;

    extern "C" {
        /// Copies `length` bytes from the user-mode address `user_source` to
        /// `destination` after probing the range for user-space residence
        /// and `alignment`. Returns the exception code on failure.
        ///
        /// # Safety
        ///
        /// `destination` must be valid for writes of `length` bytes.
        /// `user_source` may be any value. IRQL must be below DISPATCH_LEVEL
        /// and the thread must be in the context of the process the address
        /// belongs to.
        pub fn SarShimProbeAndRead(
            destination: *mut c_void,
            user_source: *const c_void,
            length: usize,
            alignment: u32,
        ) -> NTSTATUS;

        /// Probes a user-mode range for write access and `alignment`.
        ///
        /// # Safety
        ///
        /// `user_destination` may be any value; IRQL and process context as
        /// for [`SarShimProbeAndRead`].
        pub fn SarShimProbeForWrite(
            user_destination: *mut c_void,
            length: usize,
            alignment: u32,
        ) -> NTSTATUS;

        /// Copies `length` bytes from `source` to the user-mode address
        /// `user_destination` after probing it.
        ///
        /// # Safety
        ///
        /// `source` must be valid for reads of `length` bytes.
        /// `user_destination` may be any value; IRQL and process context as
        /// for [`SarShimProbeAndRead`].
        pub fn SarShimProbeAndWrite(
            user_destination: *mut c_void,
            source: *const c_void,
            length: usize,
            alignment: u32,
        ) -> NTSTATUS;

        /// Copies `length` bytes to `destination`, catching a fault. No probe.
        ///
        /// # Safety
        ///
        /// `source` must be valid for reads of `length` bytes. `destination`
        /// must be either a user-mode address that the kernel has already
        /// probed, or a valid kernel address. IRQL below DISPATCH_LEVEL.
        pub fn SarShimGuardedWrite(
            destination: *mut c_void,
            source: *const c_void,
            length: usize,
        ) -> NTSTATUS;
    }
}

pub mod process {
    use wdk_sys::{HANDLE, NTSTATUS, PROCESSINFOCLASS, PROCESS_MITIGATION_POLICY, PULONG, PVOID, ULONG};

    extern "system" {
        /// Documented at
        /// <https://learn.microsoft.com/windows/win32/procthread/zwqueryinformationprocess>;
        /// exported by ntoskrnl.exe but declared in no WDK header, so the
        /// documentation's prototype is repeated here.
        pub fn ZwQueryInformationProcess(
            ProcessHandle: HANDLE,
            ProcessInformationClass: PROCESSINFOCLASS,
            ProcessInformation: PVOID,
            ProcessInformationLength: ULONG,
            ReturnLength: PULONG,
        ) -> NTSTATUS;
    }

    /// Buffer of the `ProcessMitigationPolicy` information class: the caller
    /// sets `Policy`, the system fills the policy's flags word. `Flags` has
    /// the layout of the matching `PROCESS_MITIGATION_*_POLICY` structure in
    /// ntddk.h (each is a single `ULONG` of bit fields).
    #[repr(C)]
    #[derive(Clone, Copy)]
    pub struct PROCESS_MITIGATION_POLICY_INFORMATION {
        pub Policy: PROCESS_MITIGATION_POLICY,
        pub Flags: ULONG,
    }
}
