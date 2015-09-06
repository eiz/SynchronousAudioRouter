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

NTSTATUS SarKsPinRtGetBuffer(
    PIRP irp, PKSIDENTIFIER request, PVOID data)
{
    UNREFERENCED_PARAMETER(irp);
    UNREFERENCED_PARAMETER(request);
    UNREFERENCED_PARAMETER(data);
    SAR_LOG("SarKsPinRtGetBuffer");
    PKSRTAUDIO_BUFFER_PROPERTY prop = (PKSRTAUDIO_BUFFER_PROPERTY)request;
    PKSRTAUDIO_BUFFER buffer = (PKSRTAUDIO_BUFFER)data;

    buffer->ActualBufferSize = prop->RequestedBufferSize;
    buffer->BufferAddress = (PVOID)0x124;
    buffer->CallMemoryBarrier = FALSE;
    return STATUS_SUCCESS;
}

NTSTATUS SarKsPinRtGetBufferWithNotification(
    PIRP irp, PKSIDENTIFIER request, PVOID data)
{
    UNREFERENCED_PARAMETER(irp);
    UNREFERENCED_PARAMETER(request);
    UNREFERENCED_PARAMETER(data);
    SAR_LOG("SarKsPinRtGetBufferWithNotification");
    return STATUS_NOT_IMPLEMENTED;
}

NTSTATUS SarKsPinRtGetClockRegister(
    PIRP irp, PKSIDENTIFIER request, PVOID data)
{
    UNREFERENCED_PARAMETER(irp);
    UNREFERENCED_PARAMETER(request);
    UNREFERENCED_PARAMETER(data);
    SAR_LOG("SarKsPinRtGetClockRegister");
    return STATUS_NOT_IMPLEMENTED;
}

NTSTATUS SarKsPinRtGetHwLatency(
    PIRP irp, PKSIDENTIFIER request, PVOID data)
{
    UNREFERENCED_PARAMETER(irp);
    UNREFERENCED_PARAMETER(request);
    UNREFERENCED_PARAMETER(data);
    SAR_LOG("SarKsPinRtGetHwLatency");
    PKSRTAUDIO_HWLATENCY latency = (PKSRTAUDIO_HWLATENCY)data;

    latency->FifoSize = 0;
    latency->ChipsetDelay = 0;
    latency->CodecDelay = 0;
    return STATUS_SUCCESS;
}

NTSTATUS SarKsPinRtGetPacketCount(
    PIRP irp, PKSIDENTIFIER request, PVOID data)
{
    UNREFERENCED_PARAMETER(irp);
    UNREFERENCED_PARAMETER(request);
    UNREFERENCED_PARAMETER(data);
    SAR_LOG("SarKsPinRtGetPacketCount");
    return STATUS_NOT_IMPLEMENTED;
}

NTSTATUS SarKsPinRtGetPositionRegister(
    PIRP irp, PKSIDENTIFIER request, PVOID data)
{
    UNREFERENCED_PARAMETER(irp);
    UNREFERENCED_PARAMETER(request);
    UNREFERENCED_PARAMETER(data);
    SAR_LOG("SarKsPinRtGetPositionRegister");
    PKSRTAUDIO_HWREGISTER reg = (PKSRTAUDIO_HWREGISTER)data;

    reg->Register = (PVOID)0x130;
    reg->Width = 32;
    reg->Accuracy = 0;
    reg->Numerator = 0;
    reg->Denominator = 0;
    return STATUS_SUCCESS;
}

NTSTATUS SarKsPinRtGetPresentationPosition(
    PIRP irp, PKSIDENTIFIER request, PVOID data)
{
    UNREFERENCED_PARAMETER(irp);
    UNREFERENCED_PARAMETER(request);
    UNREFERENCED_PARAMETER(data);
    SAR_LOG("SarKsPinRtGetPresentationPosition");
    return STATUS_NOT_IMPLEMENTED;
}

NTSTATUS SarKsPinRtQueryNotificationSupport(
    PIRP irp, PKSIDENTIFIER request, PVOID data)
{
    UNREFERENCED_PARAMETER(irp);
    UNREFERENCED_PARAMETER(request);
    UNREFERENCED_PARAMETER(data);
    SAR_LOG("SarKsPinRtQueryNotificationSupport");
    return STATUS_NOT_IMPLEMENTED;
}

NTSTATUS SarKsPinRtRegisterNotificationEvent(
    PIRP irp, PKSIDENTIFIER request, PVOID data)
{
    UNREFERENCED_PARAMETER(irp);
    UNREFERENCED_PARAMETER(request);
    UNREFERENCED_PARAMETER(data);
    SAR_LOG("SarKsPinRtRegisterNotificationEvent");
    return STATUS_NOT_IMPLEMENTED;
}

NTSTATUS SarKsPinRtUnregisterNotificationEvent(
    PIRP irp, PKSIDENTIFIER request, PVOID data)
{
    UNREFERENCED_PARAMETER(irp);
    UNREFERENCED_PARAMETER(request);
    UNREFERENCED_PARAMETER(data);
    SAR_LOG("SarKsPinRtUnregisterNotificationEvent");
    return STATUS_NOT_IMPLEMENTED;
}
