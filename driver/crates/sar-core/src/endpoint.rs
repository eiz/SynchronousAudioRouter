// Copyright (c) 2026 Itty Bitty Binaries, Inc.
// SPDX-License-Identifier: MIT

//! The "create endpoint" request and the names derived from it
//! (section 5, section 7 "Identity", A.5, A.9).
#![forbid(unsafe_code)]

use ntstatus::NtStatus;

use crate::layout::{Layout, MAX_ENDPOINTS};
use crate::wire::{get_u16, get_u32};

/// UTF-16 code units in the `id` and `name` arrays.
pub const NAME_UNITS: usize = 64;
pub const MIN_CHANNELS: u32 = 1;
pub const MAX_CHANNELS: u32 = 32;

/// Which way audio flows, from the point of view of Windows applications.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Direction {
    /// `type` 1: the host writes, applications capture.
    Recording,
    /// `type` 2: applications play, the host reads.
    Playback,
}

/// The "create endpoint" input structure (A.5), 268 bytes, validated.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct EndpointRequest {
    pub direction: Direction,
    pub index: u32,
    pub channel_count: u32,
    id: [u16; NAME_UNITS],
    name: [u16; NAME_UNITS],
}

fn read_name(bytes: &[u8], offset: usize) -> [u16; NAME_UNITS] {
    let mut out = [0u16; NAME_UNITS];
    for (i, unit) in out.iter_mut().enumerate() {
        *unit = get_u16(bytes, offset + i * 2);
    }
    // "The driver treats character 64 of id and name as a terminator
    // whatever it holds."
    out[NAME_UNITS - 1] = 0;
    out
}

fn until_nul(units: &[u16; NAME_UNITS]) -> &[u16] {
    let len = units.iter().position(|u| *u == 0).unwrap_or(NAME_UNITS - 1);
    &units[..len]
}

impl EndpointRequest {
    /// Size of the input structure.
    pub const SIZE: usize = 268;

    /// Decodes and range-checks the structure.
    pub fn parse(bytes: &[u8; Self::SIZE]) -> Result<Self, NtStatus> {
        let direction = match get_u32(bytes, 0) {
            1 => Direction::Recording,
            2 => Direction::Playback,
            _ => return Err(NtStatus::INVALID_PARAMETER),
        };
        let index = get_u32(bytes, 4);
        let channel_count = get_u32(bytes, 8);
        if index >= MAX_ENDPOINTS || !(MIN_CHANNELS..=MAX_CHANNELS).contains(&channel_count) {
            return Err(NtStatus::INVALID_PARAMETER);
        }
        Ok(Self {
            direction,
            index,
            channel_count,
            id: read_name(bytes, 12),
            name: read_name(bytes, 140),
        })
    }

    /// The identifier, without terminator: at most 63 units.
    pub fn id(&self) -> &[u16] {
        until_nul(&self.id)
    }

    /// The display name, without terminator: at most 63 units.
    pub fn name(&self) -> &[u16] {
        until_nul(&self.name)
    }
}

/// A short UTF-16 string with inline storage and a terminator, for the
/// names the driver builds or keeps per endpoint.
#[derive(Clone, PartialEq, Eq)]
pub struct WideString<const N: usize> {
    units: [u16; N],
    len: usize,
}

impl<const N: usize> WideString<N> {
    pub const fn new() -> Self {
        Self { units: [0; N], len: 0 }
    }

    /// Copies `units`; `None` if they do not fit together with a terminator
    /// or contain a NUL.
    pub fn from_units(units: &[u16]) -> Option<Self> {
        let mut s = Self::new();
        s.push_units(units).then_some(s)
    }

    /// Converts an ASCII string; `None` if it does not fit or is not ASCII.
    pub fn from_ascii(text: &str) -> Option<Self> {
        let mut s = Self::new();
        (text.is_ascii() && s.push_ascii(text)).then_some(s)
    }

    fn push_units(&mut self, units: &[u16]) -> bool {
        if units.contains(&0) || self.len + units.len() + 1 > N {
            return false;
        }
        self.units[self.len..self.len + units.len()].copy_from_slice(units);
        self.len += units.len();
        true
    }

    fn push_ascii(&mut self, text: &str) -> bool {
        text.bytes().all(|b| self.push_units(&[u16::from(b)]))
    }

    fn push_decimal(&mut self, mut value: u32) -> bool {
        let mut digits = [0u16; 10];
        let mut n = 0;
        loop {
            digits[n] = u16::from(b'0') + (value % 10) as u16;
            n += 1;
            value /= 10;
            if value == 0 {
                break;
            }
        }
        digits[..n].reverse();
        self.push_units(&digits[..n])
    }

    /// The string without its terminator.
    pub fn as_units(&self) -> &[u16] {
        &self.units[..self.len]
    }

    /// The string with its terminating NUL.
    pub fn as_units_with_nul(&self) -> &[u16] {
        &self.units[..self.len + 1]
    }

    pub fn len(&self) -> usize {
        self.len
    }

    pub fn is_empty(&self) -> bool {
        self.len == 0
    }
}

impl<const N: usize> Default for WideString<N> {
    fn default() -> Self {
        Self::new()
    }
}

impl<const N: usize> core::fmt::Debug for WideString<N> {
    fn fmt(&self, f: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
        f.write_str("\"")?;
        for c in char::decode_utf16(self.as_units().iter().copied()) {
            let c = c.unwrap_or(char::REPLACEMENT_CHARACTER);
            write!(f, "{}", c.escape_debug())?;
        }
        f.write_str("\"")
    }
}

/// An endpoint id or name: up to 63 units plus terminator.
pub type NameString = WideString<NAME_UNITS>;

/// Capacity for the longest reference string:
/// 63 (id) + `_32` + `_192000` + `_4` + `_topology` + terminator = 88.
pub const REFERENCE_STRING_UNITS: usize = 96;
pub type ReferenceString = WideString<REFERENCE_STRING_UNITS>;

/// Which of an endpoint's two filters a reference string names.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum FilterKind {
    Wave,
    Topology,
}

/// Builds `<id>_<channelCount>_<sampleRate>_<sampleSize>`, with `_topology`
/// appended for the topology filter (section 7, A.9).
pub fn reference_string(
    id: &[u16],
    channel_count: u32,
    layout: &Layout,
    kind: FilterKind,
) -> Result<ReferenceString, NtStatus> {
    let mut s = ReferenceString::new();
    let ok = s.push_units(id)
        && s.push_ascii("_")
        && s.push_decimal(channel_count)
        && s.push_ascii("_")
        && s.push_decimal(layout.sample_rate)
        && s.push_ascii("_")
        && s.push_decimal(layout.sample_size)
        && (kind == FilterKind::Wave || s.push_ascii("_topology"));
    if ok {
        Ok(s)
    } else {
        Err(NtStatus::INVALID_PARAMETER)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::layout::LayoutRequest;
    use crate::wire::{put_u16, put_u32};

    fn utf16(s: &str) -> Vec<u16> {
        s.encode_utf16().collect()
    }

    fn raw(ty: u32, index: u32, channels: u32, id: &str, name: &str) -> [u8; 268] {
        let mut b = [0u8; 268];
        put_u32(&mut b, 0, ty);
        put_u32(&mut b, 4, index);
        put_u32(&mut b, 8, channels);
        for (i, u) in id.encode_utf16().enumerate() {
            put_u16(&mut b, 12 + i * 2, u);
        }
        for (i, u) in name.encode_utf16().enumerate() {
            put_u16(&mut b, 140 + i * 2, u);
        }
        b
    }

    fn layout() -> Layout {
        LayoutRequest {
            buffer_size: 1 << 20,
            period_size_bytes: 1920,
            sample_rate: 48_000,
            sample_size: 4,
            minimum_frame_count: 2,
        }
        .validate()
        .unwrap()
    }

    #[test]
    fn parses_fields() {
        let r = EndpointRequest::parse(&raw(2, 7, 2, "Out 1", "Speakers")).unwrap();
        assert_eq!(r.direction, Direction::Playback);
        assert_eq!(r.index, 7);
        assert_eq!(r.channel_count, 2);
        assert_eq!(r.id(), utf16("Out 1").as_slice());
        assert_eq!(r.name(), utf16("Speakers").as_slice());
        let r = EndpointRequest::parse(&raw(1, 0, 32, "", "")).unwrap();
        assert_eq!(r.direction, Direction::Recording);
        assert!(r.id().is_empty());
    }

    #[test]
    fn range_checks() {
        for (ty, index, channels) in [
            (0u32, 0u32, 2u32),
            (3, 0, 2),
            (u32::MAX, 0, 2),
            (1, 2340, 2),
            (1, u32::MAX, 2),
            (2, 0, 0),
            (2, 0, 33),
        ] {
            assert_eq!(
                EndpointRequest::parse(&raw(ty, index, channels, "a", "b")),
                Err(NtStatus::INVALID_PARAMETER),
                "{ty} {index} {channels}"
            );
        }
        assert!(EndpointRequest::parse(&raw(1, 2339, 1, "a", "b")).is_ok());
    }

    #[test]
    fn unit_64_is_always_a_terminator() {
        let long: String = "x".repeat(64);
        let r = EndpointRequest::parse(&raw(2, 0, 2, &long, &long)).unwrap();
        assert_eq!(r.id().len(), 63);
        assert_eq!(r.name().len(), 63);
        // A string that fills all 64 units with no NUL at all is cut at 63
        // and whatever unit 64 held is ignored.
        assert!(r.id().iter().all(|u| *u == u16::from(b'x')));
    }

    #[test]
    fn strings_end_at_first_nul() {
        let mut b = raw(2, 0, 2, "abc", "name");
        // Garbage after the terminator is not part of the string.
        put_u16(&mut b, 12 + 5 * 2, u16::from(b'z'));
        let r = EndpointRequest::parse(&b).unwrap();
        assert_eq!(r.id(), utf16("abc").as_slice());
    }

    #[test]
    fn reference_strings() {
        let l = layout();
        let wave = reference_string(&utf16("My Out"), 2, &l, FilterKind::Wave).unwrap();
        assert_eq!(wave.as_units(), utf16("My Out_2_48000_4").as_slice());
        assert_eq!(*wave.as_units_with_nul().last().unwrap(), 0);
        let topo = reference_string(&utf16("My Out"), 2, &l, FilterKind::Topology).unwrap();
        assert_eq!(topo.as_units(), utf16("My Out_2_48000_4_topology").as_slice());
    }

    #[test]
    fn longest_reference_string_fits() {
        let l = LayoutRequest {
            buffer_size: 1 << 20,
            period_size_bytes: 16,
            sample_rate: 192_000,
            sample_size: 4,
            minimum_frame_count: 2,
        }
        .validate()
        .unwrap();
        let id = utf16(&"i".repeat(63));
        let s = reference_string(&id, 32, &l, FilterKind::Topology).unwrap();
        assert_eq!(s.len(), 63 + 3 + 7 + 2 + 9);
        assert!(s.len() < REFERENCE_STRING_UNITS);
    }

    #[test]
    fn wide_string_limits() {
        assert!(NameString::from_units(&utf16(&"n".repeat(63))).is_some());
        assert!(NameString::from_units(&utf16(&"n".repeat(64))).is_none());
        assert!(NameString::from_units(&[b'a' as u16, 0, b'b' as u16]).is_none());
        let s = NameString::from_units(&utf16("héllo")).unwrap();
        assert_eq!(format!("{s:?}"), "\"héllo\"");
        assert_eq!(s.as_units_with_nul().len(), 6);
        assert!(NameString::new().is_empty());
    }

    #[test]
    fn from_ascii() {
        let s = WideString::<8>::from_ascii("MSEP").unwrap();
        assert_eq!(s.as_units(), utf16("MSEP").as_slice());
        assert!(WideString::<4>::from_ascii("MSEP").is_none());
        assert!(WideString::<8>::from_ascii("é").is_none());
    }

    #[test]
    fn decimal() {
        let mut s = WideString::<16>::new();
        assert!(s.push_decimal(0));
        assert!(s.push_ascii(","));
        assert!(s.push_decimal(4_294_967_295));
        assert_eq!(s.as_units(), utf16("0,4294967295").as_slice());
    }
}
