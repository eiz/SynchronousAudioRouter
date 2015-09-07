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
    SAR_LOG("SarKsPinRtGetBuffer");
    PKSRTAUDIO_BUFFER_PROPERTY prop = (PKSRTAUDIO_BUFFER_PROPERTY)request;
    PKSRTAUDIO_BUFFER buffer = (PKSRTAUDIO_BUFFER)data;
    SarEndpoint *endpoint = SarGetEndpointFromIrp(irp);
    SarFileContext *fileContext = endpoint->owner;
    NTSTATUS status;

    if (!endpoint) {
        SAR_LOG("No valid endpoint");
        return STATUS_NOT_FOUND;
    }

    if (prop->BaseAddress != nullptr) {
        SAR_LOG("It wants a specific address");
        return STATUS_NOT_IMPLEMENTED;
    }

    if (endpoint->activeProcess != PsGetCurrentProcess()) {
        SAR_LOG("Process didn't create the active pin");
        return STATUS_ACCESS_DENIED;
    }

    SIZE_T viewSize = ROUND_UP(prop->RequestedBufferSize, SAR_BUFFER_CELL_SIZE);

    ExAcquireFastMutex(&fileContext->mutex);

    if (!fileContext->bufferSection) {
        SAR_LOG("Buffer isn't allocated");
        ExReleaseFastMutex(&fileContext->mutex);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    ULONG cellIndex = RtlFindClearBitsAndSet(
        &fileContext->bufferMap, (ULONG)(viewSize / SAR_BUFFER_CELL_SIZE), 0);

    if (cellIndex == 0xFFFFFFFF) {
        ExReleaseFastMutex(&fileContext->mutex);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    ExReleaseFastMutex(&fileContext->mutex);

    PVOID mappedAddress = nullptr;
    LARGE_INTEGER sectionOffset;

    sectionOffset.QuadPart = cellIndex * SAR_BUFFER_CELL_SIZE;
    SAR_LOG("Mapping %08X %016llX", viewSize, sectionOffset.QuadPart);
    status = ZwMapViewOfSection(endpoint->owner->bufferSection, ZwCurrentProcess(),
        &mappedAddress, 0, 0, &sectionOffset, &viewSize, ViewUnmap,
        0, PAGE_READWRITE);

    if (!NT_SUCCESS(status)) {
        SAR_LOG("Section mapping failed %08X", status);
        return status;
    }

    SarEndpointRegisters regs;

    regs.isActive = TRUE;
    regs.bufferOffset = cellIndex * SAR_BUFFER_CELL_SIZE;
    regs.bufferSize = prop->RequestedBufferSize;
    regs.clockRegister = 0;
    regs.positionRegister = 0;
    status = SarWriteEndpointRegisters(&regs, endpoint);

    if (!NT_SUCCESS(status)) {
        SAR_LOG("Couldn't write endpoint registers: %08X %p %p", status,
            endpoint->activeProcess, PsGetCurrentProcess());
        return status; // TODO: goto err_out
    }

    buffer->ActualBufferSize = prop->RequestedBufferSize;
    buffer->BufferAddress = mappedAddress;
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
    UNREFERENCED_PARAMETER(request);
    SAR_LOG("SarKsPinRtGetClockRegister");

    PKSRTAUDIO_HWREGISTER reg = (PKSRTAUDIO_HWREGISTER)data;
    SarEndpoint *endpoint = SarGetEndpointFromIrp(irp);

    reg->Register =
        &endpoint->activeRegisterFileUVA[endpoint->index].clockRegister;
    reg->Width = 32;
    reg->Accuracy = 0;
    reg->Numerator = endpoint->owner->sampleRate;
    reg->Denominator = 1;
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
    UNREFERENCED_PARAMETER(request);
    SAR_LOG("SarKsPinRtGetPositionRegister");
    PKSRTAUDIO_HWREGISTER reg = (PKSRTAUDIO_HWREGISTER)data;
    SarEndpoint *endpoint = SarGetEndpointFromIrp(irp);

    reg->Register =
        &endpoint->activeRegisterFileUVA[endpoint->index].positionRegister;
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
