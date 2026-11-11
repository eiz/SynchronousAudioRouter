// Copyright (c) 2026 Itty Bitty Binaries, Inc.
// SPDX-License-Identifier: MIT

//! Little-endian field access on byte slices.
//!
//! All structures of the binary interface are "natural 32-bit fields,
//! little-endian" (section 5), so they are read and written field by field
//! instead of being cast to Rust structures.
#![forbid(unsafe_code)]

/// Reads the `u16` at `offset`. Panics if out of range (callers check
/// lengths first; an out-of-range offset is a programming error).
pub fn get_u16(bytes: &[u8], offset: usize) -> u16 {
    u16::from_le_bytes([bytes[offset], bytes[offset + 1]])
}

/// Reads the `u32` at `offset`.
pub fn get_u32(bytes: &[u8], offset: usize) -> u32 {
    u32::from_le_bytes([
        bytes[offset],
        bytes[offset + 1],
        bytes[offset + 2],
        bytes[offset + 3],
    ])
}

/// Reads the `u64` at `offset`.
pub fn get_u64(bytes: &[u8], offset: usize) -> u64 {
    let mut raw = [0u8; 8];
    raw.copy_from_slice(&bytes[offset..offset + 8]);
    u64::from_le_bytes(raw)
}

/// Writes a `u16` at `offset`.
pub fn put_u16(bytes: &mut [u8], offset: usize, value: u16) {
    bytes[offset..offset + 2].copy_from_slice(&value.to_le_bytes());
}

/// Writes a `u32` at `offset`.
pub fn put_u32(bytes: &mut [u8], offset: usize, value: u32) {
    bytes[offset..offset + 4].copy_from_slice(&value.to_le_bytes());
}

/// Writes a `u64` at `offset`.
pub fn put_u64(bytes: &mut [u8], offset: usize, value: u64) {
    bytes[offset..offset + 8].copy_from_slice(&value.to_le_bytes());
}

/// A GUID in its in-memory (little-endian `Data1`..`Data3`, then eight
/// bytes) form.
pub type GuidBytes = [u8; 16];

/// Builds the in-memory form of a GUID from its four parts.
pub const fn guid(data1: u32, data2: u16, data3: u16, data4: [u8; 8]) -> GuidBytes {
    let a = data1.to_le_bytes();
    let b = data2.to_le_bytes();
    let c = data3.to_le_bytes();
    [
        a[0], a[1], a[2], a[3], b[0], b[1], c[0], c[1], data4[0], data4[1], data4[2], data4[3],
        data4[4], data4[5], data4[6], data4[7],
    ]
}

/// The 16 bytes at `offset` as a GUID.
pub fn get_guid(bytes: &[u8], offset: usize) -> GuidBytes {
    let mut raw = [0u8; 16];
    raw.copy_from_slice(&bytes[offset..offset + 16]);
    raw
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn round_trips() {
        let mut b = [0u8; 16];
        put_u16(&mut b, 1, 0xBEEF);
        put_u32(&mut b, 4, 0xDEAD_BEEF);
        put_u64(&mut b, 8, 0x0123_4567_89AB_CDEF);
        assert_eq!(get_u16(&b, 1), 0xBEEF);
        assert_eq!(get_u32(&b, 4), 0xDEAD_BEEF);
        assert_eq!(get_u64(&b, 8), 0x0123_4567_89AB_CDEF);
        assert_eq!(b[4], 0xEF);
    }

    #[test]
    fn guid_layout() {
        // {6994AD04-93EF-11D0-A3CC-00A0C9223196}
        let g = guid(
            0x6994AD04,
            0x93EF,
            0x11D0,
            [0xA3, 0xCC, 0x00, 0xA0, 0xC9, 0x22, 0x31, 0x96],
        );
        assert_eq!(
            g,
            [
                0x04, 0xAD, 0x94, 0x69, 0xEF, 0x93, 0xD0, 0x11, 0xA3, 0xCC, 0x00, 0xA0, 0xC9, 0x22,
                0x31, 0x96
            ]
        );
    }
}
