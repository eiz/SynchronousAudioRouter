// Copyright (c) 2026 Itty Bitty Binaries, Inc.
// SPDX-License-Identifier: MIT

//! `NTSTATUS` as a value type.
//!
//! The numeric values are the ones in the Windows header `ntstatus.h` and in
//! appendix A.4 of the driver specification.
#![cfg_attr(not(test), no_std)]
#![forbid(unsafe_code)]

/// A Windows `NTSTATUS`.
#[derive(Clone, Copy, PartialEq, Eq, Hash)]
#[repr(transparent)]
#[must_use]
pub struct NtStatus(pub i32);

macro_rules! codes {
    ($($(#[$doc:meta])* $name:ident = $value:literal;)*) => {
        impl NtStatus {
            $($(#[$doc])* pub const $name: Self = Self::from_u32($value);)*

            /// The symbolic name, for the codes this crate knows.
            pub const fn name(self) -> Option<&'static str> {
                match self.to_u32() {
                    $($value => Some(concat!("STATUS_", stringify!($name))),)*
                    _ => None,
                }
            }
        }
    };
}

codes! {
    SUCCESS = 0x0000_0000;
    PENDING = 0x0000_0103;
    OBJECT_NAME_EXISTS = 0x4000_0000;
    DATATYPE_MISALIGNMENT = 0x8000_0002;
    BUFFER_OVERFLOW = 0x8000_0005;
    UNSUCCESSFUL = 0xC000_0001;
    NOT_IMPLEMENTED = 0xC000_0002;
    ACCESS_VIOLATION = 0xC000_0005;
    INVALID_HANDLE = 0xC000_0008;
    INVALID_PARAMETER = 0xC000_000D;
    INVALID_DEVICE_REQUEST = 0xC000_0010;
    BUFFER_TOO_SMALL = 0xC000_0023;
    OBJECT_NAME_NOT_FOUND = 0xC000_0034;
    INSUFFICIENT_RESOURCES = 0xC000_009A;
    NOT_SUPPORTED = 0xC000_00BB;
    CANCELLED = 0xC000_0120;
    FILE_CLOSED = 0xC000_0128;
    NOT_FOUND = 0xC000_0225;
    NO_MATCH = 0xC000_0272;
    CALLBACK_BYPASS = 0xC000_0503;
    RESOURCE_IN_USE = 0xC000_0708;
    INVALID_STATE_TRANSITION = 0xC000_A003;
}

impl NtStatus {
    /// Builds a status from its unsigned (documentation) form.
    pub const fn from_u32(value: u32) -> Self {
        Self(value as i32)
    }

    /// The unsigned (documentation) form.
    pub const fn to_u32(self) -> u32 {
        self.0 as u32
    }

    /// `NT_SUCCESS`: success and informational codes.
    pub const fn is_success(self) -> bool {
        self.0 >= 0
    }

    /// `NT_ERROR`: severity "error" (warnings such as
    /// `STATUS_BUFFER_OVERFLOW` are neither success nor error).
    pub const fn is_error(self) -> bool {
        self.to_u32() >> 30 == 3
    }

    /// `Ok(())` for `NT_SUCCESS`, `Err(self)` otherwise.
    pub const fn into_result(self) -> Result<(), NtStatus> {
        if self.is_success() {
            Ok(())
        } else {
            Err(self)
        }
    }
}

/// How an I/O request ends: its status and the byte count reported with it
/// (the two halves of an `IO_STATUS_BLOCK`).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[must_use]
pub struct IoStatus {
    pub status: NtStatus,
    pub information: usize,
}

impl IoStatus {
    pub const fn new(status: NtStatus, information: usize) -> Self {
        Self { status, information }
    }

    /// An outcome that reports no bytes.
    pub const fn status(status: NtStatus) -> Self {
        Self { status, information: 0 }
    }

    pub const SUCCESS: Self = Self::status(NtStatus::SUCCESS);
}

impl From<NtStatus> for IoStatus {
    fn from(status: NtStatus) -> Self {
        Self::status(status)
    }
}

impl core::fmt::Debug for NtStatus {
    fn fmt(&self, f: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
        match self.name() {
            Some(name) => f.write_str(name),
            None => write!(f, "NtStatus({:#010X})", self.to_u32()),
        }
    }
}

#[cfg(test)]
mod tests {
    use super::NtStatus;

    #[test]
    fn values_match_appendix_a4() {
        let table: &[(NtStatus, u32)] = &[
            (NtStatus::SUCCESS, 0x0000_0000),
            (NtStatus::PENDING, 0x0000_0103),
            (NtStatus::OBJECT_NAME_EXISTS, 0x4000_0000),
            (NtStatus::DATATYPE_MISALIGNMENT, 0x8000_0002),
            (NtStatus::BUFFER_OVERFLOW, 0x8000_0005),
            (NtStatus::UNSUCCESSFUL, 0xC000_0001),
            (NtStatus::NOT_IMPLEMENTED, 0xC000_0002),
            (NtStatus::ACCESS_VIOLATION, 0xC000_0005),
            (NtStatus::INVALID_PARAMETER, 0xC000_000D),
            (NtStatus::BUFFER_TOO_SMALL, 0xC000_0023),
            (NtStatus::INSUFFICIENT_RESOURCES, 0xC000_009A),
            (NtStatus::NOT_SUPPORTED, 0xC000_00BB),
            (NtStatus::CANCELLED, 0xC000_0120),
            (NtStatus::FILE_CLOSED, 0xC000_0128),
            (NtStatus::NOT_FOUND, 0xC000_0225),
            (NtStatus::NO_MATCH, 0xC000_0272),
            (NtStatus::CALLBACK_BYPASS, 0xC000_0503),
            (NtStatus::RESOURCE_IN_USE, 0xC000_0708),
            (NtStatus::INVALID_STATE_TRANSITION, 0xC000_A003),
        ];
        for (status, value) in table {
            assert_eq!(status.to_u32(), *value);
        }
    }

    #[test]
    fn severity() {
        assert!(NtStatus::SUCCESS.is_success());
        assert!(NtStatus::PENDING.is_success());
        assert!(NtStatus::OBJECT_NAME_EXISTS.is_success());
        assert!(!NtStatus::BUFFER_OVERFLOW.is_success());
        assert!(!NtStatus::BUFFER_OVERFLOW.is_error());
        assert!(!NtStatus::DATATYPE_MISALIGNMENT.is_error());
        assert!(NtStatus::BUFFER_TOO_SMALL.is_error());
        assert!(NtStatus::CALLBACK_BYPASS.is_error());
        assert_eq!(NtStatus::NO_MATCH.into_result(), Err(NtStatus::NO_MATCH));
        assert_eq!(NtStatus::PENDING.into_result(), Ok(()));
    }

    #[test]
    fn debug_names() {
        assert_eq!(format!("{:?}", NtStatus::NO_MATCH), "STATUS_NO_MATCH");
        assert_eq!(format!("{:?}", NtStatus::from_u32(0xC000_0BAD)), "NtStatus(0xC0000BAD)");
    }
}
