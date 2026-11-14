// Copyright (c) 2026 Itty Bitty Binaries, Inc.
// SPDX-License-Identifier: MIT

//! Audio formats: the endpoint's default format, the format a stream may be
//! opened with, and format matching (sections 7 and 8, A.9).
//!
//! Formats are handled as bytes in the layout Windows gives them:
//! a 64-byte `KSDATAFORMAT` followed by a 40-byte `WAVEFORMATEXTENSIBLE`.
#![forbid(unsafe_code)]

use ntstatus::NtStatus;

use crate::wire::{get_guid, get_u16, get_u32, guid, put_u16, put_u32, GuidBytes};

/// `KSDATAFORMAT_TYPE_AUDIO`, {73647561-0000-0010-8000-00AA00389B71}.
pub const TYPE_AUDIO: GuidBytes = guid(
    0x7364_7561,
    0x0000,
    0x0010,
    [0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71],
);
/// `KSDATAFORMAT_SUBTYPE_PCM`, {00000001-0000-0010-8000-00AA00389B71}.
pub const SUBTYPE_PCM: GuidBytes = guid(
    0x0000_0001,
    0x0000,
    0x0010,
    [0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71],
);
/// `KSDATAFORMAT_SPECIFIER_WAVEFORMATEX`, {05589F81-C356-11CE-BF01-00AA0055595A}.
pub const SPECIFIER_WAVEFORMATEX: GuidBytes = guid(
    0x0558_9F81,
    0xC356,
    0x11CE,
    [0xBF, 0x01, 0x00, 0xAA, 0x00, 0x55, 0x59, 0x5A],
);

/// `WAVE_FORMAT_EXTENSIBLE`.
pub const WAVE_FORMAT_EXTENSIBLE: u16 = 0xFFFE;

/// Size of `KSDATAFORMAT` / `KSDATARANGE`.
pub const DATA_FORMAT_SIZE: usize = 64;
/// Size of `KSDATAFORMAT` + `WAVEFORMATEXTENSIBLE`.
pub const WAVE_FORMAT_SIZE: usize = 104;
/// Size of `KSDATARANGE_AUDIO`.
pub const DATA_RANGE_AUDIO_SIZE: usize = 88;

// KSDATAFORMAT
const OFF_FORMAT_SIZE: usize = 0;
const OFF_SAMPLE_SIZE: usize = 8;
const OFF_MAJOR: usize = 16;
const OFF_SUB: usize = 32;
const OFF_SPECIFIER: usize = 48;
// WAVEFORMATEXTENSIBLE, after the KSDATAFORMAT
const OFF_FORMAT_TAG: usize = 64;
const OFF_CHANNELS: usize = 66;
const OFF_SAMPLES_PER_SEC: usize = 68;
const OFF_AVG_BYTES_PER_SEC: usize = 72;
const OFF_BLOCK_ALIGN: usize = 76;
const OFF_BITS_PER_SAMPLE: usize = 78;
const OFF_CB_SIZE: usize = 80;
const OFF_VALID_BITS: usize = 82;
const OFF_CHANNEL_MASK: usize = 84;
const OFF_EXT_SUBFORMAT: usize = 88;
// KSDATARANGE_AUDIO, after the KSDATARANGE
const OFF_RANGE_MAX_CHANNELS: usize = 64;
const OFF_RANGE_MIN_BITS: usize = 68;
const OFF_RANGE_MAX_BITS: usize = 72;
const OFF_RANGE_MIN_RATE: usize = 76;
const OFF_RANGE_MAX_RATE: usize = 80;

/// What an endpoint can carry: the layout's rate and sample size, and the
/// endpoint's channel count.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct EndpointFormat {
    pub sample_rate: u32,
    /// Bytes per sample, 1 to 4.
    pub sample_size: u32,
    /// 1 to 32.
    pub channel_count: u32,
}

impl EndpointFormat {
    pub fn bits_per_sample(&self) -> u32 {
        self.sample_size * 8
    }
}

/// Channels of the default format: the number of speakers in the mask, or
/// the endpoint's channel count when the mask is zero.
pub fn default_channels(speaker_mask: u32, channel_count: u32) -> u32 {
    if speaker_mask == 0 {
        channel_count
    } else {
        speaker_mask.count_ones()
    }
}

/// Whether a new speaker mask may be stored: it must name no more speakers
/// than the endpoint has channels.
pub fn speaker_mask_allowed(speaker_mask: u32, channel_count: u32) -> bool {
    speaker_mask.count_ones() <= channel_count
}

/// Builds extensible PCM. The arguments are small by construction (rate at
/// most 192,000, at most 32 bits, at most 32 channels), so nothing here can
/// overflow; saturating arithmetic keeps that from being a hidden assumption.
fn build_format(sample_rate: u32, bits: u32, channels: u32, channel_mask: u32) -> [u8; WAVE_FORMAT_SIZE] {
    let block_align = (bits / 8).saturating_mul(channels);
    let mut f = [0u8; WAVE_FORMAT_SIZE];
    put_u32(&mut f, OFF_FORMAT_SIZE, WAVE_FORMAT_SIZE as u32);
    // Flags (4) and Reserved (12) stay zero.
    put_u32(&mut f, OFF_SAMPLE_SIZE, block_align);
    f[OFF_MAJOR..OFF_MAJOR + 16].copy_from_slice(&TYPE_AUDIO);
    f[OFF_SUB..OFF_SUB + 16].copy_from_slice(&SUBTYPE_PCM);
    f[OFF_SPECIFIER..OFF_SPECIFIER + 16].copy_from_slice(&SPECIFIER_WAVEFORMATEX);
    put_u16(&mut f, OFF_FORMAT_TAG, WAVE_FORMAT_EXTENSIBLE);
    put_u16(&mut f, OFF_CHANNELS, u16::try_from(channels).unwrap_or(u16::MAX));
    put_u32(&mut f, OFF_SAMPLES_PER_SEC, sample_rate);
    put_u32(&mut f, OFF_AVG_BYTES_PER_SEC, block_align.saturating_mul(sample_rate));
    put_u16(&mut f, OFF_BLOCK_ALIGN, u16::try_from(block_align).unwrap_or(u16::MAX));
    put_u16(&mut f, OFF_BITS_PER_SAMPLE, u16::try_from(bits).unwrap_or(u16::MAX));
    put_u16(&mut f, OFF_CB_SIZE, 22);
    put_u16(&mut f, OFF_VALID_BITS, u16::try_from(bits).unwrap_or(u16::MAX));
    put_u32(&mut f, OFF_CHANNEL_MASK, channel_mask);
    f[OFF_EXT_SUBFORMAT..OFF_EXT_SUBFORMAT + 16].copy_from_slice(&SUBTYPE_PCM);
    f
}

/// The endpoint's default format (A.9): extensible PCM at the layout's rate
/// and depth, with the mask's speakers (or all channels when the mask is 0).
pub fn default_format(endpoint: &EndpointFormat, speaker_mask: u32) -> [u8; WAVE_FORMAT_SIZE] {
    build_format(
        endpoint.sample_rate,
        endpoint.bits_per_sample(),
        default_channels(speaker_mask, endpoint.channel_count),
        speaker_mask,
    )
}

/// Checks a format a client wants to open a stream with, or proposes.
///
/// Accepted: audio, PCM, `WAVEFORMATEX` specifier, extensible tag, PCM
/// extensible subformat, exactly the layout's rate and depth, and between 1
/// and the endpoint's channel count channels. Returns the stream's channel
/// count, or `None` (the caller answers `STATUS_NO_MATCH`).
///
/// `format` is whatever bytes the caller supplied; anything shorter than the
/// 104-byte structure cannot be extensible PCM.
pub fn accepted_channels(format: &[u8], endpoint: &EndpointFormat) -> Option<u32> {
    if format.len() < WAVE_FORMAT_SIZE {
        return None;
    }
    let channels = u32::from(get_u16(format, OFF_CHANNELS));
    let ok = get_guid(format, OFF_MAJOR) == TYPE_AUDIO
        && get_guid(format, OFF_SUB) == SUBTYPE_PCM
        && get_guid(format, OFF_SPECIFIER) == SPECIFIER_WAVEFORMATEX
        && get_u16(format, OFF_FORMAT_TAG) == WAVE_FORMAT_EXTENSIBLE
        && get_guid(format, OFF_EXT_SUBFORMAT) == SUBTYPE_PCM
        && get_u32(format, OFF_SAMPLES_PER_SEC) == endpoint.sample_rate
        && u32::from(get_u16(format, OFF_BITS_PER_SAMPLE)) == endpoint.bits_per_sample()
        && (1..=endpoint.channel_count).contains(&channels);
    ok.then_some(channels)
}

/// Format matching (section 7): the format lying in both the streaming pin's
/// range and `caller_range`, a `KSDATARANGE_AUDIO` offered by the caller.
///
/// `STATUS_NO_MATCH` if the caller's range is not an audio range (too short
/// to be one) or does not contain the layout's rate and depth.
pub fn intersect(
    caller_range: &[u8],
    endpoint: &EndpointFormat,
    speaker_mask: u32,
) -> Result<[u8; WAVE_FORMAT_SIZE], NtStatus> {
    if caller_range.len() < DATA_RANGE_AUDIO_SIZE
        || (get_u32(caller_range, OFF_FORMAT_SIZE) as usize) < DATA_RANGE_AUDIO_SIZE
    {
        return Err(NtStatus::NO_MATCH);
    }
    let max_channels = get_u32(caller_range, OFF_RANGE_MAX_CHANNELS);
    let min_bits = get_u32(caller_range, OFF_RANGE_MIN_BITS);
    let max_bits = get_u32(caller_range, OFF_RANGE_MAX_BITS);
    let min_rate = get_u32(caller_range, OFF_RANGE_MIN_RATE);
    let max_rate = get_u32(caller_range, OFF_RANGE_MAX_RATE);

    let bits = endpoint.bits_per_sample();
    let rate = endpoint.sample_rate;
    // The pin's own range is the single point (rate, bits).
    if !(min_rate..=max_rate).contains(&rate) || !(min_bits..=max_bits).contains(&bits) {
        return Err(NtStatus::NO_MATCH);
    }
    let channels = default_channels(speaker_mask, endpoint.channel_count).min(max_channels);
    if channels == 0 {
        // A range that admits no channel has no format in it.
        return Err(NtStatus::NO_MATCH);
    }
    Ok(build_format(rate.min(max_rate), bits.min(max_bits), channels, 0))
}

/// The streaming pin's data range: a `KSDATARANGE_AUDIO` (88 bytes) field by
/// field, for building the pin descriptor.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct PinAudioRange {
    pub maximum_channels: u32,
    pub minimum_bits_per_sample: u32,
    pub maximum_bits_per_sample: u32,
    pub minimum_sample_frequency: u32,
    pub maximum_sample_frequency: u32,
}

impl PinAudioRange {
    pub fn of(endpoint: &EndpointFormat) -> Self {
        Self {
            maximum_channels: endpoint.channel_count,
            minimum_bits_per_sample: endpoint.bits_per_sample(),
            maximum_bits_per_sample: endpoint.bits_per_sample(),
            minimum_sample_frequency: endpoint.sample_rate,
            maximum_sample_frequency: endpoint.sample_rate,
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    const EP: EndpointFormat = EndpointFormat { sample_rate: 48_000, sample_size: 3, channel_count: 6 };

    fn range(max_ch: u32, bits: (u32, u32), rate: (u32, u32)) -> [u8; 88] {
        let mut r = [0u8; 88];
        put_u32(&mut r, 0, 88);
        r[16..32].copy_from_slice(&TYPE_AUDIO);
        r[32..48].copy_from_slice(&SUBTYPE_PCM);
        r[48..64].copy_from_slice(&SPECIFIER_WAVEFORMATEX);
        put_u32(&mut r, 64, max_ch);
        put_u32(&mut r, 68, bits.0);
        put_u32(&mut r, 72, bits.1);
        put_u32(&mut r, 76, rate.0);
        put_u32(&mut r, 80, rate.1);
        r
    }

    #[test]
    fn default_format_fields_match_a9() {
        let f = default_format(&EP, 0);
        assert_eq!(get_u32(&f, 0), 104, "format size");
        assert_eq!(get_u32(&f, 4), 0, "flags");
        assert_eq!(get_u32(&f, 8), 18, "sample size = block alignment");
        assert_eq!(get_guid(&f, 16), TYPE_AUDIO);
        assert_eq!(get_guid(&f, 32), SUBTYPE_PCM);
        assert_eq!(get_guid(&f, 48), SPECIFIER_WAVEFORMATEX);
        assert_eq!(get_u16(&f, 64), 0xFFFE);
        assert_eq!(get_u16(&f, 66), 6);
        assert_eq!(get_u32(&f, 68), 48_000);
        assert_eq!(get_u32(&f, 72), 18 * 48_000);
        assert_eq!(get_u16(&f, 76), 18);
        assert_eq!(get_u16(&f, 78), 24);
        assert_eq!(get_u16(&f, 80), 22);
        assert_eq!(get_u16(&f, 82), 24);
        assert_eq!(get_u32(&f, 84), 0);
        assert_eq!(get_guid(&f, 88), SUBTYPE_PCM);
    }

    #[test]
    fn default_format_uses_the_mask_when_set() {
        // Front left | front right = 0x3
        let f = default_format(&EP, 0x3);
        assert_eq!(get_u16(&f, 66), 2);
        assert_eq!(get_u32(&f, 84), 0x3);
        assert_eq!(get_u16(&f, 76), 6);
        assert_eq!(default_channels(0, 6), 6);
        assert_eq!(default_channels(0x3F, 6), 6);
        assert_eq!(default_channels(0x8000_0000, 6), 1);
    }

    #[test]
    fn default_format_is_accepted() {
        assert_eq!(accepted_channels(&default_format(&EP, 0), &EP), Some(6));
        assert_eq!(accepted_channels(&default_format(&EP, 0x3), &EP), Some(2));
    }

    #[test]
    fn fewer_channels_are_accepted_more_are_not() {
        let mut f = default_format(&EP, 0);
        put_u16(&mut f, 66, 1);
        assert_eq!(accepted_channels(&f, &EP), Some(1));
        put_u16(&mut f, 66, 7);
        assert_eq!(accepted_channels(&f, &EP), None);
        put_u16(&mut f, 66, 0);
        assert_eq!(accepted_channels(&f, &EP), None);
    }

    #[test]
    fn every_listed_criterion_is_checked() {
        let good = default_format(&EP, 0);
        let mutations: &[(usize, u8)] = &[
            (16, 0xFF), // major format
            (32, 0x03), // subformat (3 = IEEE float)
            (48, 0xFF), // specifier
            (64, 0x01), // format tag (plain PCM, not extensible)
            (88, 0x03), // extensible subformat
            (68, 0x44), // sample rate
            (78, 16),   // bits per sample
        ];
        for (offset, value) in mutations {
            let mut f = good;
            f[*offset] = *value;
            assert_eq!(accepted_channels(&f, &EP), None, "offset {offset}");
        }
    }

    #[test]
    fn short_formats_are_refused() {
        let f = default_format(&EP, 0);
        assert_eq!(accepted_channels(&f[..103], &EP), None);
        assert_eq!(accepted_channels(&f[..64], &EP), None);
        assert_eq!(accepted_channels(&[], &EP), None);
        // Trailing bytes are ignored.
        let mut long = f.to_vec();
        long.extend_from_slice(&[0xEE; 20]);
        assert_eq!(accepted_channels(&long, &EP), Some(6));
    }

    #[test]
    fn intersect_picks_layout_values_inside_the_callers_range() {
        let f = intersect(&range(u32::MAX, (8, 32), (8_000, 192_000)), &EP, 0).unwrap();
        assert_eq!(get_u16(&f, 66), 6);
        assert_eq!(get_u32(&f, 68), 48_000);
        assert_eq!(get_u16(&f, 78), 24);
        assert_eq!(get_u32(&f, 84), 0, "no speaker positions");
        assert_eq!(get_u32(&f, 8), 18);
        // The result is a format the pin can be opened with.
        assert_eq!(accepted_channels(&f, &EP), Some(6));
    }

    #[test]
    fn intersect_limits_channels_to_the_callers_maximum() {
        let f = intersect(&range(2, (24, 24), (48_000, 48_000)), &EP, 0).unwrap();
        assert_eq!(get_u16(&f, 66), 2);
        assert_eq!(get_u16(&f, 76), 6);
        assert_eq!(get_u32(&f, 72), 6 * 48_000);
    }

    #[test]
    fn intersect_uses_the_mask_for_channels_but_reports_no_positions() {
        let f = intersect(&range(8, (24, 24), (48_000, 48_000)), &EP, 0x3).unwrap();
        assert_eq!(get_u16(&f, 66), 2);
        assert_eq!(get_u32(&f, 84), 0);
    }

    #[test]
    fn intersect_refuses_disjoint_ranges() {
        for r in [
            range(2, (16, 16), (48_000, 48_000)),
            range(2, (32, 32), (48_000, 48_000)),
            range(2, (24, 24), (44_100, 44_100)),
            range(2, (24, 24), (96_000, 192_000)),
            range(0, (24, 24), (48_000, 48_000)),
        ] {
            assert_eq!(intersect(&r, &EP, 0), Err(NtStatus::NO_MATCH));
        }
    }

    #[test]
    fn intersect_refuses_non_audio_ranges() {
        let r = range(2, (24, 24), (48_000, 48_000));
        // A plain 64-byte KSDATARANGE.
        assert_eq!(intersect(&r[..64], &EP, 0), Err(NtStatus::NO_MATCH));
        let mut short = r;
        put_u32(&mut short, 0, 64);
        assert_eq!(intersect(&short, &EP, 0), Err(NtStatus::NO_MATCH));
        assert_eq!(intersect(&[], &EP, 0), Err(NtStatus::NO_MATCH));
    }

    #[test]
    fn speaker_masks() {
        assert!(speaker_mask_allowed(0, 1));
        assert!(speaker_mask_allowed(0x3, 2));
        assert!(!speaker_mask_allowed(0x7, 2));
        assert!(speaker_mask_allowed(0x3F, 6));
        assert!(!speaker_mask_allowed(u32::MAX, 31));
        assert!(speaker_mask_allowed(u32::MAX, 32));
    }

    #[test]
    fn pin_range() {
        let r = PinAudioRange::of(&EP);
        assert_eq!(r.maximum_channels, 6);
        assert_eq!((r.minimum_bits_per_sample, r.maximum_bits_per_sample), (24, 24));
        assert_eq!((r.minimum_sample_frequency, r.maximum_sample_frequency), (48_000, 48_000));
    }
}
