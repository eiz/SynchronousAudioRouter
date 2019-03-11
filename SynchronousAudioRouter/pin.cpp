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


static WORD getnbits(DWORD value) {
    WORD bitOnesCount = 0;

    while (value > 0) {           // until all bits are zero
        if ((value & 1) == 1)     // check lower bit
            bitOnesCount++;
        value >>= 1;              // shift bits, removing lower bit
    }

    return bitOnesCount;
}

NTSTATUS SarKsFilterCreate(PKSFILTER filter, PIRP irp)
{
    filter->Context = SarGetEndpointFromIrp(irp, TRUE);

    if (!filter->Context) {
        SAR_ERROR("Failed to find endpoint for filter");
        return STATUS_NOT_FOUND;
    }

    SAR_DEBUG("create filter");

    return STATUS_SUCCESS;
}

NTSTATUS SarKsFilterClose(PKSFILTER filter, PIRP irp)
{
    UNREFERENCED_PARAMETER(irp);
    SAR_DEBUG("close filter");
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
        SAR_ERROR("Failed to find endpoint for pin");
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

    SAR_DEBUG("Query pinName: %wZ", &output);

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
    SarEndpoint *endpoint = SarGetEndpointFromIrp(irp, TRUE);
    NTSTATUS status;
    DWORD activeChannelCount;

    pin->Context = endpoint;

    if (!pin->Context) {
        SAR_ERROR("Failed to find endpoint for pin");
        return STATUS_NOT_FOUND;
    }

    if (pin->ConnectionFormat != NULL &&
        pin->ConnectionFormat->MajorFormat == KSDATAFORMAT_TYPE_AUDIO &&
        pin->ConnectionFormat->SubFormat == KSDATAFORMAT_SUBTYPE_PCM &&
        pin->ConnectionFormat->Specifier == KSDATAFORMAT_SPECIFIER_WAVEFORMATEX &&
        pin->ConnectionFormat->FormatSize >= sizeof(KSDATAFORMAT_WAVEFORMATEXTENSIBLE))
    {
        PKSDATAFORMAT_WAVEFORMATEXTENSIBLE pinWaveFormat = (PKSDATAFORMAT_WAVEFORMATEXTENSIBLE)pin->ConnectionFormat;
        activeChannelCount = pinWaveFormat->WaveFormatExt.Format.nChannels;
        SAR_DEBUG("Opening pin with %d channels", activeChannelCount);
    }
    else
    {
        SAR_ERROR("Can't read pin ConnectionFormat, using default channel count: %d", endpoint->channelCount);
        activeChannelCount = endpoint->channelCount;
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
        SAR_ERROR("Failed to get process: %08X", status);
        pin->Context = nullptr;
        endpoint->activePin = nullptr;
        SarReleaseEndpointAndContext(endpoint);
        return status;
    }

    endpoint->activeCellIndex = 0;
    endpoint->activeViewSize = 0;

    SarEndpointRegisters regs = {};

    if (!NT_SUCCESS(SarReadEndpointRegisters(&regs, endpoint))) {
        SAR_ERROR("Couldn't increment endpoint generation");
    } else {
        regs.generation =
            MAKE_GENERATION(GENERATION_NUMBER(regs.generation) + 1, FALSE);
        regs.activeChannelCount = activeChannelCount;
        endpoint->activeChannelCount = activeChannelCount;

        if (!NT_SUCCESS(SarWriteEndpointRegisters(&regs, endpoint))) {
            SAR_ERROR("Couldn't write endpoint registers");
        }
    }

    SAR_DEBUG("create pin");

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
        SAR_ERROR("Can't allocate new process context");
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    RtlZeroMemory(newContext, sizeof(SarEndpointProcessContext));
    newContext->process = process;
    status = ObOpenObjectByPointerWithTag(
        newContext->process, OBJ_KERNEL_HANDLE,
        nullptr, GENERIC_ALL, nullptr,
        KernelMode, SAR_TAG, &newContext->processHandle);

    if (!NT_SUCCESS(status)) {
        SAR_ERROR("Error while opening process object: %08X", status);
        goto err_out;
    }

    registerFileOffset.QuadPart = endpoint->owner->bufferSize;
    status = ZwMapViewOfSection(endpoint->owner->bufferSection,
        ZwCurrentProcess(), (PVOID *)&newContext->registerFileUVA, 0, 0,
        &registerFileOffset, &viewSize, ViewUnmap, 0, PAGE_READWRITE);

    if (!NT_SUCCESS(status)) {
        SAR_ERROR("Failed to map register file to userspace %08X", status);
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
    SarEndpoint *endpoint = (SarEndpoint *)pin->Context;
    SarEndpointRegisters regs = {};
    LIST_ENTRY toRemoveList;

    SAR_DEBUG("close pin");

    if (!NT_SUCCESS(SarReadEndpointRegisters(&regs, endpoint))) {
        SAR_ERROR("Couldn't increment endpoint generation");
    } else {
        regs.generation =
            MAKE_GENERATION(GENERATION_NUMBER(regs.generation) + 1, FALSE);

        if (!NT_SUCCESS(SarWriteEndpointRegisters(&regs, endpoint))) {
            SAR_ERROR("Couldn't clear endpoint registers");
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
    endpoint->activeChannelCount = 0;
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
}

NTSTATUS SarKsPinConnect(PKSPIN pin)
{
    UNREFERENCED_PARAMETER(pin);
    return STATUS_SUCCESS;
}

VOID SarKsPinDisconnect(PKSPIN pin)
{
    UNREFERENCED_PARAMETER(pin);
}

NTSTATUS SarKsPinSetDataFormat(
    PKSPIN pin,
    PKSDATAFORMAT oldFormat,
    PKSMULTIPLE_ITEM oldAttributeList,
    const KSDATARANGE *dataRange,
    const KSATTRIBUTE_LIST *attributeRange)
{
    UNREFERENCED_PARAMETER(oldFormat);
    UNREFERENCED_PARAMETER(oldAttributeList);
    UNREFERENCED_PARAMETER(attributeRange);

    NT_ASSERT(!oldFormat);

    const KSDATARANGE_AUDIO *audioRange = (const KSDATARANGE_AUDIO *)dataRange;
    PKSDATAFORMAT_WAVEFORMATEXTENSIBLE waveFormat =
        (PKSDATAFORMAT_WAVEFORMATEXTENSIBLE)pin->ConnectionFormat;

    if (waveFormat->DataFormat.FormatSize <
        sizeof(KSDATAFORMAT_WAVEFORMATEXTENSIBLE)) {

        SAR_WARNING("WAVE Format setwith invalid sized type: %d", waveFormat->DataFormat.FormatSize);
        return STATUS_NO_MATCH;
    }

    if (waveFormat->WaveFormatExt.Format.wFormatTag !=
            WAVE_FORMAT_EXTENSIBLE ||
        waveFormat->WaveFormatExt.Format.nChannels >
            audioRange->MaximumChannels ||
        waveFormat->WaveFormatExt.Format.nSamplesPerSec !=
            audioRange->MaximumSampleFrequency ||
        waveFormat->WaveFormatExt.Samples.wValidBitsPerSample !=
            audioRange->MaximumBitsPerSample ||
        waveFormat->WaveFormatExt.SubFormat != KSDATAFORMAT_SUBTYPE_PCM) {

        SAR_DEBUG("WAVE Format set type can't be handled: "
            "channels: %d, bitsPerSample: %d, formatTag: 0x%x, samplesPerSec: %d, subFormat: " GUID_FORMAT ", maxChannel: %d, maxSampleRate: %d, maxBitsPerSample: %d",
            waveFormat->WaveFormatExt.Format.nChannels,
            waveFormat->WaveFormatExt.Format.wBitsPerSample,
            waveFormat->WaveFormatExt.Format.wFormatTag,
            waveFormat->WaveFormatExt.Format.nSamplesPerSec,
            GUID_VALUES(waveFormat->WaveFormatExt.SubFormat),
            audioRange->MaximumChannels,
            audioRange->MaximumSampleFrequency,
            audioRange->MaximumBitsPerSample);

        return STATUS_NO_MATCH;
    }

    SAR_DEBUG("WAVE Format SET: "
        "channels: %d, bitsPerSample: %d, formatTag: 0x%x, samplesPerSec: %d, subFormat: " GUID_FORMAT,
        waveFormat->WaveFormatExt.Format.nChannels,
        waveFormat->WaveFormatExt.Format.wBitsPerSample,
        waveFormat->WaveFormatExt.Format.wFormatTag,
        waveFormat->WaveFormatExt.Format.nSamplesPerSec,
        GUID_VALUES(waveFormat->WaveFormatExt.SubFormat));

    return STATUS_SUCCESS;
}

NTSTATUS SarKsPinSetDeviceState(PKSPIN pin, KSSTATE toState, KSSTATE fromState)
{
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
            SAR_ERROR("Can't read endpoint registers: %08X", status);
            return status;
        }

        regs.generation =
            MAKE_GENERATION(GENERATION_NUMBER(regs.generation), isActive);

        if (resetPosition) {
            regs.positionRegister = 0;
        }

        status = SarWriteEndpointRegisters(&regs, endpoint);

        if (!NT_SUCCESS(status)) {
            SAR_ERROR("Can't write endpoint registers: %08X", status);
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

            SAR_TRACE("KSProperty: Set " GUID_FORMAT " Id %lu Flags %lu",
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
    DWORD channelMask;

    UNREFERENCED_PARAMETER(context);
    UNREFERENCED_PARAMETER(irp);
    UNREFERENCED_PARAMETER(pin);
    PKSDATARANGE_AUDIO callerFormat = nullptr;
    PKSDATARANGE_AUDIO myFormat = nullptr;

    {
        SarEndpoint *endpoint = SarGetEndpointFromIrp(irp, TRUE);
        if (!endpoint) {
            SAR_ERROR("Failed to find endpoint for pin");
            return STATUS_NOT_FOUND;
        }
        channelMask = endpoint->channelMask;
        SarReleaseEndpointAndContext(endpoint);
    }

    *dataSize = sizeof(KSDATAFORMAT_WAVEFORMATEXTENSIBLE);

    if (callerDataRange->FormatSize == sizeof(KSDATARANGE_AUDIO) &&
        callerDataRange->MajorFormat == KSDATAFORMAT_TYPE_AUDIO) {
        callerFormat = (PKSDATARANGE_AUDIO)callerDataRange;
        SAR_INFO("callerFormat: %lu-%lu Hz, %lu-%lu bits, x%lu",
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
        SAR_ERROR("Bad format, not audio");
        return STATUS_NO_MATCH;
    }

    if (!myFormat || !callerFormat) {
        SAR_ERROR("Bad format, not audio 2");
        return STATUS_NO_MATCH;
    }

    if (dataBufferSize == 0) {
        irp->IoStatus.Information = sizeof(KSDATAFORMAT_WAVEFORMATEXTENSIBLE);
        return STATUS_BUFFER_OVERFLOW;
    }

    if (dataBufferSize < sizeof(KSDATAFORMAT_WAVEFORMATEXTENSIBLE)) {
        SAR_WARNING("Intersect buffer too small: %d", dataBufferSize);
        return STATUS_BUFFER_TOO_SMALL;
    }

    if (callerFormat->MaximumBitsPerSample < myFormat->MinimumBitsPerSample ||
        callerFormat->MinimumBitsPerSample > myFormat->MaximumBitsPerSample ||
        (callerFormat->MaximumSampleFrequency < myFormat->MinimumSampleFrequency) ||
        (callerFormat->MinimumSampleFrequency > myFormat->MaximumSampleFrequency) ||
        callerFormat->MaximumChannels < 1) {
        SAR_WARNING("Intersect failed, no match with myFormat: %lu-%lu Hz, %lu-%lu bits, x%lu",
            myFormat->MinimumSampleFrequency,
            myFormat->MaximumSampleFrequency,
            myFormat->MinimumBitsPerSample,
            myFormat->MaximumBitsPerSample,
            myFormat->MaximumChannels);
        return STATUS_NO_MATCH;
    }

    PKSDATAFORMAT_WAVEFORMATEXTENSIBLE waveFormat =
        (PKSDATAFORMAT_WAVEFORMATEXTENSIBLE)data;

    RtlCopyMemory(
        &waveFormat->DataFormat, &myFormat->DataRange, sizeof(KSDATAFORMAT));

    waveFormat->WaveFormatExt.Format.nChannels = channelMask ? getnbits(channelMask) : (WORD)myFormat->MaximumChannels;
    if (waveFormat->WaveFormatExt.Format.nChannels > myFormat->MaximumChannels) {
        waveFormat->WaveFormatExt.Format.nChannels = (WORD)myFormat->MaximumChannels;
    }
    else if (waveFormat->WaveFormatExt.Format.nChannels > callerFormat->MaximumChannels) {
        waveFormat->WaveFormatExt.Format.nChannels = (WORD)callerFormat->MaximumChannels;
    }

    waveFormat->WaveFormatExt.Format.nSamplesPerSec = min(callerFormat->MaximumSampleFrequency, myFormat->MaximumSampleFrequency);
    waveFormat->WaveFormatExt.Format.wBitsPerSample = (WORD)min(callerFormat->MaximumBitsPerSample, myFormat->MaximumBitsPerSample);
    waveFormat->WaveFormatExt.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    waveFormat->WaveFormatExt.dwChannelMask = 0;

    waveFormat->WaveFormatExt.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    waveFormat->WaveFormatExt.SubFormat = KSDATAFORMAT_SUBTYPE_PCM;
    waveFormat->DataFormat.FormatSize = sizeof(KSDATAFORMAT_WAVEFORMATEXTENSIBLE);

    waveFormat->WaveFormatExt.Samples.wValidBitsPerSample =
        waveFormat->WaveFormatExt.Format.wBitsPerSample;
    waveFormat->WaveFormatExt.Format.nBlockAlign =
        (waveFormat->WaveFormatExt.Format.wBitsPerSample / 8) *
        waveFormat->WaveFormatExt.Format.nChannels;
    waveFormat->DataFormat.SampleSize =
        waveFormat->WaveFormatExt.Format.nBlockAlign;
    waveFormat->WaveFormatExt.Format.nAvgBytesPerSec =
        waveFormat->WaveFormatExt.Format.nBlockAlign *
        waveFormat->WaveFormatExt.Format.nSamplesPerSec;

    SAR_DEBUG("WAVE Intersected Format: "
        "channels: %d, bitsPerSample: %d, formatTag: 0x%x, samplesPerSec: %d, subFormat: " GUID_FORMAT,
        waveFormat->WaveFormatExt.Format.nChannels,
        waveFormat->WaveFormatExt.Format.wBitsPerSample,
        waveFormat->WaveFormatExt.Format.wFormatTag,
        waveFormat->WaveFormatExt.Format.nSamplesPerSec,
        GUID_VALUES(waveFormat->WaveFormatExt.SubFormat));


    return STATUS_SUCCESS;
}

NTSTATUS SarKsPinGetGlobalInstancesCount(
    PIRP irp, PKSIDENTIFIER request, PVOID data)
{
    PKSP_PIN pinRequest = (PKSP_PIN)request;
    PKSPIN_CINSTANCES instances = (PKSPIN_CINSTANCES)data;
    SarEndpoint *endpoint = SarGetEndpointFromIrp(irp, TRUE);

    if (!endpoint) {
        SAR_ERROR("Failed to find endpoint for pin");
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
        SAR_ERROR("Failed to find endpoint for pin");
        return STATUS_NOT_FOUND;
    }

    if (outputLength < sizeof(KSDATAFORMAT_WAVEFORMATEXTENSIBLE)) {
        if (outputLength) {
            SAR_WARNING("Format type can't be handled, data size %d < KSDATAFORMAT_WAVEFORMATEXTENSIBLE", outputLength);
        }
        irp->IoStatus.Information = sizeof(KSDATAFORMAT_WAVEFORMATEXTENSIBLE);
        SarReleaseEndpointAndContext(endpoint);
        if (outputLength)
            return STATUS_BUFFER_TOO_SMALL;
        else
            return STATUS_BUFFER_OVERFLOW;
    }

    PKSDATAFORMAT_WAVEFORMATEXTENSIBLE waveFormat =
        (PKSDATAFORMAT_WAVEFORMATEXTENSIBLE)data;
    PKSDATARANGE_AUDIO myFormat = &endpoint->filterDescriptor.digitalDataRange;

    RtlCopyMemory(
        &waveFormat->DataFormat, &myFormat->DataRange, sizeof(KSDATAFORMAT));

    waveFormat->WaveFormatExt.Format.nChannels = endpoint->channelMask ? getnbits(endpoint->channelMask) : (WORD)myFormat->MaximumChannels;
    waveFormat->WaveFormatExt.Format.nSamplesPerSec =
        myFormat->MaximumSampleFrequency;
    waveFormat->WaveFormatExt.Format.wBitsPerSample =
        (WORD)myFormat->MaximumBitsPerSample;
    waveFormat->WaveFormatExt.Format.cbSize =
        sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    waveFormat->WaveFormatExt.dwChannelMask = endpoint->channelMask;

    waveFormat->WaveFormatExt.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    waveFormat->WaveFormatExt.SubFormat = KSDATAFORMAT_SUBTYPE_PCM;
    waveFormat->DataFormat.FormatSize = sizeof(KSDATAFORMAT_WAVEFORMATEXTENSIBLE);

    waveFormat->WaveFormatExt.Samples.wValidBitsPerSample =
        waveFormat->WaveFormatExt.Format.wBitsPerSample;
    waveFormat->WaveFormatExt.Format.nBlockAlign =
        (waveFormat->WaveFormatExt.Format.wBitsPerSample / 8) *
        waveFormat->WaveFormatExt.Format.nChannels;
    waveFormat->DataFormat.SampleSize =
        waveFormat->WaveFormatExt.Format.nBlockAlign;
    waveFormat->WaveFormatExt.Format.nAvgBytesPerSec =
        waveFormat->WaveFormatExt.Format.nBlockAlign *
        waveFormat->WaveFormatExt.Format.nSamplesPerSec;

    SAR_DEBUG("WAVE Default Format: "
        "channels: %d, bitsPerSample: %d, formatTag: 0x%x, samplesPerSec: %d, subFormat: " GUID_FORMAT,
        waveFormat->WaveFormatExt.Format.nChannels,
        waveFormat->WaveFormatExt.Format.wBitsPerSample,
        waveFormat->WaveFormatExt.Format.wFormatTag,
        waveFormat->WaveFormatExt.Format.nSamplesPerSec,
        GUID_VALUES(waveFormat->WaveFormatExt.SubFormat));

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
        SAR_ERROR("Failed to find endpoint for pin");
        return STATUS_NOT_FOUND;
    }

    if (outputLength < sizeof(KSDATAFORMAT_WAVEFORMATEXTENSIBLE)) {
        if (outputLength) {
            SAR_WARNING("Format type can't be handled, data size %d < KSDATAFORMAT_WAVEFORMATEXTENSIBLE", outputLength);
        }
        irp->IoStatus.Information = sizeof(KSDATAFORMAT_WAVEFORMATEXTENSIBLE);
        SarReleaseEndpointAndContext(endpoint);
        if (outputLength)
            return STATUS_BUFFER_TOO_SMALL;
        else
            return STATUS_BUFFER_OVERFLOW;
    }

    PKSDATAFORMAT_WAVEFORMATEXTENSIBLE format =
        (PKSDATAFORMAT_WAVEFORMATEXTENSIBLE)data;

    if (format->DataFormat.MajorFormat != KSDATAFORMAT_TYPE_AUDIO ||
        format->DataFormat.SubFormat != KSDATAFORMAT_SUBTYPE_PCM ||
        format->DataFormat.Specifier != KSDATAFORMAT_SPECIFIER_WAVEFORMATEX) {
        SAR_WARNING("Format type can't be handled %lu %lu %lu"
            GUID_FORMAT " " GUID_FORMAT " " GUID_FORMAT,
            format->DataFormat.FormatSize,
            (ULONG)sizeof(KSDATAFORMAT_WAVEFORMATEX),
            (ULONG)sizeof(KSDATAFORMAT),
            GUID_VALUES(format->DataFormat.MajorFormat),
            GUID_VALUES(format->DataFormat.SubFormat),
            GUID_VALUES(format->DataFormat.Specifier));
        SarReleaseEndpointAndContext(endpoint);
        return STATUS_NO_MATCH;
    }

    if (format->WaveFormatExt.Format.nChannels > endpoint->channelCount ||
        (format->WaveFormatExt.Format.wBitsPerSample !=
         endpoint->owner->sampleSize * 8) ||
        format->WaveFormatExt.Format.wFormatTag != WAVE_FORMAT_EXTENSIBLE ||
        (format->WaveFormatExt.Format.nSamplesPerSec !=
         endpoint->owner->sampleRate) ||
        format->WaveFormatExt.SubFormat != KSDATAFORMAT_SUBTYPE_PCM) {
        SAR_DEBUG("WAVE Format type can't be handled: "
                "channels: %d, bitsPerSample: %d, formatTag: 0x%x, samplesPerSec: %d, subFormat: " GUID_FORMAT,
            format->WaveFormatExt.Format.nChannels,
            format->WaveFormatExt.Format.wBitsPerSample,
            format->WaveFormatExt.Format.wFormatTag,
            format->WaveFormatExt.Format.nSamplesPerSec,
            GUID_VALUES(format->WaveFormatExt.SubFormat));

        SarReleaseEndpointAndContext(endpoint);
        return STATUS_NO_MATCH;
    }


    SAR_DEBUG("WAVE Format valid: "
        "channels: %d, bitsPerSample: %d, formatTag: 0x%x, samplesPerSec: %d, subFormat: " GUID_FORMAT,
        format->WaveFormatExt.Format.nChannels,
        format->WaveFormatExt.Format.wBitsPerSample,
        format->WaveFormatExt.Format.wFormatTag,
        format->WaveFormatExt.Format.nSamplesPerSec,
        GUID_VALUES(format->WaveFormatExt.SubFormat));

    SarReleaseEndpointAndContext(endpoint);
    return STATUS_SUCCESS;
}

NTSTATUS SarKsNodeGetAudioChannelConfig(
    PIRP irp, PKSIDENTIFIER request, PVOID data)
{
    PKSNODEPROPERTY propertyRequest = (PKSNODEPROPERTY)request;
    PIO_STACK_LOCATION irpStack = IoGetCurrentIrpStackLocation(irp);
    ULONG outputLength =
        irpStack->Parameters.DeviceIoControl.OutputBufferLength;

    // Only one node: DAC or ADC
    if (propertyRequest->NodeId != 0) {
        return STATUS_NOT_FOUND;
    }

    SarEndpoint *endpoint = SarGetEndpointFromIrp(irp, TRUE);

    if (!endpoint) {
        SAR_ERROR("Failed to find endpoint for pin");
        return STATUS_NOT_FOUND;
    }

    if (outputLength < sizeof(KSAUDIO_CHANNEL_CONFIG)) {
        if (outputLength) {
            SAR_WARNING("SetAudioChannelConfig: buffer too small: %d, ", outputLength, irpStack->Parameters.DeviceIoControl.InputBufferLength);
        }
        SarReleaseEndpointAndContext(endpoint);
        return STATUS_BUFFER_TOO_SMALL;
    }

    KSAUDIO_CHANNEL_CONFIG* format = (KSAUDIO_CHANNEL_CONFIG*)data;

    // Get the channel mask.
    format->ActiveSpeakerPositions = endpoint->channelMask;

    SarReleaseEndpointAndContext(endpoint);
    return STATUS_SUCCESS;
}

NTSTATUS SarKsNodeSetAudioChannelConfig(
    PIRP irp, PKSIDENTIFIER request, PVOID data)
{
    PKSNODEPROPERTY propertyRequest = (PKSNODEPROPERTY)request;
    PIO_STACK_LOCATION irpStack = IoGetCurrentIrpStackLocation(irp);
    ULONG outputLength =
        irpStack->Parameters.DeviceIoControl.OutputBufferLength;

    // Only one node: DAC or ADC
    if (propertyRequest->NodeId != 0) {
        return STATUS_NOT_FOUND;
    }

    SarEndpoint *endpoint = SarGetEndpointFromIrp(irp, TRUE);

    if (!endpoint) {
        SAR_ERROR("Failed to find endpoint for pin");
        return STATUS_NOT_FOUND;
    }

    if (outputLength < sizeof(KSAUDIO_CHANNEL_CONFIG)) {
        if (outputLength) {
            SAR_WARNING("SetAudioChannelConfig: buffer too small: %d, ", outputLength, irpStack->Parameters.DeviceIoControl.InputBufferLength);
        }
        SarReleaseEndpointAndContext(endpoint);
        return STATUS_BUFFER_TOO_SMALL;
    }

    KSAUDIO_CHANNEL_CONFIG* format = (KSAUDIO_CHANNEL_CONFIG*)data;

    if(getnbits(format->ActiveSpeakerPositions) > endpoint->channelCount) {
        SAR_ERROR("Channel Mask not supported: 0x%x, too many channels (max: %d)", format, endpoint->channelCount);
        SarReleaseEndpointAndContext(endpoint);
        return STATUS_NOT_SUPPORTED;
    }

    // Store the new channel mask.
    endpoint->channelMask = format->ActiveSpeakerPositions;
    SAR_DEBUG("Setting channel mask to 0x%x", endpoint->channelMask);

    SarReleaseEndpointAndContext(endpoint);
    return STATUS_SUCCESS;
}