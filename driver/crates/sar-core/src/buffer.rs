// Copyright (c) 2026 Itty Bitty Binaries, Inc.
// SPDX-License-Identifier: MIT

//! Size of a stream's buffer (section 8, "Allocating the buffer").
#![forbid(unsafe_code)]

use ntstatus::NtStatus;

use crate::layout::{Layout, CELL_SIZE};

/// The outcome of the size calculation for one buffer request.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct BufferGeometry {
    /// Size of the buffer in bytes, a whole number of frames. This is what
    /// the client is told and what goes into the register record.
    pub size: u32,
    /// Cells set aside for it: `size` rounded up to whole cells.
    pub cells: u32,
}

/// Works out the buffer for a stream of `channels` channels when the client
/// asked for `requested` bytes.
///
/// The size is the larger of the request and
/// `minimumFrameCount * periodSizeBytes * channels`, rounded up to whole
/// frames. No step wraps: anything that does not fit is refused.
///
/// Errors: `STATUS_INVALID_PARAMETER` if the buffer would be empty (or the
/// stream has no channels), `STATUS_INSUFFICIENT_RESOURCES` if it cannot fit
/// in the audio area.
pub fn stream_buffer_geometry(
    layout: &Layout,
    channels: u32,
    requested: u32,
) -> Result<BufferGeometry, NtStatus> {
    if channels == 0 {
        return Err(NtStatus::INVALID_PARAMETER);
    }
    let too_large = NtStatus::INSUFFICIENT_RESOURCES;

    let frame = u64::from(layout.sample_size)
        .checked_mul(u64::from(channels))
        .ok_or(too_large)?;
    let minimum = u64::from(layout.minimum_frame_count)
        .checked_mul(u64::from(layout.period_size_bytes))
        .and_then(|v| v.checked_mul(u64::from(channels)))
        .ok_or(too_large)?;

    let wanted = minimum.max(u64::from(requested));
    if wanted == 0 {
        return Err(NtStatus::INVALID_PARAMETER);
    }
    let size = wanted
        .checked_add(frame - 1)
        .map(|v| v / frame * frame)
        .ok_or(too_large)?;
    if size > u64::from(layout.audio_area_size()) {
        return Err(too_large);
    }
    // size <= 2^27 from here on.
    let size = u32::try_from(size).map_err(|_| too_large)?;
    let cells = size.div_ceil(CELL_SIZE);
    if cells > layout.usable_cells() {
        return Err(too_large);
    }
    Ok(BufferGeometry { size, cells })
}

/// The accuracy reported for the position register: one period of the
/// stream, in bytes (`periodSizeBytes * channels`), saturating.
pub fn position_accuracy(layout: &Layout, channels: u32) -> u32 {
    layout.period_size_bytes.saturating_mul(channels)
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::layout::{LayoutRequest, MAX_BUFFER_SIZE};

    fn layout(buffer: u32, period: u32, sample: u32, min_frames: u32) -> Layout {
        LayoutRequest {
            buffer_size: buffer,
            period_size_bytes: period,
            sample_rate: 48_000,
            sample_size: sample,
            minimum_frame_count: min_frames,
        }
        .validate()
        .unwrap()
    }

    #[test]
    fn minimum_applies_when_request_is_smaller() {
        // period 480 samples * 4 bytes, 2 periods minimum, stereo.
        let l = layout(1 << 20, 1920, 4, 2);
        let g = stream_buffer_geometry(&l, 2, 100).unwrap();
        assert_eq!(g.size, 2 * 1920 * 2);
        assert_eq!(g.cells, 1);
    }

    #[test]
    fn request_applies_when_larger_and_is_rounded_to_frames() {
        let l = layout(1 << 20, 1920, 3, 2);
        // frame = 3 bytes * 2 channels = 6
        let g = stream_buffer_geometry(&l, 2, 100_001).unwrap();
        assert_eq!(g.size, 100_002);
        assert_eq!(g.size % 6, 0);
        assert_eq!(g.cells, 2);
        // Already a multiple: unchanged.
        assert_eq!(stream_buffer_geometry(&l, 2, 65_536 * 3 / 6 * 6).unwrap().size, 196_608);
    }

    #[test]
    fn cells_round_up() {
        let l = layout(1 << 20, 1, 1, 0);
        assert_eq!(stream_buffer_geometry(&l, 1, 65_536).unwrap().cells, 1);
        assert_eq!(stream_buffer_geometry(&l, 1, 65_537).unwrap().cells, 2);
        assert_eq!(stream_buffer_geometry(&l, 1, 1 << 20).unwrap().cells, 16);
    }

    #[test]
    fn empty_buffer_is_invalid() {
        let l = layout(1 << 20, 1920, 4, 0);
        assert_eq!(stream_buffer_geometry(&l, 2, 0), Err(NtStatus::INVALID_PARAMETER));
        assert_eq!(stream_buffer_geometry(&l, 0, 4096), Err(NtStatus::INVALID_PARAMETER));
        // One byte asked for becomes one frame.
        assert_eq!(stream_buffer_geometry(&l, 2, 1).unwrap().size, 8);
    }

    #[test]
    fn too_large_is_refused_not_wrapped() {
        let l = layout(1 << 20, 1920, 4, 2);
        assert_eq!(
            stream_buffer_geometry(&l, 2, (1 << 20) + 1),
            Err(NtStatus::INSUFFICIENT_RESOURCES)
        );
        assert_eq!(
            stream_buffer_geometry(&l, 2, u32::MAX),
            Err(NtStatus::INSUFFICIENT_RESOURCES)
        );
        // minimumFrameCount * period * channels overflows 32 bits and, at the
        // extreme, 64 bits.
        let l = layout(MAX_BUFFER_SIZE, MAX_BUFFER_SIZE, 4, u32::MAX);
        assert_eq!(
            stream_buffer_geometry(&l, 32, 16),
            Err(NtStatus::INSUFFICIENT_RESOURCES)
        );
        let l = layout(1 << 20, 1 << 16, 4, 1 << 16);
        assert_eq!(
            stream_buffer_geometry(&l, 1, 16),
            Err(NtStatus::INSUFFICIENT_RESOURCES)
        );
    }

    #[test]
    fn audio_area_smaller_than_a_cell_holds_nothing() {
        let l = layout(4096, 16, 2, 1);
        assert_eq!(l.usable_cells(), 0);
        assert_eq!(
            stream_buffer_geometry(&l, 1, 64),
            Err(NtStatus::INSUFFICIENT_RESOURCES)
        );
    }

    #[test]
    fn trailing_partial_cell_is_unusable() {
        // 1.5 cells of audio area: a buffer of more than one cell does not fit
        // even though the area is large enough in bytes.
        let l = layout(65_536 + 32_768, 16, 2, 1);
        assert_eq!(l.usable_cells(), 1);
        assert!(stream_buffer_geometry(&l, 1, 65_536).is_ok());
        assert_eq!(
            stream_buffer_geometry(&l, 1, 65_538),
            Err(NtStatus::INSUFFICIENT_RESOURCES)
        );
    }

    #[test]
    fn accuracy() {
        let l = layout(1 << 20, 1920, 4, 2);
        assert_eq!(position_accuracy(&l, 2), 3840);
        let l = layout(MAX_BUFFER_SIZE, MAX_BUFFER_SIZE, 4, 2);
        assert_eq!(position_accuracy(&l, 32), u32::MAX);
    }
}
