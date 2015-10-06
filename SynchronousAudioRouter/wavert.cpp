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

NTSTATUS SarKsPinRtGetBufferCore(
    PIRP irp, PVOID baseAddress, ULONG requestedBufferSize,
    ULONG notificationCount, PKSRTAUDIO_BUFFER buffer)
{
    SarEndpoint *endpoint = SarGetEndpointFromIrp(irp, TRUE);
    SarControlContext *controlContext = endpoint->owner;
    SarEndpointProcessContext *processContext;
    NTSTATUS status;

    if (!endpoint) {
        SAR_LOG("No valid endpoint");
        return STATUS_NOT_FOUND;
    }

    if (baseAddress != nullptr) {
        SAR_LOG("It wants a specific address");
        SarReleaseEndpointAndContext(endpoint);
        return STATUS_NOT_IMPLEMENTED;
    }

    status = SarGetOrCreateEndpointProcessContext(
        endpoint, PsGetCurrentProcess(), &processContext);

    if (!NT_SUCCESS(status)) {
        SarReleaseEndpointAndContext(endpoint);
        return status;
    }

    ULONG actualSize = ROUND_UP(
        max(requestedBufferSize,
            controlContext->minimumFrameCount *
            controlContext->frameSize *
            endpoint->channelCount),
        controlContext->frameSize * endpoint->channelCount);
    SIZE_T viewSize = ROUND_UP(actualSize, SAR_BUFFER_CELL_SIZE);

    ExAcquireFastMutex(&controlContext->mutex);

    if (!controlContext->bufferSection) {
        SAR_LOG("Buffer isn't allocated");
        ExReleaseFastMutex(&controlContext->mutex);
        SarReleaseEndpointAndContext(endpoint);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    ULONG cellIndex = RtlFindClearBitsAndSet(
        &controlContext->bufferMap,
        (ULONG)(viewSize / SAR_BUFFER_CELL_SIZE), 0);

    if (cellIndex == 0xFFFFFFFF) {
        ExReleaseFastMutex(&controlContext->mutex);
        SarReleaseEndpointAndContext(endpoint);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    endpoint->activeCellIndex = cellIndex;
    endpoint->activeViewSize = viewSize;
    endpoint->activeBufferSize = actualSize;
    ExReleaseFastMutex(&controlContext->mutex);

    PVOID mappedAddress = nullptr;
    LARGE_INTEGER sectionOffset;

    sectionOffset.QuadPart = cellIndex * SAR_BUFFER_CELL_SIZE;
    SAR_LOG("Mapping %08X %016llX %lu %lu", viewSize, sectionOffset.QuadPart,
        actualSize, requestedBufferSize);
    status = ZwMapViewOfSection(
        endpoint->owner->bufferSection, ZwCurrentProcess(),
        &mappedAddress, 0, 0, &sectionOffset, &viewSize, ViewUnmap,
        0, PAGE_READWRITE);

    if (!NT_SUCCESS(status)) {
        SAR_LOG("Section mapping failed %08X", status);
        SarReleaseEndpointAndContext(endpoint);
        return status;
    }

    SarEndpointRegisters regs = {};

    processContext->bufferUVA = mappedAddress;
    status = SarReadEndpointRegisters(&regs, endpoint);

    if (!NT_SUCCESS(status)) {
        SarReleaseEndpointAndContext(endpoint);
        return status;
    }

    regs.bufferOffset = cellIndex * SAR_BUFFER_CELL_SIZE;
    regs.bufferSize = actualSize;
    regs.notificationCount = notificationCount;
    status = SarWriteEndpointRegisters(&regs, endpoint);

    if (!NT_SUCCESS(status)) {
        SAR_LOG("Couldn't write endpoint registers: %08X %p %p", status,
            processContext->process, PsGetCurrentProcess());
        SarReleaseEndpointAndContext(endpoint);
        return status; // TODO: goto err_out
    }

    buffer->ActualBufferSize = actualSize;
    buffer->BufferAddress = mappedAddress;
    buffer->CallMemoryBarrier = FALSE;
    SarReleaseEndpointAndContext(endpoint);
    return STATUS_SUCCESS;
}

NTSTATUS SarKsPinRtGetBuffer(
    PIRP irp, PKSIDENTIFIER request, PVOID data)
{
    SAR_LOG("SarKsPinRtGetBuffer");
    PKSRTAUDIO_BUFFER_PROPERTY prop = (PKSRTAUDIO_BUFFER_PROPERTY)request;
    PKSRTAUDIO_BUFFER buffer = (PKSRTAUDIO_BUFFER)data;

    return SarKsPinRtGetBufferCore(
        irp, prop->BaseAddress, prop->RequestedBufferSize, 0, buffer);
}

NTSTATUS SarKsPinRtGetBufferWithNotification(
    PIRP irp, PKSIDENTIFIER request, PVOID data)
{
    SAR_LOG("SarKsPinRtGetBufferWithNotification");
    PKSRTAUDIO_BUFFER_PROPERTY_WITH_NOTIFICATION prop =
        (PKSRTAUDIO_BUFFER_PROPERTY_WITH_NOTIFICATION)request;
    PKSRTAUDIO_BUFFER buffer = (PKSRTAUDIO_BUFFER)data;

    return SarKsPinRtGetBufferCore(
        irp, prop->BaseAddress, prop->RequestedBufferSize,
        prop->NotificationCount, buffer);
}

NTSTATUS SarKsPinRtGetClockRegister(
    PIRP irp, PKSIDENTIFIER request, PVOID data)
{
    UNREFERENCED_PARAMETER(request);
    SAR_LOG("SarKsPinRtGetClockRegister");

    NTSTATUS status;
    PKSRTAUDIO_HWREGISTER reg = (PKSRTAUDIO_HWREGISTER)data;
    SarEndpoint *endpoint = SarGetEndpointFromIrp(irp, TRUE);
    SarEndpointProcessContext *context;

    if (!endpoint) {
        return STATUS_UNSUCCESSFUL;
    }

    status = SarGetOrCreateEndpointProcessContext(
        endpoint, PsGetCurrentProcess(), &context);

    if (!NT_SUCCESS(status)) {
        SarReleaseEndpointAndContext(endpoint);
        return status;
    }

    reg->Register =
        &context->registerFileUVA[endpoint->index].clockRegister;
    reg->Width = 32;
    reg->Accuracy = 0;
    reg->Numerator = endpoint->owner->sampleRate;
    reg->Denominator = endpoint->owner->frameSize / endpoint->owner->sampleSize;
    SarReleaseEndpointAndContext(endpoint);
    return STATUS_SUCCESS;
}

#define UNITS_PER_SECOND 10000000

NTSTATUS SarKsPinRtGetHwLatency(
    PIRP irp, PKSIDENTIFIER request, PVOID data)
{
    UNREFERENCED_PARAMETER(irp);
    UNREFERENCED_PARAMETER(request);
    SAR_LOG("SarKsPinRtGetHwLatency");
    PKSRTAUDIO_HWLATENCY latency = (PKSRTAUDIO_HWLATENCY)data;
    SarEndpoint *endpoint = SarGetEndpointFromIrp(irp, TRUE);

    if (!endpoint) {
        return STATUS_UNSUCCESSFUL;
    }

    // TODO: This seems like it should be accurate, but results in weird WASAPI
    // errors. Why?
    //ULONG sampleRate = endpoint->owner->sampleRate;
    //ULONG errPerSample = UNITS_PER_SECOND % sampleRate;
    //ULONG unitsPerSample =
    //    ((sampleRate - errPerSample) + UNITS_PER_SECOND) / sampleRate;

    //latency->FifoSize =
    //    endpoint->channelCount *
    //    endpoint->owner->frameSize;
    latency->ChipsetDelay = 0;
    //latency->CodecDelay =
    //    unitsPerSample *
    //    (endpoint->owner->frameSize / endpoint->owner->sampleSize);
    latency->CodecDelay = latency->FifoSize = 0;
    //SAR_LOG("FifoSize %lu CodecDelay %lu",
    //    latency->FifoSize, latency->CodecDelay);
    SarReleaseEndpointAndContext(endpoint);
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

    NTSTATUS status;
    PKSRTAUDIO_HWREGISTER reg = (PKSRTAUDIO_HWREGISTER)data;
    SarEndpoint *endpoint = SarGetEndpointFromIrp(irp, TRUE);
    SarEndpointProcessContext *context;

    if (!endpoint) {
        return STATUS_UNSUCCESSFUL;
    }

    status = SarGetOrCreateEndpointProcessContext(
        endpoint, PsGetCurrentProcess(), &context);

    if (!NT_SUCCESS(status)) {
        SarReleaseEndpointAndContext(endpoint);
        return status;
    }

    reg->Register =
        &context->registerFileUVA[endpoint->index].positionRegister;
    reg->Width = 32;
    // TODO: reporting a more 'correct' accuracy here causes odd intermittent
    // WASAPI errors, specifically AUDCLNT_E_BUFFER_SIZE_ERROR. Or at least,
    // it seems to be implicated. Why?
    reg->Accuracy = 1;
    reg->Numerator = 0;
    reg->Denominator = 0;
    SarReleaseEndpointAndContext(endpoint);
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
    SAR_LOG("SarKsPinRtQueryNotificationSupport");

    *((BOOL *)data) = TRUE;
    return STATUS_SUCCESS;
}

NTSTATUS SarKsPinRtRegisterNotificationEvent(
    PIRP irp, PKSIDENTIFIER request, PVOID data)
{
    UNREFERENCED_PARAMETER(data);
    SAR_LOG("SarKsPinRtRegisterNotificationEvent");
    SarEndpoint *endpoint = SarGetEndpointFromIrp(irp, TRUE);
    SarEndpointRegisters regs;
    NTSTATUS status;
    PKSRTAUDIO_NOTIFICATION_EVENT_PROPERTY prop =
        (PKSRTAUDIO_NOTIFICATION_EVENT_PROPERTY)request;

    if (!endpoint) {
        return STATUS_UNSUCCESSFUL;
    }

    status = SarReadEndpointRegisters(&regs, endpoint);

    if (!NT_SUCCESS(status)) {
        SarReleaseEndpointAndContext(endpoint);
        return status;
    }

    ULONG64 associatedData = regs.generation | ((ULONG64)endpoint->index << 32);

    status = SarPostHandleQueue(&endpoint->owner->handleQueue,
        prop->NotificationEvent, associatedData);
    SarReleaseEndpointAndContext(endpoint);
    return status;
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
