// Copyright (c) 2026 Itty Bitty Binaries, Inc.
// SPDX-License-Identifier: MIT

//! The buffer layout request and the geometry of the shared section
//! (sections 5, 6, 12, A.5, A.6).
#![forbid(unsafe_code)]

use ntstatus::NtStatus;

use crate::wire::{get_u32, put_u32, put_u64};

/// The memory page size the section's audio area is rounded up to (A.6).
pub const PAGE_SIZE: u32 = 4096;
/// The cell size (A.6). Also the allocation granularity views are aligned to.
pub const CELL_SIZE: u32 = 65_536;
/// Largest audio area a host may ask for: 128 MiB.
pub const MAX_BUFFER_SIZE: u32 = 134_217_728;
/// Size of one register record (A.7).
pub const REGISTER_RECORD_SIZE: u32 = 28;
/// Records in the register table, and so endpoints per control handle.
pub const MAX_ENDPOINTS: u32 = CELL_SIZE / REGISTER_RECORD_SIZE;
/// Cells in the largest audio area.
pub const MAX_CELLS: u32 = MAX_BUFFER_SIZE / CELL_SIZE;

pub const MIN_SAMPLE_RATE: u32 = 8_000;
pub const MAX_SAMPLE_RATE: u32 = 192_000;
pub const MIN_SAMPLE_SIZE: u32 = 1;
pub const MAX_SAMPLE_SIZE: u32 = 4;

/// The "set buffer layout" input structure (A.5), 20 bytes.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct LayoutRequest {
    pub buffer_size: u32,
    pub period_size_bytes: u32,
    pub sample_rate: u32,
    pub sample_size: u32,
    pub minimum_frame_count: u32,
}

impl LayoutRequest {
    /// Size of the input structure.
    pub const SIZE: usize = 20;

    pub fn parse(bytes: &[u8; Self::SIZE]) -> Self {
        Self {
            buffer_size: get_u32(bytes, 0),
            period_size_bytes: get_u32(bytes, 4),
            sample_rate: get_u32(bytes, 8),
            sample_size: get_u32(bytes, 12),
            minimum_frame_count: get_u32(bytes, 16),
        }
    }

    /// Applies the range checks of section 5 and derives the geometry.
    pub fn validate(&self) -> Result<Layout, NtStatus> {
        let ok = (1..=MAX_BUFFER_SIZE).contains(&self.buffer_size)
            && (MIN_SAMPLE_SIZE..=MAX_SAMPLE_SIZE).contains(&self.sample_size)
            && (MIN_SAMPLE_RATE..=MAX_SAMPLE_RATE).contains(&self.sample_rate)
            && (1..=self.buffer_size).contains(&self.period_size_bytes);
        if !ok {
            return Err(NtStatus::INVALID_PARAMETER);
        }
        // buffer_size <= 2^27, so rounding up to a page cannot overflow.
        let audio_area_size = self.buffer_size.div_ceil(PAGE_SIZE) * PAGE_SIZE;
        Ok(Layout {
            period_size_bytes: self.period_size_bytes,
            sample_rate: self.sample_rate,
            sample_size: self.sample_size,
            minimum_frame_count: self.minimum_frame_count,
            audio_area_size,
        })
    }
}

/// A validated buffer layout: the audio format shared by all endpoints of a
/// control handle, and the section's geometry.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Layout {
    pub period_size_bytes: u32,
    pub sample_rate: u32,
    pub sample_size: u32,
    pub minimum_frame_count: u32,
    audio_area_size: u32,
}

impl Layout {
    /// Bytes of audio area: `bufferSize` rounded up to whole pages.
    pub fn audio_area_size(&self) -> u32 {
        self.audio_area_size
    }

    /// Offset of the register table in the section.
    pub fn register_base(&self) -> u32 {
        self.audio_area_size
    }

    /// Size of the whole section: audio area plus one cell of registers.
    pub fn section_size(&self) -> u32 {
        // At most 2^27 + 2^16.
        self.audio_area_size + CELL_SIZE
    }

    /// Whole cells in the audio area; a trailing partial cell is unusable.
    pub fn usable_cells(&self) -> u32 {
        self.audio_area_size / CELL_SIZE
    }

    /// Bits per sample.
    pub fn bits_per_sample(&self) -> u32 {
        self.sample_size * 8
    }

    /// Where a view that covers the register table has to start. Views of a
    /// section begin on a 64 KiB boundary, and `register_base` is only
    /// page-aligned, so the view starts at the boundary at or below it.
    pub fn register_view_offset(&self) -> u32 {
        self.register_base() - self.register_view_delta()
    }

    /// Distance from the start of the register view to the register table.
    pub fn register_view_delta(&self) -> u32 {
        self.register_base() % CELL_SIZE
    }

    /// Length of the register view: the lead-in plus the register cell.
    pub fn register_view_size(&self) -> u32 {
        self.register_view_delta() + CELL_SIZE
    }

    /// Section offset of cell `k`.
    pub fn cell_offset(&self, cell: u32) -> u32 {
        debug_assert!(cell < self.usable_cells());
        cell * CELL_SIZE
    }
}

/// The "set buffer layout" output structure (A.5), 16 bytes.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct LayoutReply {
    pub virtual_address: u64,
    pub actual_size: u32,
    pub register_base: u32,
}

impl LayoutReply {
    pub const SIZE: usize = 16;

    pub fn new(layout: &Layout, virtual_address: u64) -> Self {
        Self {
            virtual_address,
            actual_size: layout.section_size(),
            register_base: layout.register_base(),
        }
    }

    pub fn encode(&self) -> [u8; Self::SIZE] {
        let mut out = [0u8; Self::SIZE];
        put_u64(&mut out, 0, self.virtual_address);
        put_u32(&mut out, 8, self.actual_size);
        put_u32(&mut out, 12, self.register_base);
        out
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn request() -> LayoutRequest {
        LayoutRequest {
            buffer_size: 1 << 20,
            period_size_bytes: 480 * 2,
            sample_rate: 48_000,
            sample_size: 2,
            minimum_frame_count: 2,
        }
    }

    #[test]
    fn parse_reads_fields_in_order() {
        let mut raw = [0u8; 20];
        for (i, v) in [1u32, 2, 3, 4, 5].iter().enumerate() {
            put_u32(&mut raw, i * 4, *v);
        }
        assert_eq!(
            LayoutRequest::parse(&raw),
            LayoutRequest {
                buffer_size: 1,
                period_size_bytes: 2,
                sample_rate: 3,
                sample_size: 4,
                minimum_frame_count: 5
            }
        );
    }

    #[test]
    fn constants() {
        assert_eq!(MAX_ENDPOINTS, 2340);
        assert_eq!(CELL_SIZE - MAX_ENDPOINTS * REGISTER_RECORD_SIZE, 16);
        assert_eq!(MAX_CELLS, 2048);
    }

    #[test]
    fn accepts_the_documented_ranges() {
        assert!(request().validate().is_ok());
        for (size, rate, bytes) in [
            (1u32, 8_000u32, 1u32),
            (MAX_BUFFER_SIZE, 192_000, 4),
        ] {
            let r = LayoutRequest {
                buffer_size: size,
                period_size_bytes: size,
                sample_rate: rate,
                sample_size: bytes,
                minimum_frame_count: u32::MAX,
            };
            assert!(r.validate().is_ok(), "{r:?}");
        }
    }

    #[test]
    fn rejects_out_of_range_parameters() {
        let bad = [
            LayoutRequest { buffer_size: 0, ..request() },
            LayoutRequest { buffer_size: MAX_BUFFER_SIZE + 1, ..request() },
            LayoutRequest { sample_size: 0, ..request() },
            LayoutRequest { sample_size: 5, ..request() },
            LayoutRequest { sample_rate: 7_999, ..request() },
            LayoutRequest { sample_rate: 192_001, ..request() },
            LayoutRequest { period_size_bytes: 0, ..request() },
            LayoutRequest { period_size_bytes: (1 << 20) + 1, ..request() },
            LayoutRequest { buffer_size: u32::MAX, period_size_bytes: 1, ..request() },
        ];
        for r in bad {
            assert_eq!(r.validate(), Err(NtStatus::INVALID_PARAMETER), "{r:?}");
        }
    }

    #[test]
    fn geometry_follows_a6() {
        let l = LayoutRequest { buffer_size: 1, period_size_bytes: 1, ..request() }
            .validate()
            .unwrap();
        assert_eq!(l.audio_area_size(), 4096);
        assert_eq!(l.register_base(), 4096);
        assert_eq!(l.section_size(), 4096 + 65_536);
        assert_eq!(l.usable_cells(), 0);
        assert_eq!(l.register_view_offset(), 0);
        assert_eq!(l.register_view_delta(), 4096);
        assert_eq!(l.register_view_size(), 4096 + 65_536);

        let l = LayoutRequest { buffer_size: 65_536 * 3 + 1, ..request() }
            .validate()
            .unwrap();
        assert_eq!(l.audio_area_size(), 65_536 * 3 + 4096);
        assert_eq!(l.usable_cells(), 3);
        assert_eq!(l.register_view_offset(), 65_536 * 3);
        assert_eq!(l.register_view_delta(), 4096);

        let l = request().validate().unwrap();
        assert_eq!(l.audio_area_size(), 1 << 20);
        assert_eq!(l.usable_cells(), 16);
        assert_eq!(l.register_view_offset(), 1 << 20);
        assert_eq!(l.register_view_delta(), 0);
        assert_eq!(l.register_view_size(), 65_536);
        assert_eq!(l.cell_offset(3), 3 * 65_536);

        let l = LayoutRequest { buffer_size: MAX_BUFFER_SIZE, ..request() }
            .validate()
            .unwrap();
        assert_eq!(l.section_size(), MAX_BUFFER_SIZE + 65_536);
        assert_eq!(l.usable_cells(), 2048);
    }

    #[test]
    fn reply_layout() {
        let l = request().validate().unwrap();
        let bytes = LayoutReply::new(&l, 0x0000_7FFE_1234_0000).encode();
        assert_eq!(&bytes[0..8], &0x0000_7FFE_1234_0000u64.to_le_bytes());
        assert_eq!(&bytes[8..12], &((1u32 << 20) + 65_536).to_le_bytes());
        assert_eq!(&bytes[12..16], &(1u32 << 20).to_le_bytes());
    }
}
