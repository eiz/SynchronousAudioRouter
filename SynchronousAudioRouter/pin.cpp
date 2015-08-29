// SynchronousAudioRouter
// Copyright (C) 2015 Mackenzie Straight
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with SynchronousAudioRouter.  If not, see <http://www.gnu.org/licenses/>.

#include "sar.h"

NTSTATUS SarKsPinSetDataFormat(
    PKSPIN pin,
    PKSDATAFORMAT oldFormat,
    PKSMULTIPLE_ITEM oldAttributeList,
    const KSDATARANGE *dataRange,
    const KSATTRIBUTE_LIST *attributeRange)
{
    UNREFERENCED_PARAMETER(pin);
    UNREFERENCED_PARAMETER(oldFormat);
    UNREFERENCED_PARAMETER(oldAttributeList);
    UNREFERENCED_PARAMETER(dataRange);
    UNREFERENCED_PARAMETER(attributeRange);

    SAR_LOG("SarKsPinSetDataFormat");
    return STATUS_NOT_IMPLEMENTED;
}

NTSTATUS SarKsPinSetDeviceState(PKSPIN pin, KSSTATE toState, KSSTATE fromState)
{
    UNREFERENCED_PARAMETER(pin);
    UNREFERENCED_PARAMETER(toState);
    UNREFERENCED_PARAMETER(fromState);

    SAR_LOG("SarKsPinSetDataFormat");
    return STATUS_NOT_IMPLEMENTED;
}

NTSTATUS SarKsPinIntersectHandler(
    PVOID context, PIRP irp, PKSP_PIN pin,
    PKSDATARANGE callerDataRange, PKSDATARANGE descriptorDataRange,
    ULONG dataBufferSize, PVOID data, PULONG dataSize)
{
    UNREFERENCED_PARAMETER(context);
    UNREFERENCED_PARAMETER(irp);
    UNREFERENCED_PARAMETER(pin);
    PKSDATARANGE_AUDIO callerFormat = nullptr;
    PKSDATARANGE_AUDIO myFormat = nullptr;

    *dataSize = sizeof(KSDATAFORMAT_WAVEFORMATEX);

    if (callerDataRange->FormatSize == sizeof(KSDATARANGE_AUDIO) &&
        callerDataRange->MajorFormat == KSDATAFORMAT_TYPE_AUDIO) {
        callerFormat = (PKSDATARANGE_AUDIO)callerDataRange;
        SAR_LOG("dataRange: %dHz-%dHz, %dbit-%dbit, x%d",
            callerFormat->MinimumSampleFrequency,
            callerFormat->MaximumSampleFrequency,
            callerFormat->MinimumBitsPerSample,
            callerFormat->MaximumBitsPerSample,
            callerFormat->MaximumChannels);
    }

    if (descriptorDataRange->FormatSize == sizeof(KSDATARANGE_AUDIO) &&
        descriptorDataRange->MajorFormat == KSDATAFORMAT_TYPE_AUDIO) {
        myFormat = (PKSDATARANGE_AUDIO)descriptorDataRange;
        SAR_LOG("matchingDataRange: %dHz-%dHz, %dbit-%dbit, x%d",
            myFormat->MinimumSampleFrequency,
            myFormat->MaximumSampleFrequency,
            myFormat->MinimumBitsPerSample,
            myFormat->MaximumBitsPerSample,
            myFormat->MaximumChannels);
    }

    if (!myFormat || !callerFormat) {
        return STATUS_NO_MATCH;
    }

    if (dataBufferSize == 0) {
        return STATUS_BUFFER_OVERFLOW;
    }

    if (dataBufferSize < sizeof(KSDATAFORMAT_WAVEFORMATEX)) {
        return STATUS_BUFFER_TOO_SMALL;
    }

    if (callerFormat->MaximumBitsPerSample < myFormat->MinimumBitsPerSample ||
        callerFormat->MinimumBitsPerSample > myFormat->MaximumBitsPerSample ||
        callerFormat->MaximumSampleFrequency < myFormat->MinimumSampleFrequency ||
        callerFormat->MinimumSampleFrequency > myFormat->MaximumSampleFrequency ||
        callerFormat->MaximumChannels < myFormat->MaximumChannels) {
        return STATUS_NO_MATCH;
    }

    PKSDATAFORMAT_WAVEFORMATEX waveFormat = (PKSDATAFORMAT_WAVEFORMATEX)data;

    RtlCopyMemory(
        &waveFormat->DataFormat, &myFormat->DataRange, sizeof(KSDATAFORMAT));
    waveFormat->WaveFormatEx.wFormatTag = WAVE_FORMAT_PCM;
    waveFormat->WaveFormatEx.nChannels = (WORD)myFormat->MaximumChannels;
    waveFormat->WaveFormatEx.nSamplesPerSec = myFormat->MaximumSampleFrequency;
    waveFormat->WaveFormatEx.wBitsPerSample = (WORD)myFormat->MaximumBitsPerSample;
    waveFormat->WaveFormatEx.nBlockAlign =
        (WORD)myFormat->MaximumBitsPerSample / 8 * (WORD)myFormat->MaximumChannels;
    waveFormat->WaveFormatEx.nAvgBytesPerSec =
        waveFormat->WaveFormatEx.nBlockAlign *
        waveFormat->WaveFormatEx.nSamplesPerSec;
    waveFormat->WaveFormatEx.cbSize = 0;
    waveFormat->DataFormat.SampleSize = waveFormat->WaveFormatEx.nBlockAlign;
    waveFormat->DataFormat.FormatSize = *dataSize;
    return STATUS_SUCCESS;
}