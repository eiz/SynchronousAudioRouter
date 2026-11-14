// Copyright (c) 2026 Itty Bitty Binaries, Inc.
// SPDX-License-Identifier: MIT

//! Application routing support: the registry filter (section 10, A.11).
//!
//! Once started, the filter answers reads of the in-process server of the
//! Windows audio device enumerator class (and of the audio interface
//! activation class) with that of SAR's wrapper class, for processes of
//! one user that are able to load the wrapper.
#![forbid(unsafe_code)]

use ntk::{
    alloc_ext::try_zeroed_bytes,
    registry::{FilterDecision, RegistryFilter, RegistryKey, ValueRead, KEY_QUERY_VALUE},
    security::{current_process_is_32_bit, current_process_signature_policy, UserSid},
};
use ntstatus::NtStatus;
use sar_core::{
    endpoint::WideString,
    registry::{
        classify_enumerate, classify_query, find_redirection, signature_policy_restricts, source_length_bounds,
        FilteredOperation,
    },
};

/// Size of the first buffer tried for the substitute read; a class's
/// in-process server path nearly always fits.
const FIRST_BUFFER_SIZE: usize = 1024;

pub struct RoutingFilter {
    /// The user of the process that started the filter.
    user: UserSid,
}

impl RoutingFilter {
    pub fn new(user: UserSid) -> Self {
        Self { user }
    }

    /// Whether the reading process qualifies (section 10): same user as the
    /// process that started the filter, and not restricted to images the
    /// wrapper could not satisfy. If the restriction cannot be determined
    /// the process counts as unrestricted.
    fn process_qualifies(&self) -> bool {
        match UserSid::of_current_process() {
            Ok(user) if user == self.user => {}
            _ => return false,
        }
        !current_process_signature_policy().is_some_and(signature_policy_restricts)
    }
}

impl RegistryFilter for RoutingFilter {
    fn pre_value_read(&self, read: &mut ValueRead<'_>) -> FilterDecision {
        // This runs for every value read on the system; discard the
        // uninteresting ones as cheaply as possible.
        let operation = match (read.value_name_length(), read.index()) {
            (Some(length), _) => classify_query(length),
            (None, Some(index)) => classify_enumerate(index),
            (None, None) => None,
        };
        let Some(operation) = operation else {
            return FilterDecision::PassThrough;
        };
        let wow64 = current_process_is_32_bit();
        let (shortest, longest) = source_length_bounds();
        let redirection = read.with_key_path(|path| {
            if (shortest..=longest).contains(&path.len()) {
                find_redirection(path, wow64)
            } else {
                None
            }
        });
        let Ok(Some(redirection)) = redirection else {
            return FilterDecision::PassThrough;
        };
        if !self.process_qualifies() {
            return FilterDecision::PassThrough;
        }

        // If SAR's user-mode part is not registered, the wrapper's key does
        // not open and the read passes through unchanged.
        let Some(path) = WideString::<160>::from_ascii(redirection.substitute) else {
            return FilterDecision::PassThrough;
        };
        let Ok(key) = RegistryKey::open(path.as_units(), KEY_QUERY_VALUE) else {
            return FilterDecision::PassThrough;
        };

        // The same read, with the same information class and buffer length,
        // on the substitute key. The caller's buffer is user memory, so the
        // read goes to a kernel buffer first. The caller may name any
        // length; start small and grow only to what the value needs.
        let caller_length = read.output_length() as usize;
        let class = read.information_class();
        let mut size = caller_length.min(FIRST_BUFFER_SIZE);
        let (status, result_length, buffer) = loop {
            let Ok(mut buffer) = try_zeroed_bytes(size) else {
                // Out of memory: step aside and let the registry answer, as
                // in every other case the filter does not handle.
                return FilterDecision::PassThrough;
            };
            let (status, result_length) = match operation {
                FilteredOperation::QueryDefaultValue => key.query_value(&[], class, &mut buffer),
                FilteredOperation::EnumerateFirstValue => key.enumerate_value(0, class, &mut buffer),
            };
            let too_small = status == NtStatus::BUFFER_OVERFLOW || status == NtStatus::BUFFER_TOO_SMALL;
            let needed = result_length as usize;
            if too_small && size < caller_length && needed > size {
                size = caller_length.min(needed);
                continue;
            }
            break (status, result_length, buffer);
        };

        if status.is_success() {
            let valid = (result_length as usize).min(buffer.len());
            return match read.write_result(&buffer[..valid], result_length) {
                Ok(()) => FilterDecision::Completed,
                Err(status) => FilterDecision::Fail(status),
            };
        }
        // The substitute read failed: its status becomes the caller's
        // result, with the result length filled in. On an overflow the
        // registry has filled the buffer as far as it goes, and callers
        // rely on that partial data, so it is passed on as well.
        let partial: &[u8] = if status == NtStatus::BUFFER_OVERFLOW { &buffer } else { &[] };
        match read.write_result(partial, result_length) {
            Ok(()) => FilterDecision::Fail(status),
            Err(write_status) => FilterDecision::Fail(write_status),
        }
    }
}
