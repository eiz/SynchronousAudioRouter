// Copyright (c) 2026 Itty Bitty Binaries, Inc.
// SPDX-License-Identifier: MIT

//! The control interface's request codes and buffer-length rules
//! (section 5, A.2, A.3, A.5).
#![forbid(unsafe_code)]

use ntstatus::NtStatus;

use crate::wire::put_u64;

/// `CTL_CODE(DeviceType, Function, Method, Access)` from the Windows headers.
pub const fn ctl_code(device_type: u32, function: u32, method: u32, access: u32) -> u32 {
    (device_type << 16) | (access << 14) | (function << 2) | method
}

const FILE_DEVICE_UNKNOWN: u32 = 0x22;
const METHOD_BUFFERED: u32 = 0;
const METHOD_NEITHER: u32 = 3;
const FILE_READ_AND_WRITE_DATA: u32 = 3;

pub const IOCTL_SET_BUFFER_LAYOUT: u32 =
    ctl_code(FILE_DEVICE_UNKNOWN, 1, METHOD_NEITHER, FILE_READ_AND_WRITE_DATA);
pub const IOCTL_CREATE_ENDPOINT: u32 =
    ctl_code(FILE_DEVICE_UNKNOWN, 2, METHOD_NEITHER, FILE_READ_AND_WRITE_DATA);
pub const IOCTL_WAIT_NOTIFICATIONS: u32 =
    ctl_code(FILE_DEVICE_UNKNOWN, 3, METHOD_BUFFERED, FILE_READ_AND_WRITE_DATA);
pub const IOCTL_START_REGISTRY_FILTER: u32 =
    ctl_code(FILE_DEVICE_UNKNOWN, 4, METHOD_NEITHER, FILE_READ_AND_WRITE_DATA);

/// The name a control open arrives with: a backslash and the control
/// interface's reference string (A.1). Compared without regard to case.
pub const CONTROL_FILE_NAME: &str = "\\{0EB287D4-6C04-4926-AE19-3C066A4C3F3A}";
/// The control interface's reference string.
pub const CONTROL_REFERENCE_STRING: &str = "{0EB287D4-6C04-4926-AE19-3C066A4C3F3A}";
/// The control interface's device interface class,
/// {C16E8D6C-C4CC-4C76-B11C-79B8414EA968}.
pub const CONTROL_INTERFACE_CLASS: crate::wire::GuidBytes = crate::wire::guid(
    0xC16E_8D6C,
    0xC4CC,
    0x4C76,
    [0xB1, 0x1C, 0x79, 0xB8, 0x41, 0x4E, 0xA9, 0x68],
);

/// A control request the driver implements.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Request {
    SetBufferLayout,
    CreateEndpoint,
    WaitNotifications,
    StartRegistryFilter,
}

impl Request {
    /// Maps a device control code to a request; `None` for anything else.
    pub fn decode(code: u32) -> Option<Self> {
        match code {
            IOCTL_SET_BUFFER_LAYOUT => Some(Self::SetBufferLayout),
            IOCTL_CREATE_ENDPOINT => Some(Self::CreateEndpoint),
            IOCTL_WAIT_NOTIFICATIONS => Some(Self::WaitNotifications),
            IOCTL_START_REGISTRY_FILTER => Some(Self::StartRegistryFilter),
            _ => None,
        }
    }
}

/// How a request ends: the status and the byte count reported with it.
pub type Completion = ntstatus::IoStatus;

/// "An input buffer shorter than the structure fails with
/// STATUS_BUFFER_OVERFLOW" (information 0).
pub fn check_input_length(length: usize, required: usize) -> Result<(), Completion> {
    if length < required {
        Err(Completion::status(NtStatus::BUFFER_OVERFLOW))
    } else {
        Ok(())
    }
}

/// "A zero-length output buffer fails with STATUS_BUFFER_OVERFLOW and one
/// that is too short with STATUS_BUFFER_TOO_SMALL; in both cases the
/// required size is reported."
pub fn check_output_length(length: usize, required: usize) -> Result<(), Completion> {
    if length == 0 {
        Err(Completion::new(NtStatus::BUFFER_OVERFLOW, required))
    } else if length < required {
        Err(Completion::new(NtStatus::BUFFER_TOO_SMALL, required))
    } else {
        Ok(())
    }
}

/// Size of one record of the "wait for notification events" output.
pub const NOTIFICATION_RECORD_SIZE: usize = 16;

/// How many records an output buffer of `length` bytes holds.
pub fn notification_capacity(length: usize) -> usize {
    length / NOTIFICATION_RECORD_SIZE
}

/// Encodes one output record: the handle, then `associatedData` with the
/// generation in the low and the endpoint index in the high 32 bits.
pub fn encode_notification(handle: u64, generation: u32, index: u32) -> [u8; NOTIFICATION_RECORD_SIZE] {
    let mut out = [0u8; NOTIFICATION_RECORD_SIZE];
    put_u64(&mut out, 0, handle);
    put_u64(&mut out, 8, (u64::from(index) << 32) | u64::from(generation));
    out
}

/// Case-insensitive (ASCII) comparison of a UTF-16 string with an ASCII
/// string. Used for the control file name and for registry key paths, whose
/// reference values are pure ASCII: a UTF-16 string can only equal them if it
/// is ASCII too.
pub fn utf16_eq_ascii_ignore_case(utf16: &[u16], ascii: &str) -> bool {
    let ascii = ascii.as_bytes();
    utf16.len() == ascii.len()
        && utf16.iter().zip(ascii).all(|(w, a)| match u8::try_from(*w) {
            Ok(b) => b.eq_ignore_ascii_case(a),
            Err(_) => false,
        })
}

/// Whether a file opened with `file_name` is a control handle (A.1).
pub fn is_control_file_name(file_name: &[u16]) -> bool {
    utf16_eq_ascii_ignore_case(file_name, CONTROL_FILE_NAME)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn utf16(s: &str) -> Vec<u16> {
        s.encode_utf16().collect()
    }

    #[test]
    fn codes_match_a2() {
        assert_eq!(IOCTL_SET_BUFFER_LAYOUT, 0x0022_C007);
        assert_eq!(IOCTL_CREATE_ENDPOINT, 0x0022_C00B);
        assert_eq!(IOCTL_WAIT_NOTIFICATIONS, 0x0022_C00C);
        assert_eq!(IOCTL_START_REGISTRY_FILTER, 0x0022_C013);
    }

    #[test]
    fn decode() {
        assert_eq!(Request::decode(0x0022_C007), Some(Request::SetBufferLayout));
        assert_eq!(Request::decode(0x0022_C00B), Some(Request::CreateEndpoint));
        assert_eq!(Request::decode(0x0022_C00C), Some(Request::WaitNotifications));
        assert_eq!(Request::decode(0x0022_C013), Some(Request::StartRegistryFilter));
        // The retired function 5 has no meaning, in any method.
        for method in 0..4 {
            assert_eq!(Request::decode(ctl_code(0x22, 5, method, 3)), None);
        }
        // Same function, different method or access: not ours.
        assert_eq!(Request::decode(ctl_code(0x22, 1, 0, 3)), None);
        assert_eq!(Request::decode(ctl_code(0x22, 1, 3, 0)), None);
        assert_eq!(Request::decode(0), None);
    }

    #[test]
    fn length_rules() {
        assert_eq!(check_input_length(19, 20), Err(Completion::status(NtStatus::BUFFER_OVERFLOW)));
        assert_eq!(check_input_length(0, 20), Err(Completion::status(NtStatus::BUFFER_OVERFLOW)));
        assert_eq!(check_input_length(20, 20), Ok(()));
        assert_eq!(check_input_length(4096, 20), Ok(()));

        assert_eq!(check_output_length(0, 16), Err(Completion::new(NtStatus::BUFFER_OVERFLOW, 16)));
        assert_eq!(check_output_length(15, 16), Err(Completion::new(NtStatus::BUFFER_TOO_SMALL, 16)));
        assert_eq!(check_output_length(16, 16), Ok(()));
    }

    #[test]
    fn notification_records() {
        assert_eq!(notification_capacity(0), 0);
        assert_eq!(notification_capacity(15), 0);
        assert_eq!(notification_capacity(16), 1);
        assert_eq!(notification_capacity(32 * 16 + 7), 32);
        let r = encode_notification(0x1234, 0x0000_0007, 2339);
        assert_eq!(&r[0..8], &0x1234u64.to_le_bytes());
        assert_eq!(&r[8..12], &7u32.to_le_bytes());
        assert_eq!(&r[12..16], &2339u32.to_le_bytes());
    }

    #[test]
    fn control_class() {
        // {C16E8D6C-C4CC-4C76-B11C-79B8414EA968} in memory order.
        assert_eq!(
            CONTROL_INTERFACE_CLASS,
            [0x6C, 0x8D, 0x6E, 0xC1, 0xCC, 0xC4, 0x76, 0x4C, 0xB1, 0x1C, 0x79, 0xB8, 0x41, 0x4E, 0xA9, 0x68]
        );
    }

    #[test]
    fn control_name() {
        assert!(is_control_file_name(&utf16("\\{0EB287D4-6C04-4926-AE19-3C066A4C3F3A}")));
        assert!(is_control_file_name(&utf16("\\{0eb287d4-6c04-4926-ae19-3c066a4c3f3a}")));
        assert!(!is_control_file_name(&utf16("{0EB287D4-6C04-4926-AE19-3C066A4C3F3A}")));
        assert!(!is_control_file_name(&utf16("\\{0EB287D4-6C04-4926-AE19-3C066A4C3F3A}\\")));
        assert!(!is_control_file_name(&utf16("\\{0EB287D4-6C04-4926-AE19-3C066A4C3F3B}")));
        assert!(!is_control_file_name(&[]));
        // A non-ASCII unit never matches, even one whose low byte would.
        let mut name = utf16("\\{0EB287D4-6C04-4926-AE19-3C066A4C3F3A}");
        name[1] = 0x017B; // low byte is '{'
        assert!(!is_control_file_name(&name));
        assert_eq!(CONTROL_FILE_NAME.len(), CONTROL_REFERENCE_STRING.len() + 1);
    }
}
