// Copyright (c) 2026 Itty Bitty Binaries, Inc.
// SPDX-License-Identifier: MIT

//! Questions about the process the calling thread runs in.

use alloc::vec::Vec;
use core::ffi::c_void;

use ntstatus::NtStatus;
use ntx_sys::process::{ZwQueryInformationProcess, PROCESS_MITIGATION_POLICY_INFORMATION};
use wdk_sys::{
    ntddk::{
        ExFreePool, IoGetCurrentProcess, IoIs32bitProcess, PsDereferencePrimaryToken,
        PsReferencePrimaryToken, RtlLengthSid, SeQueryInformationToken,
    },
    HANDLE, TOKEN_USER,
};

use crate::alloc_ext::try_vec_from_slice;

/// The pseudo-handle for the current process (`NtCurrentProcess()`).
const CURRENT_PROCESS: HANDLE = usize::MAX as HANDLE;

/// A user's security identifier, copied out of a token. Two SIDs are equal
/// exactly when their bytes are (this is what `RtlEqualSid` compares).
#[derive(Clone, PartialEq, Eq)]
pub struct UserSid(Vec<u8>);

impl UserSid {
    /// The user the current process runs as (its primary token's user).
    pub fn of_current_process() -> Result<Self, NtStatus> {
        // SAFETY: the current process object is valid while one of its
        // threads runs; the token reference is released below.
        let token = unsafe { PsReferencePrimaryToken(IoGetCurrentProcess()) };
        if token.is_null() {
            return Err(NtStatus::UNSUCCESSFUL);
        }
        let mut information: *mut c_void = core::ptr::null_mut();
        // SAFETY: `token` is a referenced token; for TokenUser the routine
        // allocates a TOKEN_USER and stores its address in `information`.
        let status = unsafe {
            SeQueryInformationToken(token, wdk_sys::_TOKEN_INFORMATION_CLASS::TokenUser, &mut information)
        };
        // SAFETY: releases the reference taken above.
        unsafe { PsDereferencePrimaryToken(token) };
        crate::check(status)?;
        if information.is_null() {
            return Err(NtStatus::UNSUCCESSFUL);
        }
        // SAFETY: on success `information` is a TOKEN_USER whose `User.Sid`
        // points to a valid SID inside the same allocation; RtlLengthSid
        // gives that SID's size in bytes.
        let copy = unsafe {
            let sid = (*information.cast::<TOKEN_USER>()).User.Sid;
            let length = RtlLengthSid(sid) as usize;
            try_vec_from_slice(core::slice::from_raw_parts(sid.cast::<u8>().cast_const(), length))
        };
        // SAFETY: the buffer was allocated by SeQueryInformationToken for
        // the caller to free with ExFreePool.
        unsafe { ExFreePool(information) };
        copy.map(Self)
    }
}

/// Whether the current process is a 32-bit process on this 64-bit system.
pub fn current_process_is_32_bit() -> bool {
    // SAFETY: with a null IRP the routine examines the current process.
    unsafe { IoIs32bitProcess(core::ptr::null_mut()) != 0 }
}

/// The flags of the current process's binary-signature mitigation policy
/// (`PROCESS_MITIGATION_BINARY_SIGNATURE_POLICY`), or `None` if they cannot
/// be read.
pub fn current_process_signature_policy() -> Option<u32> {
    let mut information = PROCESS_MITIGATION_POLICY_INFORMATION {
        Policy: wdk_sys::_PROCESS_MITIGATION_POLICY::ProcessSignaturePolicy,
        Flags: 0,
    };
    // SAFETY: the buffer is a live local of the size passed; the routine
    // validates the information class and length.
    let status = unsafe {
        ZwQueryInformationProcess(
            CURRENT_PROCESS,
            wdk_sys::_PROCESSINFOCLASS::ProcessMitigationPolicy,
            core::ptr::from_mut(&mut information).cast(),
            core::mem::size_of::<PROCESS_MITIGATION_POLICY_INFORMATION>() as u32,
            core::ptr::null_mut(),
        )
    };
    NtStatus(status).is_success().then_some(information.Flags)
}
