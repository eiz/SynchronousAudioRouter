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

IO_WORKITEM_ROUTINE SarProcessPendingEndpoints;

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
    PULONG bufferMap = nullptr;
    DWORD bufferSize = 0;

    if (request->bufferSize == 0 ||
        request->bufferSize > SAR_MAX_BUFFER_SIZE ||
        request->sampleSize < SAR_MIN_SAMPLE_SIZE ||
        request->sampleSize > SAR_MAX_SAMPLE_SIZE ||
        request->sampleRate < SAR_MIN_SAMPLE_RATE ||
        request->sampleRate > SAR_MAX_SAMPLE_RATE ||
        request->periodSizeBytes > request->bufferSize ||
        request->periodSizeBytes == 0) {
        return STATUS_INVALID_PARAMETER;
    }

    if (!NT_SUCCESS(status = ExUuidCreate(&sectionGuid))) {
        return status;
    }

    bufferSize = ROUND_TO_PAGES(request->bufferSize);

    RtlUnicodeStringPrintf(
        &sectionName,
        L"\\BaseNamedObjects\\SynchronousAudioRouter_" GUID_FORMAT,
        GUID_VALUES(sectionGuid));
    InitializeObjectAttributes(
        &sectionAttributes, &sectionName, OBJ_KERNEL_HANDLE, nullptr, nullptr);
    sectionSize.QuadPart = bufferSize + SAR_BUFFER_CELL_SIZE;

    DWORD bufferMapSize = SarBufferMapSize(bufferSize);

    bufferMap = (PULONG)ExAllocatePoolWithTag(
        NonPagedPool, bufferMapSize, SAR_TAG);

    if (!bufferMap) {
        status = STATUS_INSUFFICIENT_RESOURCES;
        goto err_out;
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
    
    ExAcquireFastMutex(&controlContext->mutex);

    if (controlContext->bufferSize) {
        ExReleaseFastMutex(&controlContext->mutex);
        status = STATUS_INVALID_STATE_TRANSITION;
        goto err_out;
    }

    controlContext->bufferSize = bufferSize;
    controlContext->periodSizeBytes = request->periodSizeBytes;
    controlContext->sampleSize = request->sampleSize;
    controlContext->sampleRate = request->sampleRate;
    controlContext->minimumFrameCount = request->minimumFrameCount;
    
    RtlInitializeBitMap(&controlContext->bufferMap,
        bufferMap, SarBufferMapEntryCount(bufferSize));

    controlContext->bufferMapStorage = bufferMap;
    bufferMap = nullptr;

    controlContext->sectionViewBaseAddress = baseAddress;
    baseAddress = nullptr;

    controlContext->bufferSection = section;
    ExReleaseFastMutex(&controlContext->mutex);

    response->actualSize = sectionSize.LowPart;
    response->virtualAddress = controlContext->sectionViewBaseAddress;
    response->registerBase = sectionSize.LowPart - SAR_BUFFER_CELL_SIZE;

    return STATUS_SUCCESS;

err_out:
    if (baseAddress) {
        ZwUnmapViewOfSection(ZwCurrentProcess(), baseAddress);
    }

    if (section) {
        ZwClose(section);
    }

    if (bufferMap) {
        ExFreePoolWithTag(bufferMap, SAR_TAG);
    }

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
    UNICODE_STRING associationValue, deviceDesc, guidEmpty;
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
        &deviceDesc, L"{a45c254e-df1c-4efd-8020-67d146a850e0},2");
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

    // Required to make windows 10 uses our endpoint name instead of a generic name based on the category
    status = ZwSetValueKey(
        zeroSubKey, &deviceDesc, 0, REG_SZ, endpoint->deviceName.Buffer,
        endpoint->deviceName.Length + sizeof(UNICODE_NULL));

    if (!NT_SUCCESS(status)) {
        SAR_LOG("Couldn't set device description for endpoint");
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

        // Call KsFilterFactoryGetSymbolicLink require IRQL at PASSIVE_LEVEL (so after mutex release)
        PUNICODE_STRING symlink;
        PUNICODE_STRING topologySymlink;
        PLIST_ENTRY current = entry;
        PIRP pendingIrp = endpoint->pendingIrp;

        entry = endpoint->listEntry.Flink;
        RemoveEntryList(current);
        ExReleaseFastMutex(&controlContext->mutex);

        symlink =
            KsFilterFactoryGetSymbolicLink(endpoint->filterFactory);
        topologySymlink =
            KsFilterFactoryGetSymbolicLink(endpoint->topologyFilterFactory);
        
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

        status = SarSetDeviceInterfaceProperties(
            endpoint, topologySymlink, &KSCATEGORY_AUDIO);

        if (!NT_SUCCESS(status)) {
            goto out;
        }

        status = SarSetDeviceInterfaceProperties(
            endpoint, topologySymlink, &KSCATEGORY_TOPOLOGY);

        if (!NT_SUCCESS(status)) {
            goto out;
        }

        RtlCopyUnicodeString(&endpoint->filterDescriptor.physicalConnectionSymlink,
            symlink);
        RtlCopyUnicodeString(&endpoint->topologyDescriptor.physicalConnectionSymlink,
            topologySymlink);

        status = KsFilterFactorySetDeviceClassesState(
            endpoint->filterFactory, TRUE);
        status = KsFilterFactorySetDeviceClassesState(
            endpoint->topologyFilterFactory, TRUE);

        if (!NT_SUCCESS(status)) {
            SAR_LOG("Couldn't enable KS filter factory");
            goto out;
        }

out:
        ExAcquireFastMutex(&controlContext->mutex);

        if (NT_SUCCESS(status)) {
            InsertTailList(&controlContext->endpointList, &endpoint->listEntry);
        } else {
            SarReleaseEndpoint(endpoint);
        }

        pendingIrp->IoStatus.Status = status;
        IoCompleteRequest(pendingIrp, IO_NO_INCREMENT);
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

    // Endpoints assume buffer parameters are not 0 (else division by 0 could occur)
    if (!controlContext->bufferSize) {
        SAR_LOG("(SAR) Creating endpoints require setting buffer beforehand");
        return STATUS_INVALID_STATE_TRANSITION;
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
    endpoint->channelMask = KSAUDIO_SPEAKER_DIRECTOUT;
    endpoint->type = request->type;
    endpoint->index = request->index;
    endpoint->owner = controlContext;

    endpoint->filterDescriptor.initWaveFilter(controlContext, request);
    endpoint->topologyDescriptor.initTopologyFilter(request);

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

    deviceIdAllocated = TRUE;

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
            L"%ws_%u_%u_%u", request->id, request->channelCount,
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
    {
        DECLARE_UNICODE_STRING_SIZE(topologyFilterRefIdBuffer, 256);

        RtlUnicodeStringPrintf(&topologyFilterRefIdBuffer,
            L"%wZ_topology", &endpoint->deviceIdMangled);
        status = SarStringDuplicate(
            &endpoint->topologyFilterRefId, &topologyFilterRefIdBuffer);
    }

    if (!NT_SUCCESS(status)) {
        goto err_out;
    }

    KsAcquireDevice(ksDevice);
    status = KsCreateFilterFactory(
        device, &endpoint->filterDescriptor.filterDesc, endpoint->deviceIdMangled.Buffer,
        nullptr, KSCREATE_ITEM_FREEONSTOP,
        nullptr, nullptr, &endpoint->filterFactory);
    status = KsCreateFilterFactory(
        device, &endpoint->topologyDescriptor.filterDesc, endpoint->topologyFilterRefId.Buffer,
        nullptr, KSCREATE_ITEM_FREEONSTOP,
        nullptr, nullptr, &endpoint->topologyFilterFactory);

    KsReleaseDevice(ksDevice);

    if (!NT_SUCCESS(status)) {
        goto err_out;
    }

    // Only call IoMarkIrpPending when there is no error and we WILL return STATUS_PENDING
    // but before any chance for the IRP to be completed (in SarProcessPendingEndpoints)
    // So before queuing the endpoint to the pendingEndpointList list
    IoMarkIrpPending(irp);

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

    if (endpoint->topologyFilterFactory) {
        PKSDEVICE ksDevice = KsFilterFactoryGetDevice(endpoint->topologyFilterFactory);

        KsAcquireDevice(ksDevice);
        KsDeleteFilterFactory(endpoint->topologyFilterFactory);
        KsReleaseDevice(ksDevice);
    }

    if (endpoint->filterFactory) {
        PKSDEVICE ksDevice = KsFilterFactoryGetDevice(endpoint->filterFactory);

        KsAcquireDevice(ksDevice);
        KsDeleteFilterFactory(endpoint->filterFactory);
        KsReleaseDevice(ksDevice);
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

    if (endpoint->topologyFilterRefId.Buffer) {
        SarStringFree(&endpoint->topologyFilterRefId);
    }

    ExFreePoolWithTag(endpoint, SAR_TAG);
}

VOID SarOrphanEndpoint(SarEndpoint *endpoint)
{
    ExAcquireFastMutex(&endpoint->mutex);
    endpoint->orphan = TRUE;
    ExReleaseFastMutex(&endpoint->mutex);
    KsFilterFactorySetDeviceClassesState(endpoint->topologyFilterFactory, FALSE);
    KsFilterFactorySetDeviceClassesState(endpoint->filterFactory, FALSE);
    SarReleaseEndpoint(endpoint);
}

NTSTATUS SarSendFormatChangeEvent(PDEVICE_OBJECT deviceObject, SarDriverExtension *extension)
{
    PVOID restartKey = nullptr;
    PKSDEVICE ksDevice = KsGetDeviceForDeviceObject(deviceObject);

    // Always acquire ksDevice before extension->mutex, else deadlocks can occurs with a concurrent SarKsFilterCreate for example
    KsAcquireDevice(ksDevice);

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
        }

        ExReleaseFastMutexUnsafe(&controlContext->mutex);
    }

    ExReleaseFastMutexUnsafe(&extension->mutex);
    KeLeaveCriticalRegion();

    KsReleaseDevice(ksDevice);

    return STATUS_SUCCESS;
}
