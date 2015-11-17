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

#define DEFINE_KSPROPERTY_GETTER(id, handler, intype, outtype) \
    DEFINE_KSPROPERTY_ITEM((id), (handler), sizeof(intype), sizeof(outtype), \
    nullptr, nullptr, 0, nullptr, nullptr, 0)

DEFINE_KSPROPERTY_TABLE(gPinRtAudioProperties) {
    DEFINE_KSPROPERTY_GETTER(
        KSPROPERTY_RTAUDIO_BUFFER, SarKsPinRtGetBuffer,
        KSRTAUDIO_BUFFER_PROPERTY, KSRTAUDIO_BUFFER),
    DEFINE_KSPROPERTY_GETTER(
        KSPROPERTY_RTAUDIO_BUFFER_WITH_NOTIFICATION,
        SarKsPinRtGetBufferWithNotification,
        KSRTAUDIO_BUFFER_PROPERTY, KSRTAUDIO_BUFFER),
    DEFINE_KSPROPERTY_GETTER(
        KSPROPERTY_RTAUDIO_CLOCKREGISTER, SarKsPinRtGetClockRegister,
        KSRTAUDIO_HWREGISTER_PROPERTY, KSRTAUDIO_HWREGISTER),
    DEFINE_KSPROPERTY_GETTER(
        KSPROPERTY_RTAUDIO_HWLATENCY, SarKsPinRtGetHwLatency,
        KSPROPERTY, KSRTAUDIO_HWLATENCY),
    DEFINE_KSPROPERTY_GETTER(
        KSPROPERTY_RTAUDIO_PACKETCOUNT, SarKsPinRtGetPacketCount,
        KSPROPERTY, ULONG),
    DEFINE_KSPROPERTY_GETTER(
        KSPROPERTY_RTAUDIO_POSITIONREGISTER, SarKsPinRtGetPositionRegister,
        KSRTAUDIO_HWREGISTER_PROPERTY, KSRTAUDIO_HWREGISTER),
    DEFINE_KSPROPERTY_GETTER(
        KSPROPERTY_RTAUDIO_PRESENTATION_POSITION,
        SarKsPinRtGetPresentationPosition,
        KSPROPERTY, KSAUDIO_PRESENTATION_POSITION),
    DEFINE_KSPROPERTY_GETTER(
        KSPROPERTY_RTAUDIO_QUERY_NOTIFICATION_SUPPORT,
        SarKsPinRtQueryNotificationSupport, KSPROPERTY, BOOL),
    DEFINE_KSPROPERTY_ITEM(
        KSPROPERTY_RTAUDIO_REGISTER_NOTIFICATION_EVENT,
        SarKsPinRtRegisterNotificationEvent,
        sizeof(KSRTAUDIO_NOTIFICATION_EVENT_PROPERTY), 0,
        SarKsPinRtRegisterNotificationEvent,
        nullptr, 0, nullptr, nullptr, 0),
    DEFINE_KSPROPERTY_ITEM(
        KSPROPERTY_RTAUDIO_UNREGISTER_NOTIFICATION_EVENT,
        SarKsPinRtUnregisterNotificationEvent,
        sizeof(KSRTAUDIO_NOTIFICATION_EVENT_PROPERTY), 0,
        SarKsPinRtUnregisterNotificationEvent,
        nullptr, 0, nullptr, nullptr, 0),
};

DEFINE_KSPROPERTY_SET_TABLE(gPinPropertySets) {
    DEFINE_KSPROPERTY_SET(
        &KSPROPSETID_RtAudio,
        SIZEOF_ARRAY(gPinRtAudioProperties),
        gPinRtAudioProperties,
        0,
        nullptr)
};

DEFINE_KSAUTOMATION_TABLE(gPinAutomation) {
    DEFINE_KSAUTOMATION_PROPERTIES(gPinPropertySets),
    DEFINE_KSAUTOMATION_METHODS_NULL,
    DEFINE_KSAUTOMATION_EVENTS_NULL
};

const KSPIN_INTERFACE gPinInterfaces[] = {
    {
        STATICGUIDOF(KSINTERFACESETID_Standard),
        KSINTERFACE_STANDARD_LOOPED_STREAMING
    }
};

const KSPIN_DESCRIPTOR_EX gPinDescriptorTemplate = {
    &gPinDispatch, // Dispatch
    nullptr, // AutomationTable
    {}, // PinDescriptor
    KSPIN_FLAG_DO_NOT_INITIATE_PROCESSING |
    KSPIN_FLAG_FRAMES_NOT_REQUIRED_FOR_PROCESSING |
    KSPIN_FLAG_PROCESS_IF_ANY_IN_RUN_STATE |
    KSPIN_FLAG_FIXED_FORMAT |
    KSPIN_FLAG_DO_NOT_USE_STANDARD_TRANSPORT,
    1, // InstancesPossible
    0, // InstancesNecessary
    nullptr, // AllocatorFraming
    SarKsPinIntersectHandler, // IntersectHandler
};

const GUID gCategoriesTableCapture[] = {
    STATICGUIDOF(KSCATEGORY_CAPTURE),
    STATICGUIDOF(KSCATEGORY_AUDIO),
    STATICGUIDOF(KSCATEGORY_REALTIME),
};

const GUID gCategoriesTableRender[] = {
    STATICGUIDOF(KSCATEGORY_RENDER),
    STATICGUIDOF(KSCATEGORY_AUDIO),
    STATICGUIDOF(KSCATEGORY_REALTIME),
};

const KSTOPOLOGY_CONNECTION gFilterConnectionsRendering[] = {
    { KSFILTER_NODE, 0, 0, 1 },
    { 0, 0, KSFILTER_NODE, 1 }
};

const KSTOPOLOGY_CONNECTION gFilterConnectionsCapture[] = {
    { KSFILTER_NODE, 1, 0, 1 },
    { 0, 0, KSFILTER_NODE, 0 }
};

KSFILTER_DISPATCH gFilterDispatch = {
    SarKsFilterCreate, // Create
    SarKsFilterClose, // Close
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
        SarKsPinGetDefaultDataFormat,
        sizeof(KSP_PIN), 0,
        SarKsPinProposeDataFormat, nullptr, 0, nullptr, nullptr, 0),
    DEFINE_KSPROPERTY_ITEM(
        KSPROPERTY_PIN_NAME,
        SarKsPinGetName,
        sizeof(KSP_PIN), 0,
        SarKsPinGetName, nullptr, 0, nullptr, nullptr, 0)
};

DEFINE_KSPROPERTY_SET_TABLE(gFilterPropertySets) {
    DEFINE_KSPROPERTY_SET(
        &KSPROPSETID_Pin,
        SIZEOF_ARRAY(gFilterPinProperties),
        gFilterPinProperties,
        0,
        nullptr)
};

DEFINE_KSEVENT_TABLE(gFilterEvents) {
    DEFINE_KSEVENT_ITEM(
        KSEVENT_PINCAPS_FORMATCHANGE,
        sizeof(KSEVENTDATA), 0,
        nullptr, nullptr, nullptr)
};

DEFINE_KSEVENT_SET_TABLE(gFilterEventSets) {
    DEFINE_KSEVENT_SET(
        &KSEVENTSETID_PinCapsChange,
        SIZEOF_ARRAY(gFilterEvents),
        gFilterEvents)
};

DEFINE_KSAUTOMATION_TABLE(gFilterAutomation) {
    DEFINE_KSAUTOMATION_PROPERTIES(gFilterPropertySets),
    DEFINE_KSAUTOMATION_METHODS_NULL,
    DEFINE_KSAUTOMATION_EVENTS(gFilterEventSets),
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
    2, // ConnectionsCount
    nullptr, // Connections
    nullptr, // ComponentId
};

NTSTATUS SarSetBufferLayout(
    SarControlContext *controlContext,
    SarSetBufferLayoutRequest *request,
    SarSetBufferLayoutResponse *response)
{
    HANDLE section = nullptr;
    OBJECT_ATTRIBUTES sectionAttributes;
    DECLARE_UNICODE_STRING_SIZE(sectionName, 128);
    LARGE_INTEGER sectionSize;
    NTSTATUS status;
    SIZE_T viewSize = 0;
    PVOID baseAddress = nullptr;
    GUID sectionGuid = {};

    if (request->bufferSize > SAR_MAX_BUFFER_SIZE ||
        request->sampleSize < SAR_MIN_SAMPLE_SIZE ||
        request->sampleSize > SAR_MAX_SAMPLE_SIZE ||
        request->sampleRate < SAR_MIN_SAMPLE_RATE ||
        request->sampleRate > SAR_MAX_SAMPLE_RATE ||
        request->frameSize > request->bufferSize ||
        request->frameSize == 0) {
        return STATUS_INVALID_PARAMETER;
    }

    if (!NT_SUCCESS(status = ExUuidCreate(&sectionGuid))) {
        return status;
    }

    ExAcquireFastMutex(&controlContext->mutex);

    if (controlContext->bufferSize) {
        ExReleaseFastMutex(&controlContext->mutex);
        return STATUS_INVALID_STATE_TRANSITION;
    }

    controlContext->bufferSize = ROUND_TO_PAGES(request->bufferSize);
    controlContext->frameSize = request->frameSize;
    controlContext->sampleSize = request->sampleSize;
    controlContext->sampleRate = request->sampleRate;
    controlContext->minimumFrameCount = request->minimumFrameCount;
    ExReleaseFastMutex(&controlContext->mutex);

    RtlUnicodeStringPrintf(
        &sectionName,
        L"\\BaseNamedObjects\\SynchronousAudioRouter_" GUID_FORMAT,
        controlContext, GUID_VALUES(sectionGuid));
    InitializeObjectAttributes(
        &sectionAttributes, &sectionName, OBJ_KERNEL_HANDLE, nullptr, nullptr);
    sectionSize.QuadPart = controlContext->bufferSize + SAR_BUFFER_CELL_SIZE;

    DWORD bufferMapSize = SarBufferMapSize(controlContext);
    // TODO: don't leak this
    PULONG bufferMap = (PULONG)ExAllocatePoolWithTag(
        NonPagedPool, bufferMapSize, SAR_TAG);

    if (!bufferMap) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    RtlZeroMemory(bufferMap, bufferMapSize);
    status = ZwCreateSection(&section,
        SECTION_MAP_READ|SECTION_MAP_WRITE|SECTION_QUERY,
        &sectionAttributes, &sectionSize, PAGE_READWRITE, SEC_COMMIT, nullptr);

    if (!NT_SUCCESS(status)) {
        SAR_LOG("Failed to allocate buffer section %08X", status);
        goto err_out;
    }

    status = ZwMapViewOfSection(section,
        ZwCurrentProcess(), &baseAddress, 0, 0, nullptr,
        &viewSize, ViewUnmap, 0, PAGE_READWRITE);

    if (!NT_SUCCESS(status)) {
        SAR_LOG("Couldn't map view of section");
        goto err_out;
    }

    response->actualSize = sectionSize.LowPart;
    response->virtualAddress = baseAddress;
    response->registerBase = sectionSize.LowPart - SAR_BUFFER_CELL_SIZE;
    ExAcquireFastMutex(&controlContext->mutex);
    RtlInitializeBitMap(&controlContext->bufferMap,
        bufferMap, SarBufferMapEntryCount(controlContext));
    controlContext->bufferSection = section;
    ExReleaseFastMutex(&controlContext->mutex);
    return STATUS_SUCCESS;

err_out:
    if (section) {
        ZwClose(section);
    }

    ExAcquireFastMutex(&controlContext->mutex);
    controlContext->bufferSize =
        controlContext->sampleSize = controlContext->sampleRate = 0;
    ExReleaseFastMutex(&controlContext->mutex);
    return status;
}

NTSTATUS SarSetDeviceInterfaceProperties(
    SarEndpoint *endpoint,
    PUNICODE_STRING symbolicLinkName,
    const GUID *aliasInterfaceClassGuid)
{
    NTSTATUS status;
    HANDLE deviceInterfaceKey = nullptr;
    HANDLE epSubKey = nullptr, zeroSubKey = nullptr;
    UNICODE_STRING clsidValue, clsidData = {}, aliasLink = {};
    UNICODE_STRING epSubKeyStr, zeroSubKeyStr, supportsEventModeValue;
    UNICODE_STRING associationValue, guidEmpty;
    UNICODE_STRING sarIdValue, friendlyNameValue;
    OBJECT_ATTRIBUTES oa;
    DWORD one = 1;

    RtlUnicodeStringInit(&clsidValue, L"CLSID");
    RtlUnicodeStringInit(&friendlyNameValue, L"FriendlyName");
    RtlUnicodeStringInit(&epSubKeyStr, L"MSEP");
    RtlUnicodeStringInit(&zeroSubKeyStr, L"0");
    RtlUnicodeStringInit(
        &supportsEventModeValue, L"{1DA5D803-D492-4EDD-8C23-E0C0FFEE7F0E},7");
    RtlUnicodeStringInit(
        &associationValue, L"{1DA5D803-D492-4EDD-8C23-E0C0FFEE7F0E},2");
    RtlUnicodeStringInit(
        &sarIdValue, L"{F4B15B6F-8C3F-48B6-A115-42FDE19EF05B},0");
    RtlUnicodeStringInit(
        &guidEmpty, L"{00000000-0000-0000-0000-000000000000}");

    status = IoGetDeviceInterfaceAlias(
        symbolicLinkName, aliasInterfaceClassGuid, &aliasLink);

    if (!NT_SUCCESS(status)) {
        SAR_LOG("Couldn't get device alias: %08X", status);
        goto out;
    }

    status = IoOpenDeviceInterfaceRegistryKey(
        &aliasLink, KEY_ALL_ACCESS, &deviceInterfaceKey);

    if (!NT_SUCCESS(status)) {
        SAR_LOG("Couldn't open registry key: %08X", status);
        goto out;
    }

    status = RtlStringFromGUID(CLSID_Proxy, &clsidData);

    if (!NT_SUCCESS(status)) {
        SAR_LOG("Couldn't convert GUID to string: %08X", status);
        goto out;
    }

    status = ZwSetValueKey(
        deviceInterfaceKey, &friendlyNameValue, 0, REG_SZ,
        endpoint->deviceName.Buffer,
        endpoint->deviceName.Length + sizeof(UNICODE_NULL));

    if (!NT_SUCCESS(status)) {
        SAR_LOG("Couldn't set friendly name: %08X", status);
        goto out;
    }

    status = ZwSetValueKey(
        deviceInterfaceKey, &clsidValue, 0, REG_SZ, clsidData.Buffer,
        clsidData.Length + sizeof(UNICODE_NULL));

    if (!NT_SUCCESS(status)) {
        SAR_LOG("Couldn't set CLSID: %08X", status);
        goto out;
    }

    InitializeObjectAttributes(
        &oa, &epSubKeyStr, OBJ_KERNEL_HANDLE, deviceInterfaceKey, nullptr);
    status = ZwCreateKey(
        &epSubKey, KEY_ALL_ACCESS, &oa, 0, nullptr, 0, nullptr);

    if (!NT_SUCCESS(status)) {
        SAR_LOG("Couldn't open EP subkey");
        goto out;
    }

    InitializeObjectAttributes(
        &oa, &zeroSubKeyStr, OBJ_KERNEL_HANDLE, epSubKey, nullptr);
    status = ZwCreateKey(
        &zeroSubKey, KEY_ALL_ACCESS, &oa, 0, nullptr, 0, nullptr);

    if (!NT_SUCCESS(status)) {
        SAR_LOG("Couldn't open EP\\0 subkey");
        goto out;
    }

    status = ZwSetValueKey(
        zeroSubKey, &supportsEventModeValue, 0, REG_DWORD, &one, sizeof(DWORD));

    if (!NT_SUCCESS(status)) {
        SAR_LOG("Couldn't set registry value for evented mode support");
        goto out;
    }

    status = ZwSetValueKey(
        zeroSubKey, &associationValue, 0, REG_SZ, guidEmpty.Buffer,
        guidEmpty.Length + sizeof(UNICODE_NULL));

    if (!NT_SUCCESS(status)) {
        SAR_LOG("Couldn't set kscategory association for endpoint");
    }

    status = ZwSetValueKey(
        zeroSubKey, &sarIdValue, 0, REG_SZ, endpoint->deviceId.Buffer,
        endpoint->deviceId.Length + sizeof(UNICODE_NULL));

    if (!NT_SUCCESS(status)) {
        SAR_LOG("Couldn't set sar endpoint id for endpoint");
    }

out:
    if (clsidData.Buffer) {
        RtlFreeUnicodeString(&clsidData);
    }

    if (zeroSubKey) {
        ZwClose(zeroSubKey);
    }

    if (epSubKey) {
        ZwClose(epSubKey);
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
    SarControlContext *controlContext = (SarControlContext *)context;

    ExAcquireFastMutex(&controlContext->mutex);

retry:
    PLIST_ENTRY entry = controlContext->pendingEndpointList.Flink;

    while (entry != &controlContext->pendingEndpointList) {
        SarEndpoint *endpoint =
            CONTAINING_RECORD(entry, SarEndpoint, listEntry);
        PUNICODE_STRING symlink =
            KsFilterFactoryGetSymbolicLink(endpoint->filterFactory);
        PLIST_ENTRY current = entry;

        entry = endpoint->listEntry.Flink;
        RemoveEntryList(current);
        ExReleaseFastMutex(&controlContext->mutex);

        status = SarSetDeviceInterfaceProperties(
            endpoint, symlink, &KSCATEGORY_AUDIO);

        if (!NT_SUCCESS(status)) {
            goto out;
        }

        status = SarSetDeviceInterfaceProperties(
            endpoint, symlink, &KSCATEGORY_REALTIME);

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
        ExAcquireFastMutex(&controlContext->mutex);

        if (NT_SUCCESS(status)) {
            InsertTailList(&controlContext->endpointList, &endpoint->listEntry);
        } else {
            // TODO: delete failed endpoint
        }

        endpoint->pendingIrp->IoStatus.Status = status;
        IoCompleteRequest(endpoint->pendingIrp, IO_NO_INCREMENT);
    }

    // Someone added a new endpoint request while we were working with locks
    // dropped. Try again.
    if (!IsListEmpty(&controlContext->pendingEndpointList)) {
        goto retry;
    }

    ExReleaseFastMutex(&controlContext->mutex);
    SarReleaseControlContext(controlContext);
}

NTSTATUS SarCreateEndpoint(
    PDEVICE_OBJECT device,
    PIRP irp,
    SarControlContext *controlContext,
    SarCreateEndpointRequest *request)
{
    NTSTATUS status = STATUS_SUCCESS;
    PKSDEVICE ksDevice = KsGetDeviceForDeviceObject(device);
    BOOLEAN deviceNameAllocated = FALSE, deviceIdAllocated = FALSE;
    RTL_OSVERSIONINFOW versionInfo = {};
    SarEndpoint *endpoint;

    if (request->type != SAR_ENDPOINT_TYPE_RECORDING &&
        request->type != SAR_ENDPOINT_TYPE_PLAYBACK) {
        return STATUS_INVALID_PARAMETER;
    }

    if (request->index >= SAR_MAX_ENDPOINT_COUNT ||
        request->channelCount > SAR_MAX_CHANNEL_COUNT) {
        return STATUS_INVALID_PARAMETER;
    }

    if (!NT_SUCCESS(status)) {
        return status;
    }

    status = STATUS_INSUFFICIENT_RESOURCES;
    endpoint = (SarEndpoint *)
        ExAllocatePoolWithTag(NonPagedPool, sizeof(SarEndpoint), SAR_TAG);

    if (!endpoint) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    RtlZeroMemory(endpoint, sizeof(SarEndpoint));
    endpoint->refs = 1;
    ExInitializeFastMutex(&endpoint->mutex);
    InitializeListHead(&endpoint->activeProcessList);
    endpoint->pendingIrp = irp;
    endpoint->channelCount = request->channelCount;
    endpoint->type = request->type;
    endpoint->index = request->index;
    endpoint->owner = controlContext;

    endpoint->filterDesc = (PKSFILTER_DESCRIPTOR)
        ExAllocatePoolWithTag(
            NonPagedPool, sizeof(KSFILTER_DESCRIPTOR), SAR_TAG);

    if (!endpoint->filterDesc) {
        goto err_out;
    }

    endpoint->pinDesc = (PKSPIN_DESCRIPTOR_EX)
        ExAllocatePoolWithTag(
            NonPagedPool, sizeof(KSPIN_DESCRIPTOR_EX) * 2, SAR_TAG);

    if (!endpoint->pinDesc) {
        goto err_out;
    }

    endpoint->dataRange = (PKSDATARANGE_AUDIO)
        ExAllocatePoolWithTag(NonPagedPool, sizeof(KSDATARANGE_AUDIO), SAR_TAG);

    if (!endpoint->dataRange) {
        goto err_out;
    }

    endpoint->analogDataRange = (PKSDATARANGE_AUDIO)
        ExAllocatePoolWithTag(NonPagedPool, sizeof(KSDATARANGE_AUDIO), SAR_TAG);

    if (!endpoint->analogDataRange) {
        goto err_out;
    }

    endpoint->nodeDesc = (PKSNODE_DESCRIPTOR)
        ExAllocatePoolWithTag(NonPagedPool, sizeof(KSNODE_DESCRIPTOR), SAR_TAG);

    if (!endpoint->nodeDesc) {
        goto err_out;
    }

    *endpoint->filterDesc = gFilterDescriptorTemplate;
    endpoint->pinDesc[0] = gPinDescriptorTemplate;
    endpoint->pinDesc[1] = gPinDescriptorTemplate;
    endpoint->filterDesc->CategoriesCount = 3;
    endpoint->filterDesc->Categories =
        request->type == SAR_ENDPOINT_TYPE_RECORDING ?
        gCategoriesTableCapture : gCategoriesTableRender;
    endpoint->filterDesc->PinDescriptors = endpoint->pinDesc;
    endpoint->filterDesc->PinDescriptorsCount = 2;
    endpoint->filterDesc->PinDescriptorSize = sizeof(KSPIN_DESCRIPTOR_EX);
    endpoint->filterDesc->NodeDescriptors = endpoint->nodeDesc;
    endpoint->filterDesc->NodeDescriptorSize = sizeof(KSNODE_DESCRIPTOR);
    endpoint->filterDesc->NodeDescriptorsCount = 1;
    endpoint->filterDesc->Connections =
        request->type == SAR_ENDPOINT_TYPE_RECORDING ?
        gFilterConnectionsCapture : gFilterConnectionsRendering;

    PKSPIN_DESCRIPTOR pinDesc = &endpoint->pinDesc[0].PinDescriptor;

    endpoint->pinDesc[0].AutomationTable = &gPinAutomation;
    pinDesc->DataRangesCount = 1;
    pinDesc->DataRanges = (PKSDATARANGE *)&endpoint->dataRange;
    pinDesc->Communication = KSPIN_COMMUNICATION_BOTH;
    pinDesc->DataFlow =
        request->type == SAR_ENDPOINT_TYPE_RECORDING ?
        KSPIN_DATAFLOW_OUT : KSPIN_DATAFLOW_IN;
    pinDesc->Category = &KSCATEGORY_AUDIO;
    pinDesc->InterfacesCount = 1;
    pinDesc->Interfaces = gPinInterfaces;

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
    endpoint->dataRange->MaximumBitsPerSample = controlContext->sampleSize * 8;
    endpoint->dataRange->MinimumBitsPerSample = controlContext->sampleSize * 8;
    endpoint->dataRange->MaximumSampleFrequency = controlContext->sampleRate;
    endpoint->dataRange->MinimumSampleFrequency = controlContext->sampleRate;
    endpoint->dataRange->MaximumChannels = request->channelCount;

    endpoint->analogDataRange->DataRange.FormatSize = sizeof(KSDATARANGE_AUDIO);
    endpoint->analogDataRange->DataRange.Flags = 0;
    endpoint->analogDataRange->DataRange.SampleSize = 0;
    endpoint->analogDataRange->DataRange.Reserved = 0;
    endpoint->analogDataRange->DataRange.MajorFormat = KSDATAFORMAT_TYPE_AUDIO;
    endpoint->analogDataRange->DataRange.SubFormat =
        KSDATAFORMAT_SUBTYPE_ANALOG;
    endpoint->analogDataRange->DataRange.Specifier =
        KSDATAFORMAT_SPECIFIER_NONE;
    endpoint->analogDataRange->MaximumBitsPerSample = 0;
    endpoint->analogDataRange->MinimumBitsPerSample = 0;
    endpoint->analogDataRange->MaximumSampleFrequency = 0;
    endpoint->analogDataRange->MinimumSampleFrequency = 0;
    endpoint->analogDataRange->MaximumChannels = 0;

    request->name[MAX_ENDPOINT_NAME_LENGTH] = '\0';
    request->id[MAX_ENDPOINT_NAME_LENGTH] = '\0';
    RtlInitUnicodeString(&endpoint->deviceName, request->name);
    RtlInitUnicodeString(&endpoint->deviceId, request->id);
    status = SarStringDuplicate(
        &endpoint->deviceName, &endpoint->deviceName);

    if (!NT_SUCCESS(status)) {
        goto err_out;
    }

    deviceNameAllocated = TRUE;
    status = SarStringDuplicate(
        &endpoint->deviceId, &endpoint->deviceId);

    if (!NT_SUCCESS(status)) {
        goto err_out;
    }

    // Windows 10 introduces a 'format cache' which at this time appears to
    // not fully invalidate itself when KSEVENT_PINCAPS_FORMATCHANGE is
    // sent. Work around this by encoding information about the sample rate,
    // channel count and sample resolution into the endpoint ID. This means
    // that WASAPI endpoints won't be stable across changes to these
    // parameters on Windows 10 and e.g. applications that store them as
    // configuration may lose track of them, but that seems better than "it
    // fully stops working until you reinstall the driver".
    RtlGetVersion(&versionInfo);

    if (versionInfo.dwMajorVersion >= 10) {
        DECLARE_UNICODE_STRING_SIZE(deviceIdBuffer, 256);

        RtlUnicodeStringPrintf(&deviceIdBuffer,
            L"%ws_%d_%d_%d", request->id, request->channelCount,
            controlContext->sampleRate, controlContext->sampleSize);
        status = SarStringDuplicate(
            &endpoint->deviceIdMangled, &deviceIdBuffer);

        if (!NT_SUCCESS(status)) {
            goto err_out;
        }
    } else {
        status = SarStringDuplicate(
            &endpoint->deviceIdMangled, &endpoint->deviceId);

        if (!NT_SUCCESS(status)) {
            goto err_out;
        }
    }

    KsAcquireDevice(ksDevice);
    status = KsCreateFilterFactory(
        device, endpoint->filterDesc, endpoint->deviceIdMangled.Buffer,
        nullptr, KSCREATE_ITEM_FREEONSTOP,
        nullptr, nullptr, &endpoint->filterFactory);
    KsReleaseDevice(ksDevice);

    if (!NT_SUCCESS(status)) {
        goto err_out;
    }

    ExAcquireFastMutex(&controlContext->mutex);

    BOOLEAN runWorkItem = IsListEmpty(&controlContext->pendingEndpointList);

    InsertTailList(&controlContext->pendingEndpointList, &endpoint->listEntry);

    if (runWorkItem) {
        SarRetainControlContext(controlContext);
        IoQueueWorkItem(
            controlContext->workItem,
            SarProcessPendingEndpoints, DelayedWorkQueue, controlContext);
    }

    ExReleaseFastMutex(&controlContext->mutex);
    return STATUS_PENDING;

err_out:
    if (!deviceNameAllocated) {
        endpoint->deviceName = {};
    }

    if (!deviceIdAllocated) {
        endpoint->deviceId = {};
    }

    SarDeleteEndpoint(endpoint);
    return status;
}

VOID SarDeleteEndpoint(SarEndpoint *endpoint)
{
    SAR_LOG("SarDeleteEndpoint");
    PKSDEVICE ksDevice = KsFilterFactoryGetDevice(endpoint->filterFactory);

    if (endpoint->filterFactory) {
        KsAcquireDevice(ksDevice);
        KsDeleteFilterFactory(endpoint->filterFactory);
        KsReleaseDevice(ksDevice);
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

    if (endpoint->deviceName.Buffer) {
        SarStringFree(&endpoint->deviceName);
    }

    if (endpoint->deviceId.Buffer) {
        SarStringFree(&endpoint->deviceId);
    }

    if (endpoint->deviceIdMangled.Buffer) {
        SarStringFree(&endpoint->deviceIdMangled);
    }

    ExFreePoolWithTag(endpoint, SAR_TAG);
}

VOID SarOrphanEndpoint(SarEndpoint *endpoint)
{
    SAR_LOG("SarOrphanEndpoint");
    ExAcquireFastMutex(&endpoint->mutex);
    endpoint->orphan = TRUE;
    ExReleaseFastMutex(&endpoint->mutex);
    KsFilterFactorySetDeviceClassesState(endpoint->filterFactory, FALSE);
    SarReleaseEndpoint(endpoint);
}

NTSTATUS SarSendFormatChangeEvent(SarDriverExtension *extension)
{
    PVOID restartKey = nullptr;

    KeEnterCriticalRegion();
    ExAcquireFastMutexUnsafe(&extension->mutex);

    FOR_EACH_GENERIC(
        &extension->controlContextTable, SarTableEntry,
        tableEntry, restartKey) {

        SarControlContext *controlContext =
            (SarControlContext *)tableEntry->value;

        ExAcquireFastMutexUnsafe(&controlContext->mutex);

        PLIST_ENTRY entry = controlContext->endpointList.Flink;

        while (entry != &controlContext->endpointList) {
            SarEndpoint *endpoint =
                CONTAINING_RECORD(entry, SarEndpoint, listEntry);

            entry = entry->Flink;
            KsAcquireDevice(extension->ksDevice);

            for (PKSFILTER filter =
                    KsFilterFactoryGetFirstChildFilter(endpoint->filterFactory);
                 filter;
                 filter = KsFilterGetNextSiblingFilter(filter)) {

                KsFilterGenerateEvents(filter,
                    &KSEVENTSETID_PinCapsChange,
                    KSEVENT_PINCAPS_FORMATCHANGE,
                    0, nullptr,
                    nullptr, nullptr);
            }

            KsReleaseDevice(extension->ksDevice);
        }

        ExReleaseFastMutexUnsafe(&controlContext->mutex);
    }

    ExReleaseFastMutexUnsafe(&extension->mutex);
    KeLeaveCriticalRegion();
    return STATUS_SUCCESS;
}
