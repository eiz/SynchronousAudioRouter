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

const KSPIN_DISPATCH gPinDispatch = {
    SarKsPinCreate, // Create
    SarKsPinClose, // Close
    SarKsPinProcess, // Process
    SarKsPinReset, // Reset
    SarKsPinSetDataFormat, // SetDataFormat
    SarKsPinSetDeviceState, // SetDeviceState
    SarKsPinConnect, // Connect
    SarKsPinDisconnect, // Disconnect
    nullptr, // Clock
    nullptr, // Allocator
};

DECLARE_SIMPLE_FRAMING_EX(
    gAllocatorFraming,
    STATICGUIDOF(KSMEMORY_TYPE_KERNEL_NONPAGED),
    KSALLOCATOR_REQUIREMENTF_SYSTEM_MEMORY |
    KSALLOCATOR_REQUIREMENTF_PREFERENCES_ONLY,
    25,
    0,
    2 * PAGE_SIZE,
    2 * PAGE_SIZE);

const KSPIN_DESCRIPTOR_EX gPinDescriptorTemplate = {
    &gPinDispatch, // Dispatch
    nullptr, // AutomationTable
    {}, // PinDescriptor
    KSPIN_FLAG_DO_NOT_INITIATE_PROCESSING |
    KSPIN_FLAG_FRAMES_NOT_REQUIRED_FOR_PROCESSING |
    KSPIN_FLAG_PROCESS_IF_ANY_IN_RUN_STATE |
    KSPIN_FLAG_FIXED_FORMAT,
    1, // InstancesPossible
    0, // InstancesNecessary
    &gAllocatorFraming, // AllocatorFraming
    SarKsPinIntersectHandler, // IntersectHandler
};

const GUID gCategoriesTableCapture[] = {
    STATICGUIDOF(KSCATEGORY_CAPTURE),
    STATICGUIDOF(KSCATEGORY_AUDIO),
};

const GUID gCategoriesTableRender[] = {
    STATICGUIDOF(KSCATEGORY_RENDER),
    STATICGUIDOF(KSCATEGORY_AUDIO),
};

const KSTOPOLOGY_CONNECTION gFilterConnections[] = {
    { KSFILTER_NODE, 0, 0, 1 },
    { 0, 0, KSFILTER_NODE, 1 }
};

KSFILTER_DISPATCH gFilterDispatch = {
    nullptr, // Create
    nullptr, // Close
    nullptr, // Process
    nullptr, // Reset
};

DEFINE_KSPROPERTY_TABLE(gFilterPinProperties) {
    DEFINE_KSPROPERTY_ITEM(
        KSPROPERTY_PIN_GLOBALCINSTANCES,
        SarKsPinGetGlobalInstancesCount,
        sizeof(KSP_PIN), sizeof(KSPIN_CINSTANCES),
        nullptr, nullptr, 0, nullptr, nullptr, 0),
    DEFINE_KSPROPERTY_ITEM(
        KSPROPERTY_PIN_PROPOSEDATAFORMAT,
        nullptr, sizeof(KSP_PIN), sizeof(KSDATAFORMAT_WAVEFORMATEX),
        SarKsPinProposeDataFormat, nullptr, 0, nullptr, nullptr, 0)
};

DEFINE_KSPROPERTY_SET_TABLE(gFilterPropertySets) {
    DEFINE_KSPROPERTY_SET(
        &KSPROPSETID_Pin,
        SIZEOF_ARRAY(gFilterPinProperties),
        gFilterPinProperties,
        0,
        nullptr)
};

DEFINE_KSAUTOMATION_TABLE(gFilterAutomation) {
    DEFINE_KSAUTOMATION_PROPERTIES(gFilterPropertySets),
    DEFINE_KSAUTOMATION_METHODS_NULL,
    DEFINE_KSAUTOMATION_EVENTS_NULL,
};

static KSFILTER_DESCRIPTOR gFilterDescriptorTemplate = {
    &gFilterDispatch, // Dispatch
    &gFilterAutomation, // AutomationTable
    KSFILTER_DESCRIPTOR_VERSION, // Version
    0, // Flags
    nullptr, // ReferenceGuid
    0, // PinDescriptorsCount
    0, // PinDescriptorSize
    nullptr,
    DEFINE_KSFILTER_CATEGORIES_NULL,
    DEFINE_KSFILTER_NODE_DESCRIPTORS_NULL,
    DEFINE_KSFILTER_CONNECTIONS(gFilterConnections),
    nullptr, // ComponentId
};

BOOL SarCheckIoctlInput(
    NTSTATUS *status, PIO_STACK_LOCATION irpStack, ULONG size)
{
    ULONG length = irpStack->Parameters.DeviceIoControl.InputBufferLength;

    if (length < size) {
        if (status) {
            *status = STATUS_BUFFER_TOO_SMALL;
        }

        return FALSE;
    }

    return TRUE;
}

BOOLEAN SarIoctlInput(
    NTSTATUS *status, PIRP irp, PIO_STACK_LOCATION irpStack, PVOID *buffer,
    ULONG size)
{
    UNREFERENCED_PARAMETER(buffer);

    if (!status) {
        return FALSE;
    }

    if (!SarCheckIoctlInput(status, irpStack, size)) {
        return FALSE;
    }

    RtlCopyMemory(buffer, irp->AssociatedIrp.SystemBuffer, size);
    return NT_SUCCESS(*status);
}

NTSTATUS SarSetBufferLayout(
    SarFileContext *fileContext,
    SarSetBufferLayoutRequest *request)
{
    if (request->bufferCount > SAR_MAX_BUFFER_COUNT ||
        request->bufferSize > SAR_MAX_BUFFER_SIZE ||
        request->sampleDepth < SAR_MIN_SAMPLE_DEPTH ||
        request->sampleDepth > SAR_MAX_SAMPLE_DEPTH ||
        request->sampleRate < SAR_MIN_SAMPLE_RATE ||
        request->sampleRate > SAR_MAX_SAMPLE_RATE) {
        return STATUS_INVALID_PARAMETER;
    }

    ExAcquireFastMutex(&fileContext->mutex);
    fileContext->bufferCount = request->bufferCount;
    fileContext->bufferSize = request->bufferSize;
    fileContext->sampleDepth = request->sampleDepth;
    fileContext->sampleRate = request->sampleRate;
    ExReleaseFastMutex(&fileContext->mutex);
    return STATUS_SUCCESS;
}

// on driver entry: create ks device object
// on create endpoint: create ks filter & ks pin
//  - audio category
//    - for capture endpoints use KSCATEGORY_CAPTURE
//    - for playback endpoints use KSCATEGORY_RENDER
//  - filter has 1 pin
//  - pin has possibly multichannel data format (KSDATARANGE)
//  - pin supports a single KSDATARANGE
//  - pin flags: KSPIN_FLAG_DO_NOT_INITIATE_PROCESSING KSPIN_FLAG_PROCESS_IN_RUN_STATE_ONLY KSPIN_FLAG_FIXED_FORMAT KSPIN_FLAG_FRAMES_NOT_REQUIRED_FOR_PROCESSING

NTSTATUS SarSetDeviceInterfaceProperties(
    SarEndpoint *endpoint,
    PUNICODE_STRING symbolicLinkName,
    const GUID *aliasInterfaceClassGuid)
{
    NTSTATUS status;
    HANDLE deviceInterfaceKey = nullptr;
    UNICODE_STRING clsidValue, clsidData = {}, aliasLink = {};

    status = IoGetDeviceInterfaceAlias(
        symbolicLinkName, aliasInterfaceClassGuid, &aliasLink);

    if (!NT_SUCCESS(status)) {
        SAR_LOG("Couldn't get device alias: %08X", status);
        goto out;
    }

    SAR_LOG("Setting interface properties for %wZ", &aliasLink);

    status = IoSetDeviceInterfacePropertyData(&aliasLink,
        &DEVPKEY_DeviceInterface_FriendlyName, LOCALE_NEUTRAL, 0,
        DEVPROP_TYPE_STRING,
        endpoint->pendingDeviceName.Length + sizeof(WCHAR),
        endpoint->pendingDeviceName.Buffer);

    if (!NT_SUCCESS(status)) {
        SAR_LOG("Couldn't set friendly name: %08X", status);
        goto out;
    }

    status = IoOpenDeviceInterfaceRegistryKey(
        &aliasLink, KEY_ALL_ACCESS, &deviceInterfaceKey);

    if (!NT_SUCCESS(status)) {
        SAR_LOG("Couldn't open registry key: %08X", status);
        goto out;
    }

    RtlUnicodeStringInit(&clsidValue, L"CLSID");
    status = RtlStringFromGUID(CLSID_Proxy, &clsidData);

    if (!NT_SUCCESS(status)) {
        SAR_LOG("Couldn't convert GUID to string: %08X", status);
        goto out;
    }

    status = ZwSetValueKey(
        deviceInterfaceKey, &clsidValue, 0, REG_SZ, clsidData.Buffer,
        clsidData.Length + sizeof(UNICODE_NULL));

    if (!NT_SUCCESS(status)) {
        SAR_LOG("Couldn't set CLSID: %08X", status);
        goto out;
    }

out:
    if (clsidData.Buffer) {
        RtlFreeUnicodeString(&clsidData);
    }

    if (deviceInterfaceKey) {
        ZwClose(deviceInterfaceKey);
    }

    if (aliasLink.Buffer) {
        RtlFreeUnicodeString(&aliasLink);
    }

    return status;
}

VOID SarProcessPendingEndpoints(PDEVICE_OBJECT deviceObject, PVOID context)
{
    UNREFERENCED_PARAMETER(deviceObject);
    NTSTATUS status = STATUS_UNSUCCESSFUL;
    SarFileContext *fileContext = (SarFileContext *)context;

    ExAcquireFastMutex(&fileContext->mutex);

retry:
    PLIST_ENTRY entry = fileContext->pendingEndpointList.Flink;

    while (entry != &fileContext->pendingEndpointList) {
        SarEndpoint *endpoint = CONTAINING_RECORD(entry, SarEndpoint, listEntry);
        PUNICODE_STRING symlink =
            KsFilterFactoryGetSymbolicLink(endpoint->filterFactory);
        PLIST_ENTRY current = entry;

        entry = endpoint->listEntry.Flink;
        RemoveEntryList(current);
        ExReleaseFastMutex(&fileContext->mutex);

        status = SarSetDeviceInterfaceProperties(
            endpoint, symlink, &KSCATEGORY_AUDIO);

        if (!NT_SUCCESS(status)) {
            goto out;
        }

        status = SarSetDeviceInterfaceProperties(
            endpoint, symlink, endpoint->type == SAR_ENDPOINT_TYPE_PLAYBACK ?
            &KSCATEGORY_RENDER : &KSCATEGORY_CAPTURE);

        if (!NT_SUCCESS(status)) {
            goto out;
        }

        status = KsFilterFactorySetDeviceClassesState(
            endpoint->filterFactory, TRUE);

        if (!NT_SUCCESS(status)) {
            SAR_LOG("Couldn't enable KS filter factory");
            goto out;
        }

out:
        ExAcquireFastMutex(&fileContext->mutex);

        if (NT_SUCCESS(status)) {
            InsertTailList(&fileContext->endpointList, &endpoint->listEntry);
        } else {
            // TODO: delete failed endpoint
        }

        endpoint->pendingIrp->IoStatus.Status = status;
        IoCompleteRequest(endpoint->pendingIrp, IO_NO_INCREMENT);
    }

    // Someone added a new endpoint request while we were working with locks
    // dropped. Try again.
    if (!IsListEmpty(&fileContext->pendingEndpointList)) {
        goto retry;
    }

    ExReleaseFastMutex(&fileContext->mutex);
}

NTSTATUS SarCreateEndpoint(
    PDEVICE_OBJECT device,
    PIRP irp,
    SarDriverExtension *extension,
    SarFileContext *fileContext,
    SarCreateEndpointRequest *request)
{
    UNREFERENCED_PARAMETER(fileContext);
    WCHAR buf[20] = {};
    UNICODE_STRING referenceString = { 0, sizeof(buf), buf };
    NTSTATUS status = STATUS_INSUFFICIENT_RESOURCES;
    PKSDEVICE ksDevice = KsGetDeviceForDeviceObject(device);
    SarEndpoint *endpoint;

    if (request->type != SAR_ENDPOINT_TYPE_RECORDING &&
        request->type != SAR_ENDPOINT_TYPE_PLAYBACK) {
        return STATUS_INVALID_PARAMETER;
    }

    if (request->channelCount > SAR_MAX_CHANNEL_COUNT) {
        return STATUS_INVALID_PARAMETER;
    }

    endpoint = (SarEndpoint *)
        ExAllocatePoolWithTag(PagedPool,
            SarEndpointSize(request->channelCount), SAR_TAG);

    if (!endpoint) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    RtlZeroMemory(endpoint, SarEndpointSize(request->channelCount));
    endpoint->pendingIrp = irp;
    endpoint->channelCount = request->channelCount;
    endpoint->type = request->type;
    endpoint->owner = fileContext;

    for (DWORD i = 0; i < endpoint->channelCount; ++i) {
        endpoint->channelMappings[i] = SAR_INVALID_BUFFER;
    }

    endpoint->filterDesc = (PKSFILTER_DESCRIPTOR)
        ExAllocatePoolWithTag(PagedPool, sizeof(KSFILTER_DESCRIPTOR), SAR_TAG);

    if (!endpoint->filterDesc) {
        goto err_out;
    }

    endpoint->pinDesc = (PKSPIN_DESCRIPTOR_EX)
        ExAllocatePoolWithTag(PagedPool, sizeof(KSPIN_DESCRIPTOR_EX) * 2, SAR_TAG);

    if (!endpoint->pinDesc) {
        goto err_out;
    }

    endpoint->dataRange = (PKSDATARANGE_AUDIO)
        ExAllocatePoolWithTag(PagedPool, sizeof(KSDATARANGE_AUDIO), SAR_TAG);

    if (!endpoint->dataRange) {
        goto err_out;
    }

    endpoint->analogDataRange = (PKSDATARANGE_AUDIO)
        ExAllocatePoolWithTag(PagedPool, sizeof(KSDATARANGE_AUDIO), SAR_TAG);

    if (!endpoint->analogDataRange) {
        goto err_out;
    }

    endpoint->allocatorFraming = (PKSALLOCATOR_FRAMING_EX)
        ExAllocatePoolWithTag(PagedPool, sizeof(KSALLOCATOR_FRAMING_EX), SAR_TAG);

    if (!endpoint->allocatorFraming) {
        goto err_out;
    }

    endpoint->nodeDesc = (PKSNODE_DESCRIPTOR)
        ExAllocatePoolWithTag(PagedPool, sizeof(KSNODE_DESCRIPTOR), SAR_TAG);

    if (!endpoint->nodeDesc) {
        goto err_out;
    }

    *endpoint->filterDesc = gFilterDescriptorTemplate;
    endpoint->pinDesc[0] = gPinDescriptorTemplate;
    endpoint->pinDesc[1] = gPinDescriptorTemplate;
    endpoint->filterDesc->CategoriesCount = 2;
    endpoint->filterDesc->Categories =
        request->type == SAR_ENDPOINT_TYPE_RECORDING ?
        gCategoriesTableCapture : gCategoriesTableRender;
    endpoint->filterDesc->PinDescriptors = endpoint->pinDesc;
    endpoint->filterDesc->PinDescriptorsCount = 2;
    endpoint->filterDesc->PinDescriptorSize = sizeof(KSPIN_DESCRIPTOR_EX);
    endpoint->filterDesc->NodeDescriptors = endpoint->nodeDesc;
    endpoint->filterDesc->NodeDescriptorSize = sizeof(KSNODE_DESCRIPTOR);
    endpoint->filterDesc->NodeDescriptorsCount = 1;

    PKSPIN_DESCRIPTOR pinDesc = &endpoint->pinDesc[0].PinDescriptor;

    pinDesc->DataRangesCount = 1;
    pinDesc->DataRanges = (PKSDATARANGE *)&endpoint->dataRange;
    pinDesc->Communication = KSPIN_COMMUNICATION_BOTH;
    pinDesc->DataFlow =
        request->type == SAR_ENDPOINT_TYPE_RECORDING ?
        KSPIN_DATAFLOW_OUT : KSPIN_DATAFLOW_IN;
    pinDesc->Category = &KSCATEGORY_AUDIO;

    if (request->type == SAR_ENDPOINT_TYPE_PLAYBACK) {
        endpoint->pinDesc[0].Flags |= KSPIN_FLAG_RENDERER;
    }

    pinDesc = &endpoint->pinDesc[1].PinDescriptor;
    endpoint->pinDesc[1].IntersectHandler = nullptr;
    endpoint->pinDesc[1].Dispatch = nullptr;
    pinDesc->DataRangesCount = 1;
    pinDesc->DataRanges = (PKSDATARANGE *)&endpoint->analogDataRange;
    pinDesc->Communication = KSPIN_COMMUNICATION_NONE;
    pinDesc->Category = &KSNODETYPE_LINE_CONNECTOR;
    pinDesc->DataFlow =
        request->type == SAR_ENDPOINT_TYPE_RECORDING ?
        KSPIN_DATAFLOW_IN : KSPIN_DATAFLOW_OUT;

    endpoint->nodeDesc->AutomationTable = nullptr;
    endpoint->nodeDesc->Type =
        request->type == SAR_ENDPOINT_TYPE_RECORDING ?
        &KSNODETYPE_ADC : &KSNODETYPE_DAC;
    endpoint->nodeDesc->Name = nullptr;

    endpoint->dataRange->DataRange.FormatSize = sizeof(KSDATARANGE_AUDIO);
    endpoint->dataRange->DataRange.Flags = 0;
    endpoint->dataRange->DataRange.SampleSize = 0;
    endpoint->dataRange->DataRange.Reserved = 0;
    endpoint->dataRange->DataRange.MajorFormat = KSDATAFORMAT_TYPE_AUDIO;
    endpoint->dataRange->DataRange.SubFormat = KSDATAFORMAT_SUBTYPE_PCM;
    endpoint->dataRange->DataRange.Specifier =
        KSDATAFORMAT_SPECIFIER_WAVEFORMATEX;
    endpoint->dataRange->MaximumBitsPerSample = fileContext->sampleDepth * 8;
    endpoint->dataRange->MinimumBitsPerSample = fileContext->sampleDepth * 8;
    endpoint->dataRange->MaximumSampleFrequency = fileContext->sampleRate;
    endpoint->dataRange->MinimumSampleFrequency = fileContext->sampleRate;
    endpoint->dataRange->MaximumChannels = request->channelCount;

    endpoint->analogDataRange->DataRange.FormatSize = sizeof(KSDATARANGE_AUDIO);
    endpoint->analogDataRange->DataRange.Flags = 0;
    endpoint->analogDataRange->DataRange.SampleSize = 0;
    endpoint->analogDataRange->DataRange.Reserved = 0;
    endpoint->analogDataRange->DataRange.MajorFormat = KSDATAFORMAT_TYPE_AUDIO;
    endpoint->analogDataRange->DataRange.SubFormat = KSDATAFORMAT_SUBTYPE_ANALOG;
    endpoint->analogDataRange->DataRange.Specifier = KSDATAFORMAT_SPECIFIER_NONE;
    endpoint->analogDataRange->MaximumBitsPerSample = 0;
    endpoint->analogDataRange->MinimumBitsPerSample = 0;
    endpoint->analogDataRange->MaximumSampleFrequency = 0;
    endpoint->analogDataRange->MinimumSampleFrequency = 0;
    endpoint->analogDataRange->MaximumChannels = 0;

    request->name[MAX_ENDPOINT_NAME_LENGTH] = '\0';
    RtlInitUnicodeString(&endpoint->pendingDeviceName, request->name);

    LONG filterId = InterlockedIncrement(&extension->nextFilterId);

    RtlIntegerToUnicodeString(filterId, 10, &referenceString);
    KsAcquireDevice(ksDevice);
    status = KsCreateFilterFactory(
        device, endpoint->filterDesc, buf, nullptr, KSCREATE_ITEM_FREEONSTOP,
        nullptr, nullptr, &endpoint->filterFactory);
    KsReleaseDevice(ksDevice);

    if (!NT_SUCCESS(status)) {
        goto err_out;
    }

    ExAcquireFastMutex(&fileContext->mutex);

    BOOLEAN runWorkItem = IsListEmpty(&fileContext->pendingEndpointList);

    InsertTailList(&fileContext->pendingEndpointList, &endpoint->listEntry);

    if (runWorkItem) {
        IoQueueWorkItem(
            fileContext->workItem, SarProcessPendingEndpoints, DelayedWorkQueue,
            fileContext);
    }

    ExReleaseFastMutex(&fileContext->mutex);
    return STATUS_PENDING;

err_out:
    if (endpoint->filterFactory) {
        KsAcquireDevice(ksDevice);
        KsDeleteFilterFactory(endpoint->filterFactory);
        KsReleaseDevice(ksDevice);
    }

    if (endpoint->allocatorFraming) {
        ExFreePoolWithTag(endpoint->allocatorFraming, SAR_TAG);
    }

    if (endpoint->dataRange) {
        ExFreePoolWithTag(endpoint->dataRange, SAR_TAG);
    }

    if (endpoint->analogDataRange) {
        ExFreePoolWithTag(endpoint->analogDataRange, SAR_TAG);
    }

    if (endpoint->nodeDesc) {
        ExFreePoolWithTag(endpoint->nodeDesc, SAR_TAG);
    }

    if (endpoint->pinDesc) {
        ExFreePoolWithTag(endpoint->pinDesc, SAR_TAG);
    }

    if (endpoint->filterDesc) {
        ExFreePoolWithTag(endpoint->filterDesc, SAR_TAG);
    }

    ExFreePoolWithTag(endpoint, SAR_TAG);
    return status;
}
