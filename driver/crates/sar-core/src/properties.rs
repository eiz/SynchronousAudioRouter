// Copyright (c) 2026 Itty Bitty Binaries, Inc.
// SPDX-License-Identifier: MIT

//! Property requests and replies as bytes (sections 7 and 8, A.9).
//!
//! Kernel streaming hands a property handler two buffers: the request (a
//! `KSPROPERTY` followed by request-specific fields) and the value buffer.
//! The functions here decode the former and fill the latter, applying the
//! length rules of A.9.
#![forbid(unsafe_code)]

use ntstatus::NtStatus;

use crate::control::Completion;
use crate::format::WAVE_FORMAT_SIZE;
use crate::wire::{get_u32, get_u64, put_u32, put_u64};

/// Size of `KSPROPERTY` (set GUID, id, flags); request-specific fields follow.
pub const KSPROPERTY_SIZE: usize = 24;

/// The pin id of a `KSP_PIN` request.
pub fn pin_id(request: &[u8]) -> Option<u32> {
    (request.len() >= KSPROPERTY_SIZE + 4).then(|| get_u32(request, KSPROPERTY_SIZE))
}

/// The node id of a `KSNODEPROPERTY` request.
pub fn node_id(request: &[u8]) -> Option<u32> {
    (request.len() >= KSPROPERTY_SIZE + 4).then(|| get_u32(request, KSPROPERTY_SIZE))
}

/// The fields of `KSRTAUDIO_BUFFER_PROPERTY` and
/// `KSRTAUDIO_BUFFER_PROPERTY_WITH_NOTIFICATION` (both 40 bytes).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct BufferRequest {
    /// The address the client would like the buffer at; must be null.
    pub base_address: u64,
    pub requested_size: u32,
    /// Zero for the plain buffer property.
    pub notification_count: u32,
}

impl BufferRequest {
    pub const SIZE: usize = 40;

    pub fn parse(request: &[u8], with_notification: bool) -> Option<Self> {
        (request.len() >= Self::SIZE).then(|| Self {
            base_address: get_u64(request, 24),
            requested_size: get_u32(request, 32),
            notification_count: if with_notification { get_u32(request, 36) } else { 0 },
        })
    }
}

/// The event handle of a `KSRTAUDIO_NOTIFICATION_EVENT_PROPERTY` (32 bytes).
pub fn notification_event_handle(request: &[u8]) -> Option<u64> {
    (request.len() >= 32).then(|| get_u64(request, 24))
}

/// Applies the variable-length reply rule: a value buffer of length 0 gets
/// `STATUS_BUFFER_OVERFLOW`, a shorter-than-needed one
/// `STATUS_BUFFER_TOO_SMALL`, both reporting the needed size; otherwise
/// `fill` writes exactly `needed` bytes.
fn sized_reply(out: &mut [u8], needed: usize, fill: impl FnOnce(&mut [u8])) -> Completion {
    if out.is_empty() {
        Completion::new(NtStatus::BUFFER_OVERFLOW, needed)
    } else if out.len() < needed {
        Completion::new(NtStatus::BUFFER_TOO_SMALL, needed)
    } else {
        fill(&mut out[..needed]);
        Completion::new(NtStatus::SUCCESS, needed)
    }
}

/// Fixed-size reply. Kernel streaming enforces the minimum value size before
/// the handler runs; a short buffer here is refused all the same.
fn fixed_reply(out: &mut [u8], needed: usize, fill: impl FnOnce(&mut [u8])) -> Completion {
    if out.len() < needed {
        Completion::new(NtStatus::BUFFER_TOO_SMALL, needed)
    } else {
        fill(&mut out[..needed]);
        Completion::new(NtStatus::SUCCESS, needed)
    }
}

/// `KSPROPERTY_PIN_PHYSICALCONNECTION`: a `KSPIN_PHYSICALCONNECTION` naming
/// the other filter's interface `path` and its pin.
///
/// Layout (A.9): `Size` = 12 + path bytes, `Pin`, the path, a terminator.
/// The reply is 12 + path bytes + 2 long.
pub fn physical_connection(out: &mut [u8], pin: u32, path: &[u16]) -> Completion {
    let path_bytes = path.len() * 2;
    let needed = 12 + path_bytes + 2;
    sized_reply(out, needed, |out| {
        out.fill(0);
        put_u32(out, 0, (12 + path_bytes) as u32);
        put_u32(out, 4, pin);
        for (i, unit) in path.iter().enumerate() {
            out[8 + i * 2..10 + i * 2].copy_from_slice(&unit.to_le_bytes());
        }
        // The terminator and the padding after it are already zero.
    })
}

/// `KSPROPERTY_PIN_NAME`: the name in UTF-16 with a terminator.
pub fn pin_name(out: &mut [u8], name: &[u16]) -> Completion {
    let needed = name.len() * 2 + 2;
    sized_reply(out, needed, |out| {
        out.fill(0);
        for (i, unit) in name.iter().enumerate() {
            out[i * 2..i * 2 + 2].copy_from_slice(&unit.to_le_bytes());
        }
    })
}

/// `KSPROPERTY_PIN_GLOBALCINSTANCES`: a `KSPIN_CINSTANCES`.
pub fn instance_counts(out: &mut [u8], possible: u32, current: u32) -> Completion {
    fixed_reply(out, 8, |out| {
        put_u32(out, 0, possible);
        put_u32(out, 4, current);
    })
}

/// `KSPROPERTY_PIN_PROPOSEDATAFORMAT`, get: the 104-byte default format.
pub fn proposed_format(out: &mut [u8], format: &[u8; WAVE_FORMAT_SIZE]) -> Completion {
    sized_reply(out, WAVE_FORMAT_SIZE, |out| out.copy_from_slice(format))
}

/// `KSPROPERTY_PIN_PROPOSEDATAFORMAT`, set: the same length rules on the
/// supplied format. `Ok` hands back the bytes to validate.
pub fn proposed_format_input(data: &[u8]) -> Result<&[u8], Completion> {
    if data.is_empty() {
        Err(Completion::new(NtStatus::BUFFER_OVERFLOW, WAVE_FORMAT_SIZE))
    } else if data.len() < WAVE_FORMAT_SIZE {
        Err(Completion::new(NtStatus::BUFFER_TOO_SMALL, WAVE_FORMAT_SIZE))
    } else {
        Ok(data)
    }
}

/// `KSPROPERTY_AUDIO_CHANNEL_CONFIG`, get: the 4-byte speaker mask.
pub fn speaker_mask(out: &mut [u8], mask: u32) -> Completion {
    fixed_reply(out, 4, |out| put_u32(out, 0, mask))
}

/// `KSPROPERTY_AUDIO_CHANNEL_CONFIG`, set: the mask the caller supplied.
pub fn speaker_mask_input(data: &[u8]) -> Result<u32, Completion> {
    if data.len() < 4 {
        Err(Completion::new(NtStatus::BUFFER_TOO_SMALL, 4))
    } else {
        Ok(get_u32(data, 0))
    }
}

/// `KSPROPERTY_RTAUDIO_BUFFER[_WITH_NOTIFICATION]`: a `KSRTAUDIO_BUFFER`
/// (address, actual size, "call memory barrier" = false).
pub fn rt_buffer(out: &mut [u8], address: u64, size: u32) -> Completion {
    fixed_reply(out, 16, |out| {
        put_u64(out, 0, address);
        put_u32(out, 8, size);
        put_u32(out, 12, 0);
    })
}

/// `KSPROPERTY_RTAUDIO_POSITIONREGISTER`: a `KSRTAUDIO_HWREGISTER` with the
/// register's address, width 32, numerator and denominator 0, and accuracy.
pub fn hw_register(out: &mut [u8], address: u64, accuracy: u32) -> Completion {
    fixed_reply(out, 40, |out| {
        out.fill(0);
        put_u64(out, 0, address);
        put_u32(out, 8, 32);
        // Numerator (16) and Denominator (24) stay zero.
        put_u32(out, 32, accuracy);
    })
}

/// `KSPROPERTY_RTAUDIO_HWLATENCY`: a `KSRTAUDIO_HWLATENCY`, all zero.
pub fn hw_latency(out: &mut [u8]) -> Completion {
    fixed_reply(out, 12, |out| out.fill(0))
}

/// A 4-byte `BOOL`.
pub fn boolean(out: &mut [u8], value: bool) -> Completion {
    fixed_reply(out, 4, |out| put_u32(out, 0, u32::from(value)))
}

#[cfg(test)]
mod tests {
    use super::*;

    fn utf16(s: &str) -> Vec<u16> {
        s.encode_utf16().collect()
    }

    #[test]
    fn request_fields() {
        let mut r = [0u8; 40];
        put_u32(&mut r, 24, 7);
        assert_eq!(pin_id(&r), Some(7));
        assert_eq!(node_id(&r), Some(7));
        assert_eq!(pin_id(&r[..27]), None);

        put_u64(&mut r, 24, 0x1122_3344_5566_7788);
        put_u32(&mut r, 32, 96_000);
        put_u32(&mut r, 36, 2);
        assert_eq!(
            BufferRequest::parse(&r, true),
            Some(BufferRequest { base_address: 0x1122_3344_5566_7788, requested_size: 96_000, notification_count: 2 })
        );
        // The plain property has padding where the count would be.
        assert_eq!(BufferRequest::parse(&r, false).unwrap().notification_count, 0);
        assert_eq!(BufferRequest::parse(&r[..39], true), None);
        assert_eq!(notification_event_handle(&r), Some(0x1122_3344_5566_7788));
        assert_eq!(notification_event_handle(&r[..31]), None);
    }

    #[test]
    fn physical_connection_layout() {
        let path = utf16("\\??\\x");
        let mut out = [0xFFu8; 64];
        let c = physical_connection(&mut out, 1, &path);
        assert_eq!(c, Completion::new(NtStatus::SUCCESS, 12 + 10 + 2));
        assert_eq!(get_u32(&out, 0), 22, "Size = 12 + path bytes");
        assert_eq!(get_u32(&out, 4), 1);
        assert_eq!(&out[8..18], &[b'\\', 0, b'?', 0, b'?', 0, b'\\', 0, b'x', 0]);
        assert_eq!(&out[18..24], &[0; 6], "terminator and padding");
        assert_eq!(out[24], 0xFF, "nothing beyond the reported length");
    }

    #[test]
    fn variable_length_rules() {
        let path = utf16("abc");
        let needed = 12 + 6 + 2;
        assert_eq!(physical_connection(&mut [], 0, &path), Completion::new(NtStatus::BUFFER_OVERFLOW, needed));
        let mut short = [0xEEu8; 19];
        assert_eq!(physical_connection(&mut short, 0, &path), Completion::new(NtStatus::BUFFER_TOO_SMALL, needed));
        assert!(short.iter().all(|b| *b == 0xEE), "a short buffer is not touched");

        let name = utf16("Speakers");
        assert_eq!(pin_name(&mut [], &name), Completion::new(NtStatus::BUFFER_OVERFLOW, 18));
        assert_eq!(pin_name(&mut [0u8; 17], &name), Completion::new(NtStatus::BUFFER_TOO_SMALL, 18));
        let mut out = [0xFFu8; 20];
        assert_eq!(pin_name(&mut out, &name), Completion::new(NtStatus::SUCCESS, 18));
        assert_eq!(&out[..4], &[b'S', 0, b'p', 0]);
        assert_eq!(&out[16..18], &[0, 0]);
        assert_eq!(out[18], 0xFF);
        // An empty name is just a terminator.
        assert_eq!(pin_name(&mut [0xFFu8; 2], &[]), Completion::new(NtStatus::SUCCESS, 2));
    }

    #[test]
    fn proposed_format_rules() {
        let format = [0x5Au8; 104];
        assert_eq!(proposed_format(&mut [], &format), Completion::new(NtStatus::BUFFER_OVERFLOW, 104));
        assert_eq!(proposed_format(&mut [0u8; 103], &format), Completion::new(NtStatus::BUFFER_TOO_SMALL, 104));
        let mut out = [0u8; 200];
        assert_eq!(proposed_format(&mut out, &format), Completion::new(NtStatus::SUCCESS, 104));
        assert_eq!(&out[..104], &format[..]);
        assert_eq!(out[104], 0);

        assert_eq!(proposed_format_input(&[]), Err(Completion::new(NtStatus::BUFFER_OVERFLOW, 104)));
        assert_eq!(proposed_format_input(&[0u8; 64]), Err(Completion::new(NtStatus::BUFFER_TOO_SMALL, 104)));
        assert!(proposed_format_input(&[0u8; 104]).is_ok());
    }

    #[test]
    fn fixed_replies() {
        let mut out = [0xFFu8; 48];
        assert_eq!(instance_counts(&mut out, 1, 0), Completion::new(NtStatus::SUCCESS, 8));
        assert_eq!(&out[..8], &[1, 0, 0, 0, 0, 0, 0, 0]);

        assert_eq!(rt_buffer(&mut out, 0x7FFE_0000_1000, 7680), Completion::new(NtStatus::SUCCESS, 16));
        assert_eq!(get_u64(&out, 0), 0x7FFE_0000_1000);
        assert_eq!(get_u32(&out, 8), 7680);
        assert_eq!(get_u32(&out, 12), 0, "no memory barrier needed");

        let mut out = [0xFFu8; 48];
        assert_eq!(hw_register(&mut out, 0xABCD_0004, 3840), Completion::new(NtStatus::SUCCESS, 40));
        assert_eq!(get_u64(&out, 0), 0xABCD_0004);
        assert_eq!(get_u32(&out, 8), 32);
        assert_eq!(get_u64(&out, 16), 0);
        assert_eq!(get_u64(&out, 24), 0);
        assert_eq!(get_u32(&out, 32), 3840);
        assert_eq!(get_u32(&out, 36), 0);
        assert_eq!(out[40], 0xFF);

        let mut out = [0xFFu8; 16];
        assert_eq!(hw_latency(&mut out), Completion::new(NtStatus::SUCCESS, 12));
        assert_eq!(&out[..12], &[0; 12]);
        assert_eq!(boolean(&mut out, true), Completion::new(NtStatus::SUCCESS, 4));
        assert_eq!(&out[..4], &[1, 0, 0, 0]);
        assert_eq!(speaker_mask(&mut out, 0x3F), Completion::new(NtStatus::SUCCESS, 4));
        assert_eq!(get_u32(&out, 0), 0x3F);

        assert_eq!(speaker_mask(&mut [0u8; 3], 1), Completion::new(NtStatus::BUFFER_TOO_SMALL, 4));
        assert_eq!(speaker_mask_input(&[1, 0, 0]), Err(Completion::new(NtStatus::BUFFER_TOO_SMALL, 4)));
        assert_eq!(speaker_mask_input(&[3, 0, 0, 0, 9]), Ok(3));
        assert_eq!(hw_register(&mut [0u8; 39], 0, 0).status, NtStatus::BUFFER_TOO_SMALL);
    }
}
