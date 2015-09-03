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

NTSTATUS SarKsPinProcess(PKSPIN pin)
{
    UNREFERENCED_PARAMETER(pin);
    SAR_LOG("Pin level processing");
    return STATUS_SUCCESS;
}

NTSTATUS SarKsPinCreate(PKSPIN pin, PIRP irp)
{
    UNREFERENCED_PARAMETER(pin);
    UNREFERENCED_PARAMETER(irp);
    SAR_LOG("SarKsPinCreate");
    return STATUS_SUCCESS;
}

NTSTATUS SarKsPinClose(PKSPIN pin, PIRP irp)
{
    UNREFERENCED_PARAMETER(pin);
    UNREFERENCED_PARAMETER(irp);
    SAR_LOG("SarKsPinClose");
    return STATUS_SUCCESS;
}

VOID SarKsPinReset(PKSPIN pin)
{
    UNREFERENCED_PARAMETER(pin);
    SAR_LOG("SarKsPinReset");
}

NTSTATUS SarKsPinConnect(PKSPIN pin)
{
    UNREFERENCED_PARAMETER(pin);
    SAR_LOG("SarKsPinConnect");
    return STATUS_SUCCESS;
}

VOID SarKsPinDisconnect(PKSPIN pin)
{
    UNREFERENCED_PARAMETER(pin);
    SAR_LOG("SarKsPinDisconnect");
}

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
    UNREFERENCED_PARAMETER(attributeRange);

    NT_ASSERT(!oldFormat);

    const KSDATARANGE_AUDIO *audioRange = (const KSDATARANGE_AUDIO *)dataRange;
    PKSDATAFORMAT_WAVEFORMATEX waveFormat =
        (PKSDATAFORMAT_WAVEFORMATEX)pin->ConnectionFormat;

    if (waveFormat->DataFormat.FormatSize != sizeof(KSDATAFORMAT_WAVEFORMATEX)) {
        return STATUS_NO_MATCH;
    }

    if (waveFormat->WaveFormatEx.wFormatTag != WAVE_FORMAT_PCM ||
        waveFormat->WaveFormatEx.nChannels != audioRange->MaximumChannels ||
        waveFormat->WaveFormatEx.nSamplesPerSec !=
            audioRange->MaximumSampleFrequency ||
        waveFormat->WaveFormatEx.wBitsPerSample !=
            audioRange->MaximumBitsPerSample) {

        return STATUS_NO_MATCH;
    }

    return STATUS_SUCCESS;
}

NTSTATUS SarKsPinSetDeviceState(PKSPIN pin, KSSTATE toState, KSSTATE fromState)
{
    UNREFERENCED_PARAMETER(pin);
    UNREFERENCED_PARAMETER(toState);
    UNREFERENCED_PARAMETER(fromState);

    SAR_LOG("SarKsPinSetDeviceState %d %d", toState, fromState);
    return STATUS_SUCCESS;
}

#define GUID_FORMAT "{%08lx-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x}"
#define GUID_VALUES(g) (g).Data1, (g).Data2, (g).Data3, \
    (g).Data4[0], \
    (g).Data4[1], \
    (g).Data4[2], \
    (g).Data4[3], \
    (g).Data4[4], \
    (g).Data4[5], \
    (g).Data4[6], \
    (g).Data4[7]

NTSTATUS SarCopyUserBuffer(PVOID dest, PIO_STACK_LOCATION irpStack, ULONG size)
{
    if (irpStack->Parameters.DeviceIoControl.InputBufferLength < size) {
        return STATUS_BUFFER_OVERFLOW;
    }

    __try {
        ProbeForRead(irpStack->Parameters.DeviceIoControl.Type3InputBuffer, size,
            TYPE_ALIGNMENT(ULONG));
        RtlCopyMemory(
            dest, irpStack->Parameters.DeviceIoControl.Type3InputBuffer, size);
        return STATUS_SUCCESS;
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        return GetExceptionCode();
    }
}

VOID SarDumpKsIoctl(PIO_STACK_LOCATION irpStack)
{
    if (irpStack->MajorFunction == IRP_MJ_DEVICE_CONTROL) {
        if (irpStack->Parameters.DeviceIoControl.IoControlCode ==
                IOCTL_KS_PROPERTY) {

            KSPROPERTY propertyInfo = {};

            SarCopyUserBuffer(&propertyInfo, irpStack, sizeof(KSPROPERTY));

            SAR_LOG("KSProperty: Set " GUID_FORMAT " Id %d Flags %d",
                GUID_VALUES(propertyInfo.Set), propertyInfo.Id,
                propertyInfo.Flags);

            if (propertyInfo.Set == KSPROPSETID_Pin &&
                propertyInfo.Id == KSPROPERTY_PIN_PROPOSEDATAFORMAT) {
                KSP_PIN ppInfo = {};

                SarCopyUserBuffer(&ppInfo, irpStack, sizeof(KSP_PIN));
                SAR_LOG("Breaking for pin %d", ppInfo.PinId);
                //DbgBreakPoint();
            }
        }
    }
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
    }

    if (descriptorDataRange->FormatSize == sizeof(KSDATARANGE_AUDIO) &&
        descriptorDataRange->MajorFormat == KSDATAFORMAT_TYPE_AUDIO) {
        myFormat = (PKSDATARANGE_AUDIO)descriptorDataRange;
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

NTSTATUS SarKsPinGetGlobalInstancesCount(
    PIRP irp, PKSIDENTIFIER request, PVOID data)
{
    UNREFERENCED_PARAMETER(irp);
    PKSP_PIN pinRequest = (PKSP_PIN)request;
    PKSPIN_CINSTANCES instances = (PKSPIN_CINSTANCES)data;

    if (pinRequest->PinId == 0) {
        instances->CurrentCount = 0;
        instances->PossibleCount = 1;
    } else {
        instances->CurrentCount = 0;
        instances->PossibleCount = 0;
    }

    return STATUS_SUCCESS;
}

NTSTATUS SarKsPinProposeDataFormat(
    PIRP irp, PKSIDENTIFIER request, PVOID data)
{
    UNREFERENCED_PARAMETER(irp);
    UNREFERENCED_PARAMETER(data);
    PKSP_PIN pinRequest = (PKSP_PIN)request;
    PKSDATAFORMAT_WAVEFORMATEX format = (PKSDATAFORMAT_WAVEFORMATEX)data;

    if (pinRequest->PinId != 0) {
        return STATUS_NOT_FOUND;
    }

    SarEndpoint *endpoint = SarGetEndpointFromIrp(irp);

    if (!endpoint) {
        return STATUS_NOT_FOUND;
    }

    if (format->DataFormat.MajorFormat != KSDATAFORMAT_TYPE_AUDIO ||
        format->DataFormat.SubFormat != KSDATAFORMAT_SUBTYPE_PCM ||
        format->DataFormat.Specifier != KSDATAFORMAT_SPECIFIER_WAVEFORMATEX ||
        format->DataFormat.FormatSize < sizeof(KSDATAFORMAT_WAVEFORMATEX)) {
        SAR_LOG("Format type can't be handled");
        return STATUS_NO_MATCH;
    }

    if (format->WaveFormatEx.nChannels != endpoint->channelCount ||
        format->WaveFormatEx.wBitsPerSample != endpoint->owner->sampleDepth * 8 ||
        format->WaveFormatEx.wFormatTag != WAVE_FORMAT_PCM ||
        format->WaveFormatEx.nSamplesPerSec != endpoint->owner->sampleRate) {
        return STATUS_NO_MATCH;
    }

    return STATUS_SUCCESS;
}

