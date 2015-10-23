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

NTSTATUS SarKsFilterCreate(PKSFILTER filter, PIRP irp)
{
    filter->Context = SarGetEndpointFromIrp(irp, TRUE);

    if (!filter->Context) {
        SAR_LOG("Failed to find endpoint for filter");
        return STATUS_NOT_FOUND;
    }

    return STATUS_SUCCESS;
}

NTSTATUS SarKsFilterClose(PKSFILTER filter, PIRP irp)
{
    UNREFERENCED_PARAMETER(irp);
    SarReleaseEndpointAndContext((SarEndpoint *)filter->Context);
    return STATUS_SUCCESS;
}

NTSTATUS SarKsPinGetName(
    PIRP irp, PKSIDENTIFIER request, PVOID data)
{
    PKSP_PIN pinRequest = (PKSP_PIN)request;

    if (pinRequest->PinId != 1) {
        return STATUS_NOT_FOUND;
    }

    SarEndpoint *endpoint = SarGetEndpointFromIrp(irp, TRUE);

    if (!endpoint) {
        return STATUS_NOT_FOUND;
    }

    PIO_STACK_LOCATION irpStack = IoGetCurrentIrpStackLocation(irp);
    ULONG outputLength =
        irpStack->Parameters.DeviceIoControl.OutputBufferLength;

    irp->IoStatus.Information = endpoint->deviceName.MaximumLength;

    if (outputLength == 0) {
        SarReleaseEndpointAndContext(endpoint);
        return STATUS_BUFFER_OVERFLOW;
    }

    if (outputLength < endpoint->deviceName.MaximumLength) {
        SarReleaseEndpointAndContext(endpoint);
        return STATUS_BUFFER_TOO_SMALL;
    }

    UNICODE_STRING output = { 0, (USHORT)outputLength, (PWCH)data };

    RtlCopyUnicodeString(&output, &endpoint->deviceName);
    SarReleaseEndpointAndContext(endpoint);
    return STATUS_SUCCESS;
}

NTSTATUS SarKsPinProcess(PKSPIN pin)
{
    UNREFERENCED_PARAMETER(pin);
    return STATUS_PENDING;
}

NTSTATUS SarKsPinCreate(PKSPIN pin, PIRP irp)
{
    UNREFERENCED_PARAMETER(pin);
    UNREFERENCED_PARAMETER(irp);
    SAR_LOG("SarKsPinCreate");
    SarEndpoint *endpoint = SarGetEndpointFromIrp(irp, TRUE);
    NTSTATUS status;

    pin->Context = endpoint;

    if (!pin->Context) {
        SAR_LOG("Failed to find endpoint for pin");
        return STATUS_NOT_FOUND;
    }

    InterlockedCompareExchangePointer(
        (PVOID *)&endpoint->activePin, pin, nullptr);

    if (endpoint->activePin != pin) {
        pin->Context = nullptr;
        SarReleaseEndpointAndContext(endpoint);
        return STATUS_RESOURCE_IN_USE;
    }

    status = SarGetOrCreateEndpointProcessContext(
        endpoint, PsGetCurrentProcess(), nullptr);

    if (!NT_SUCCESS(status)) {
        pin->Context = nullptr;
        endpoint->activePin = nullptr;
        SarReleaseEndpointAndContext(endpoint);
        return status;
    }

    endpoint->activeCellIndex = 0;
    endpoint->activeViewSize = 0;

    SarEndpointRegisters regs = {};

    if (!NT_SUCCESS(SarReadEndpointRegisters(&regs, endpoint))) {
        SAR_LOG("Couldn't increment endpoint generation");
    } else {
        regs.generation =
            MAKE_GENERATION(GENERATION_NUMBER(regs.generation) + 1, FALSE);

        if (!NT_SUCCESS(SarWriteEndpointRegisters(&regs, endpoint))) {
            SAR_LOG("Couldn't write endpoint registers");
        }
    }

    return status;
}

NTSTATUS SarGetOrCreateEndpointProcessContext(
    SarEndpoint *endpoint,
    PEPROCESS process,
    SarEndpointProcessContext **outContext)
{
    NTSTATUS status;
    SarEndpointProcessContext *newContext = nullptr;
    SIZE_T viewSize = SAR_BUFFER_CELL_SIZE;
    LARGE_INTEGER registerFileOffset = {};

    ExAcquireFastMutex(&endpoint->mutex);

    PLIST_ENTRY entry = endpoint->activeProcessList.Flink;
    SarEndpointProcessContext *foundContext = nullptr;

    while (entry != &endpoint->activeProcessList) {
        SarEndpointProcessContext *existingContext =
            CONTAINING_RECORD(entry, SarEndpointProcessContext, listEntry);
        entry = entry->Flink;

        if (existingContext->process == process) {
            foundContext = existingContext;
            break;
        }
    }

    ExReleaseFastMutex(&endpoint->mutex);

    if (foundContext) {
        if (outContext) {
            *outContext = foundContext;
        }

        return STATUS_SUCCESS;
    }

    newContext = (SarEndpointProcessContext *)ExAllocatePoolWithTag(
        NonPagedPool, sizeof(SarEndpointProcessContext), SAR_TAG);

    if (!newContext) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    RtlZeroMemory(newContext, sizeof(SarEndpointProcessContext));
    newContext->process = process;
    status = ObOpenObjectByPointerWithTag(
        newContext->process, OBJ_KERNEL_HANDLE,
        nullptr, GENERIC_ALL, nullptr,
        KernelMode, SAR_TAG, &newContext->processHandle);

    if (!NT_SUCCESS(status)) {
        goto err_out;
    }

    registerFileOffset.QuadPart = endpoint->owner->bufferSize;
    status = ZwMapViewOfSection(endpoint->owner->bufferSection,
        ZwCurrentProcess(), (PVOID *)&newContext->registerFileUVA, 0, 0,
        &registerFileOffset, &viewSize, ViewUnmap, 0, PAGE_READWRITE);

    if (!NT_SUCCESS(status)) {
        SAR_LOG("Failed to map register file to userspace %08X", status);
        goto err_out;
    }

    ExAcquireFastMutex(&endpoint->mutex);
    InsertHeadList(&endpoint->activeProcessList, &newContext->listEntry);
    ExReleaseFastMutex(&endpoint->mutex);

    if (outContext) {
        *outContext = newContext;
    }

    return STATUS_SUCCESS;

err_out:
    if (newContext != nullptr) {
        if (newContext->processHandle) {
            ZwClose(newContext->processHandle);
        }

        ExFreePoolWithTag(newContext, SAR_TAG);
    }

    return status;
}

NTSTATUS SarKsPinClose(PKSPIN pin, PIRP irp)
{
    UNREFERENCED_PARAMETER(irp);
    SAR_LOG("SarKsPinClose");
    SarEndpoint *endpoint = (SarEndpoint *)pin->Context;
    SarEndpointRegisters regs = {};
    LIST_ENTRY toRemoveList;

    if (!NT_SUCCESS(SarReadEndpointRegisters(&regs, endpoint))) {
        SAR_LOG("Couldn't increment endpoint generation");
    } else {
        regs.generation =
            MAKE_GENERATION(GENERATION_NUMBER(regs.generation) + 1, FALSE);

        if (!NT_SUCCESS(SarWriteEndpointRegisters(&regs, endpoint))) {
            SAR_LOG("Couldn't clear endpoint registers");
        }
    }

    InitializeListHead(&toRemoveList);
    ExAcquireFastMutex(&endpoint->mutex);

    if (!IsListEmpty(&endpoint->activeProcessList)) {
        PLIST_ENTRY entry = endpoint->activeProcessList.Flink;

        RemoveEntryList(&endpoint->activeProcessList);
        InitializeListHead(&endpoint->activeProcessList);
        AppendTailList(&toRemoveList, entry);
    }

    ExReleaseFastMutex(&endpoint->mutex);

    PLIST_ENTRY entry = toRemoveList.Flink;

    while (entry != &toRemoveList) {
        SarEndpointProcessContext *context =
            CONTAINING_RECORD(entry, SarEndpointProcessContext, listEntry);

        entry = entry->Flink;
        SarDeleteEndpointProcessContext(context);
    }

    if (endpoint->activeViewSize) {
        ExAcquireFastMutex(&endpoint->owner->mutex);
        RtlClearBits(&endpoint->owner->bufferMap,
            endpoint->activeCellIndex,
            (ULONG)endpoint->activeViewSize / SAR_BUFFER_CELL_SIZE);
        ExReleaseFastMutex(&endpoint->owner->mutex);
    }

    endpoint->activeCellIndex = 0;
    endpoint->activeViewSize = 0;
    InterlockedExchangePointer((PVOID *)&endpoint->activePin, nullptr);
    SarReleaseEndpointAndContext(endpoint);
    return STATUS_SUCCESS;
}

NTSTATUS SarDeleteEndpointProcessContext(SarEndpointProcessContext *context)
{
    if (context->bufferUVA) {
        ZwUnmapViewOfSection(context->processHandle, context->bufferUVA);
    }

    ZwUnmapViewOfSection(context->processHandle, context->registerFileUVA);
    ZwClose(context->processHandle);
    ExFreePoolWithTag(context, SAR_TAG);

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
    PKSDATAFORMAT_WAVEFORMATEXTENSIBLE waveFormat =
        (PKSDATAFORMAT_WAVEFORMATEXTENSIBLE)pin->ConnectionFormat;

    if (waveFormat->DataFormat.FormatSize <
        sizeof(KSDATAFORMAT_WAVEFORMATEXTENSIBLE)) {

        return STATUS_NO_MATCH;
    }

    if (waveFormat->WaveFormatExt.Format.wFormatTag !=
            WAVE_FORMAT_EXTENSIBLE ||
        waveFormat->WaveFormatExt.Format.nChannels !=
            audioRange->MaximumChannels ||
        waveFormat->WaveFormatExt.Format.nSamplesPerSec !=
            audioRange->MaximumSampleFrequency ||
        waveFormat->WaveFormatExt.Samples.wValidBitsPerSample !=
            audioRange->MaximumBitsPerSample ||
        waveFormat->WaveFormatExt.SubFormat != KSDATAFORMAT_SUBTYPE_PCM) {

        return STATUS_NO_MATCH;
    }

    return STATUS_SUCCESS;
}

NTSTATUS SarKsPinSetDeviceState(PKSPIN pin, KSSTATE toState, KSSTATE fromState)
{
    SAR_LOG("SarKsPinSetDeviceState %d %d", toState, fromState);

    NTSTATUS status;
    SarEndpoint *endpoint = (SarEndpoint *)pin->Context;
    BOOL isActive = FALSE, needsChange = FALSE, resetPosition = FALSE;

    if (toState == KSSTATE_RUN && fromState != KSSTATE_RUN) {
        isActive = TRUE;
        needsChange = TRUE;
    } else if (fromState == KSSTATE_RUN && toState != KSSTATE_RUN) {
        isActive = FALSE;
        needsChange = TRUE;
    }

    if (toState == KSSTATE_STOP && fromState != KSSTATE_STOP) {
        resetPosition = TRUE;
        needsChange = TRUE;
    } else if (fromState == KSSTATE_STOP && toState != KSSTATE_STOP) {
        resetPosition = TRUE;
        needsChange = TRUE;
    }

    if (needsChange) {
        SarEndpointRegisters regs = {};

        status = SarReadEndpointRegisters(&regs, endpoint);

        if (!NT_SUCCESS(status)) {
            return status;
        }

        regs.generation =
            MAKE_GENERATION(GENERATION_NUMBER(regs.generation), isActive);

        if (resetPosition) {
            regs.positionRegister = 0;
        }

        status = SarWriteEndpointRegisters(&regs, endpoint);

        if (!NT_SUCCESS(status)) {
            return status;
        }
    }

    return STATUS_SUCCESS;
}

VOID SarDumpKsIoctl(PIRP irp)
{
    PIO_STACK_LOCATION irpStack = IoGetCurrentIrpStackLocation(irp);

    if (irpStack->MajorFunction == IRP_MJ_DEVICE_CONTROL) {
        if (irpStack->Parameters.DeviceIoControl.IoControlCode ==
                IOCTL_KS_PROPERTY) {

            KSPROPERTY propertyInfo = {};

            SarReadUserBuffer(&propertyInfo, irp, sizeof(KSPROPERTY));

            SAR_LOG("KSProperty: Set " GUID_FORMAT " Id %d Flags %d",
                GUID_VALUES(propertyInfo.Set), propertyInfo.Id,
                propertyInfo.Flags);
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

    *dataSize = sizeof(KSDATAFORMAT_WAVEFORMATEXTENSIBLE);

    if (callerDataRange->FormatSize == sizeof(KSDATARANGE_AUDIO) &&
        callerDataRange->MajorFormat == KSDATAFORMAT_TYPE_AUDIO) {
        callerFormat = (PKSDATARANGE_AUDIO)callerDataRange;
        SAR_LOG("callerFormat: %d-%dHz, %d-%dbits, x%d",
            callerFormat->MinimumSampleFrequency,
            callerFormat->MaximumSampleFrequency,
            callerFormat->MinimumBitsPerSample,
            callerFormat->MaximumBitsPerSample,
            callerFormat->MaximumChannels);
    }

    if (descriptorDataRange->FormatSize == sizeof(KSDATARANGE_AUDIO) &&
        descriptorDataRange->MajorFormat == KSDATAFORMAT_TYPE_AUDIO) {
        myFormat = (PKSDATARANGE_AUDIO)descriptorDataRange;
    } else {
        return STATUS_NO_MATCH;
    }

    if (!myFormat || !callerFormat) {
        return STATUS_NO_MATCH;
    }

    if (dataBufferSize == 0) {
        return STATUS_BUFFER_OVERFLOW;
    }

    if (dataBufferSize < sizeof(KSDATAFORMAT_WAVEFORMATEXTENSIBLE)) {
        return STATUS_BUFFER_TOO_SMALL;
    }

    if (callerFormat->MaximumBitsPerSample < myFormat->MinimumBitsPerSample ||
        callerFormat->MinimumBitsPerSample > myFormat->MaximumBitsPerSample ||
        (callerFormat->MaximumSampleFrequency <
         myFormat->MinimumSampleFrequency) ||
        (callerFormat->MinimumSampleFrequency >
         myFormat->MaximumSampleFrequency) ||
        callerFormat->MaximumChannels < myFormat->MaximumChannels) {
        return STATUS_NO_MATCH;
    }

    PKSDATAFORMAT_WAVEFORMATEXTENSIBLE waveFormat =
        (PKSDATAFORMAT_WAVEFORMATEXTENSIBLE)data;

    RtlCopyMemory(
        &waveFormat->DataFormat, &myFormat->DataRange, sizeof(KSDATAFORMAT));
    waveFormat->WaveFormatExt.Format.wFormatTag =
        WAVE_FORMAT_EXTENSIBLE;
    waveFormat->WaveFormatExt.Format.nChannels =
        (WORD)myFormat->MaximumChannels;
    waveFormat->WaveFormatExt.Format.nSamplesPerSec =
        myFormat->MaximumSampleFrequency;
    waveFormat->WaveFormatExt.Format.wBitsPerSample =
        (WORD)myFormat->MaximumBitsPerSample;
    waveFormat->WaveFormatExt.Format.nBlockAlign =
        ((WORD)myFormat->MaximumBitsPerSample / 8) *
        (WORD)myFormat->MaximumChannels;
    waveFormat->WaveFormatExt.Format.nAvgBytesPerSec =
        waveFormat->WaveFormatExt.Format.nBlockAlign *
        waveFormat->WaveFormatExt.Format.nSamplesPerSec;
    waveFormat->WaveFormatExt.Format.cbSize =
        sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    waveFormat->WaveFormatExt.Samples.wValidBitsPerSample =
        (WORD)myFormat->MaximumBitsPerSample;
    waveFormat->WaveFormatExt.dwChannelMask =
        (1 << myFormat->MaximumChannels) - 1;
    waveFormat->WaveFormatExt.SubFormat = KSDATAFORMAT_SUBTYPE_PCM;
    waveFormat->DataFormat.SampleSize =
        waveFormat->WaveFormatExt.Format.nBlockAlign;
    waveFormat->DataFormat.FormatSize = *dataSize;
    return STATUS_SUCCESS;
}

NTSTATUS SarKsPinGetGlobalInstancesCount(
    PIRP irp, PKSIDENTIFIER request, PVOID data)
{
    PKSP_PIN pinRequest = (PKSP_PIN)request;
    PKSPIN_CINSTANCES instances = (PKSPIN_CINSTANCES)data;
    SarEndpoint *endpoint = SarGetEndpointFromIrp(irp, TRUE);

    if (!endpoint) {
        return STATUS_NOT_FOUND;
    }

    if (pinRequest->PinId == 0) {
        instances->CurrentCount = (endpoint && endpoint->activePin) ? 1 : 0;
        instances->PossibleCount = 1;
    } else {
        instances->CurrentCount = 0;
        instances->PossibleCount = 0;
    }

    SarReleaseEndpointAndContext(endpoint);
    return STATUS_SUCCESS;
}

NTSTATUS SarKsPinGetDefaultDataFormat(
    PIRP irp, PKSIDENTIFIER request, PVOID data)
{
    PKSP_PIN pinRequest = (PKSP_PIN)request;

    if (pinRequest->PinId != 0) {
        return STATUS_NOT_FOUND;
    }

    SarEndpoint *endpoint = SarGetEndpointFromIrp(irp, TRUE);
    PIO_STACK_LOCATION irpStack = IoGetCurrentIrpStackLocation(irp);
    ULONG outputLength =
        irpStack->Parameters.DeviceIoControl.OutputBufferLength;

    if (!endpoint) {
        return STATUS_NOT_FOUND;
    }

    if (outputLength < sizeof(KSDATAFORMAT_WAVEFORMATEXTENSIBLE)) {
        irp->IoStatus.Information = sizeof(KSDATAFORMAT_WAVEFORMATEXTENSIBLE);
        SarReleaseEndpointAndContext(endpoint);
        return STATUS_BUFFER_OVERFLOW;
    }

    PKSDATAFORMAT_WAVEFORMATEXTENSIBLE waveFormat =
        (PKSDATAFORMAT_WAVEFORMATEXTENSIBLE)data;
    PKSDATARANGE_AUDIO myFormat = endpoint->dataRange;

    RtlCopyMemory(
        &waveFormat->DataFormat, &myFormat->DataRange, sizeof(KSDATAFORMAT));
    waveFormat->WaveFormatExt.Format.wFormatTag =
        WAVE_FORMAT_EXTENSIBLE;
    waveFormat->WaveFormatExt.Format.nChannels =
        (WORD)myFormat->MaximumChannels;
    waveFormat->WaveFormatExt.Format.nSamplesPerSec =
        myFormat->MaximumSampleFrequency;
    waveFormat->WaveFormatExt.Format.wBitsPerSample =
        (WORD)myFormat->MaximumBitsPerSample;
    waveFormat->WaveFormatExt.Format.nBlockAlign =
        ((WORD)myFormat->MaximumBitsPerSample / 8) *
        (WORD)myFormat->MaximumChannels;
    waveFormat->WaveFormatExt.Format.nAvgBytesPerSec =
        waveFormat->WaveFormatExt.Format.nBlockAlign *
        waveFormat->WaveFormatExt.Format.nSamplesPerSec;
    waveFormat->WaveFormatExt.Format.cbSize =
        sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    waveFormat->WaveFormatExt.Samples.wValidBitsPerSample =
        (WORD)myFormat->MaximumBitsPerSample;
    waveFormat->WaveFormatExt.dwChannelMask =
        (1 << myFormat->MaximumChannels) - 1;
    waveFormat->WaveFormatExt.SubFormat = KSDATAFORMAT_SUBTYPE_PCM;
    waveFormat->DataFormat.SampleSize =
        waveFormat->WaveFormatExt.Format.nBlockAlign;
    waveFormat->DataFormat.FormatSize =
        sizeof(KSDATAFORMAT_WAVEFORMATEXTENSIBLE);
    SarReleaseEndpointAndContext(endpoint);
    return STATUS_SUCCESS;
}

NTSTATUS SarKsPinProposeDataFormat(
    PIRP irp, PKSIDENTIFIER request, PVOID data)
{
    PKSP_PIN pinRequest = (PKSP_PIN)request;
    PIO_STACK_LOCATION irpStack = IoGetCurrentIrpStackLocation(irp);
    ULONG outputLength =
        irpStack->Parameters.DeviceIoControl.OutputBufferLength;

    if (pinRequest->PinId != 0) {
        return STATUS_NOT_FOUND;
    }

    SarEndpoint *endpoint = SarGetEndpointFromIrp(irp, TRUE);

    if (!endpoint) {
        return STATUS_NOT_FOUND;
    }

    if (outputLength < sizeof(KSDATAFORMAT)) {
        SarReleaseEndpointAndContext(endpoint);
        return STATUS_BUFFER_TOO_SMALL;
    }

    PKSDATAFORMAT_WAVEFORMATEXTENSIBLE format =
        (PKSDATAFORMAT_WAVEFORMATEXTENSIBLE)data;

    if (format->DataFormat.MajorFormat != KSDATAFORMAT_TYPE_AUDIO ||
        format->DataFormat.SubFormat != KSDATAFORMAT_SUBTYPE_PCM ||
        format->DataFormat.Specifier != KSDATAFORMAT_SPECIFIER_WAVEFORMATEX) {
        SAR_LOG("Format type can't be handled %d %d %d"
            GUID_FORMAT " " GUID_FORMAT " " GUID_FORMAT,
            format->DataFormat.FormatSize,
            sizeof(KSDATAFORMAT_WAVEFORMATEX),
            sizeof(KSDATAFORMAT),
            GUID_VALUES(format->DataFormat.MajorFormat),
            GUID_VALUES(format->DataFormat.SubFormat),
            GUID_VALUES(format->DataFormat.Specifier));
        SarReleaseEndpointAndContext(endpoint);
        return STATUS_NO_MATCH;
    }

    if (outputLength < sizeof(KSDATAFORMAT_WAVEFORMATEXTENSIBLE)) {
        SarReleaseEndpointAndContext(endpoint);
        return STATUS_BUFFER_TOO_SMALL;
    }

    if (format->WaveFormatExt.Format.nChannels != endpoint->channelCount ||
        (format->WaveFormatExt.Format.wBitsPerSample !=
         endpoint->owner->sampleSize * 8) ||
        format->WaveFormatExt.Format.wFormatTag != WAVE_FORMAT_EXTENSIBLE ||
        (format->WaveFormatExt.Format.nSamplesPerSec !=
         endpoint->owner->sampleRate) ||
        format->WaveFormatExt.SubFormat != KSDATAFORMAT_SUBTYPE_PCM) {

        SarReleaseEndpointAndContext(endpoint);
        return STATUS_NO_MATCH;
    }

    SarReleaseEndpointAndContext(endpoint);
    return STATUS_SUCCESS;
}
