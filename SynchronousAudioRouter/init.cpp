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

extern "C" DRIVER_INITIALIZE DriverEntry;

DRIVER_DISPATCH SarIrpCreate;
DRIVER_DISPATCH SarIrpDeviceControl;
DRIVER_DISPATCH SarIrpClose;
DRIVER_DISPATCH SarIrpCleanup;
DRIVER_UNLOAD SarUnload;

#define SAR_TAG '1RAS'
//#define NO_LOGGING

#ifdef NO_LOGGING
#define SAR_LOG(...)
#else
#define SAR_LOG(...) \
    DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_WARNING_LEVEL, __VA_ARGS__)
#endif

#define DEVICE_NT_NAME L"\\Device\\SynchronousAudioRouter"
#define DEVICE_WIN32_NAME L"\\DosDevices\\SynchronousAudioRouter"
#define DEVICE_REFERENCE_STRING L"\\{0EB287D4-6C04-4926-AE19-3C066A4C3F3A}"

static NTSTATUS SarKsDeviceAdd(IN PKSDEVICE device);
static NTSTATUS SarKsDevicePostStart(IN PKSDEVICE device);

static KSDEVICE_DISPATCH gDeviceDispatch = {
    SarKsDeviceAdd, // Add
    nullptr, // Start
    SarKsDevicePostStart, // PostStart
    nullptr, // QueryStop
    nullptr, // CancelStop
    nullptr, // Stop
    nullptr, // QueryRemove
    nullptr, // CancelRemove
    nullptr, // Remove
    nullptr, // QueryCapabilities
    nullptr, // SurpriseRemoval
    nullptr, // QueryPower
    nullptr, // SetPower
    nullptr  // QueryInterface
};

static const KSDEVICE_DESCRIPTOR gDeviceDescriptor = {
    &gDeviceDispatch,
    0, nullptr,
    KSDEVICE_DESCRIPTOR_VERSION
};

static const KSPIN_DESCRIPTOR_EX gPinDescriptorTemplate = {
    nullptr, // Dispatch
    nullptr, // AutomationTable
    {}, // PinDescriptor
    KSPIN_FLAG_DO_NOT_INITIATE_PROCESSING |
    KSPIN_FLAG_FRAMES_NOT_REQUIRED_FOR_PROCESSING |
    KSPIN_FLAG_PROCESS_IF_ANY_IN_RUN_STATE |
    KSPIN_FLAG_FIXED_FORMAT,
    1, // InstancesPossible
    1, // InstancesNecessary
    nullptr, // AllocatorFraming
    nullptr, // TODO: intersection handler
};

static const GUID gCategoriesTableCapture[] = {
    STATICGUIDOF(KSCATEGORY_AUDIO),
    STATICGUIDOF(KSCATEGORY_CAPTURE),
};

static const GUID gCategoriesTableRender[] = {
    STATICGUIDOF(KSCATEGORY_AUDIO),
    STATICGUIDOF(KSCATEGORY_RENDER),
};

static KSFILTER_DESCRIPTOR gFilterDescriptorTemplate = {
    nullptr, // Dispatch
    nullptr, // AutomationTable
    KSFILTER_DESCRIPTOR_VERSION, // Version
    0, // Flags
    nullptr, // ReferenceGuid
    0, // PinDescriptorsCount
    0, // PinDescriptorSize
    nullptr,
    DEFINE_KSFILTER_CATEGORIES_NULL,
    DEFINE_KSFILTER_NODE_DESCRIPTORS_NULL,
    DEFINE_KSFILTER_DEFAULT_CONNECTIONS,
    nullptr, // ComponentId
};

NTSTATUS SarKsDeviceAdd(IN PKSDEVICE device)
{
    NTSTATUS status;
    UNICODE_STRING referenceString;
    SarDriverExtension *extension =
        (SarDriverExtension *)IoGetDriverObjectExtension(
            device->FunctionalDeviceObject->DriverObject, DriverEntry);

    RtlUnicodeStringInit(&referenceString, DEVICE_REFERENCE_STRING + 1);
    status = IoRegisterDeviceInterface(device->PhysicalDeviceObject,
        &GUID_DEVINTERFACE_SYNCHRONOUSAUDIOROUTER, &referenceString,
        &extension->sarInterfaceName);

    if (!NT_SUCCESS(status)) {
        SAR_LOG("Failed to create device interface.");
        return status;
    }

    SAR_LOG("KSDevice was created for %p, dev interface: %wZ",
        device, &extension->sarInterfaceName);
    return status;
}

NTSTATUS SarKsDevicePostStart(IN PKSDEVICE device)
{
    NTSTATUS status;
    SarDriverExtension *extension =
        (SarDriverExtension *)IoGetDriverObjectExtension(
            device->FunctionalDeviceObject->DriverObject, DriverEntry);

    status = IoSetDeviceInterfaceState(&extension->sarInterfaceName, TRUE);

    if (!NT_SUCCESS(status)) {
        SAR_LOG("Failed to enable device interface.");
    }

    return status;
}

static BOOL checkIoctlInput(
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

static bool ioctlInput(
    NTSTATUS *status, PIRP irp, PIO_STACK_LOCATION irpStack, PVOID *buffer,
    ULONG size)
{
    UNREFERENCED_PARAMETER(buffer);

    if (!status) {
        return FALSE;
    }

    if (!checkIoctlInput(status, irpStack, size)) {
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

VOID SarProcessPendingEndpoints(PDEVICE_OBJECT deviceObject, PVOID context)
{
    UNREFERENCED_PARAMETER(deviceObject);
    NTSTATUS status = STATUS_UNSUCCESSFUL;
    SarFileContext *fileContext = (SarFileContext *)context;

    SAR_LOG("Processing pending endpoints");

    ExAcquireFastMutex(&fileContext->mutex);

    PLIST_ENTRY entry = fileContext->pendingEndpointList.Flink;

    while (entry != &fileContext->pendingEndpointList) {
        SarEndpoint *endpoint = CONTAINING_RECORD(entry, SarEndpoint, listEntry);
        PUNICODE_STRING symlink =
            KsFilterFactoryGetSymbolicLink(endpoint->filterFactory);

        SAR_LOG("Processing a pending endpoint");
        entry = endpoint->listEntry.Flink;
        status = IoSetDeviceInterfacePropertyData(symlink,
            &DEVPKEY_DeviceInterface_FriendlyName, LOCALE_NEUTRAL, 0,
            DEVPROP_TYPE_STRING,
            endpoint->pendingDeviceName.Length + sizeof(WCHAR),
            endpoint->pendingDeviceName.Buffer);

        if (!NT_SUCCESS(status)) {
            SAR_LOG("Couldn't set friendly name: %08X", status);
            goto err_out;
        }

        status = IoSetDeviceInterfacePropertyData(symlink,
            &DEVPKEY_DeviceInterface_ClassGuid, LOCALE_NEUTRAL, 0,
            DEVPROP_TYPE_GUID, sizeof(GUID), (PVOID)&CLSID_Proxy);

        if (!NT_SUCCESS(status)) {
            SAR_LOG("Couldn't set clsid: %08X", status);
            goto err_out;
        }

        status = KsFilterFactorySetDeviceClassesState(
            endpoint->filterFactory, TRUE);

        if (!NT_SUCCESS(status)) {
            SAR_LOG("Couldn't enable KS filter factory");
            goto err_out;
        }

        RemoveEntryList(entry);
        InsertTailList(&fileContext->endpointList, &endpoint->listEntry);

err_out:
        endpoint->pendingIrp->IoStatus.Status = status;
        IoCompleteRequest(endpoint->pendingIrp, IO_NO_INCREMENT);
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

    if (request->type != SAR_ENDPOINT_TYPE_CAPTURE &&
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
    endpoint->filterDesc = (PKSFILTER_DESCRIPTOR)
        ExAllocatePoolWithTag(PagedPool, sizeof(KSFILTER_DESCRIPTOR), SAR_TAG);

    if (!endpoint->filterDesc) {
        goto err_out;
    }

    endpoint->pinDesc = (PKSPIN_DESCRIPTOR_EX)
        ExAllocatePoolWithTag(PagedPool, sizeof(KSPIN_DESCRIPTOR_EX), SAR_TAG);

    if (!endpoint->pinDesc) {
        goto err_out;
    }

    endpoint->dataRange = (PKSDATARANGE_AUDIO)
        ExAllocatePoolWithTag(PagedPool, sizeof(KSDATARANGE_AUDIO), SAR_TAG);

    if (!endpoint->dataRange) {
        goto err_out;
    }

    endpoint->allocatorFraming = (PKSALLOCATOR_FRAMING_EX)
        ExAllocatePoolWithTag(PagedPool, sizeof(KSALLOCATOR_FRAMING_EX), SAR_TAG);

    if (!endpoint->allocatorFraming) {
        goto err_out;
    }

    *endpoint->filterDesc = gFilterDescriptorTemplate;
    *endpoint->pinDesc = gPinDescriptorTemplate;
    endpoint->filterDesc->CategoriesCount = 2;
    endpoint->filterDesc->Categories =
        request->type == SAR_ENDPOINT_TYPE_CAPTURE ?
        gCategoriesTableCapture : gCategoriesTableRender;
    endpoint->filterDesc->PinDescriptors = endpoint->pinDesc;
    endpoint->filterDesc->PinDescriptorsCount = 1;
    endpoint->filterDesc->PinDescriptorSize = sizeof(KSPIN_DESCRIPTOR_EX);

    PKSPIN_DESCRIPTOR pinDesc = &endpoint->pinDesc->PinDescriptor;

    pinDesc->DataRangesCount = 1;
    pinDesc->DataRanges = (PKSDATARANGE *)&endpoint->dataRange;
    pinDesc->Communication =
        request->type == SAR_ENDPOINT_TYPE_CAPTURE ?
        KSPIN_COMMUNICATION_SOURCE : KSPIN_COMMUNICATION_SINK;
    pinDesc->DataFlow =
        request->type == SAR_ENDPOINT_TYPE_CAPTURE ?
        KSPIN_DATAFLOW_OUT : KSPIN_DATAFLOW_IN;

    endpoint->dataRange->DataRange.FormatSize = sizeof(KSDATARANGE_AUDIO);
    endpoint->dataRange->DataRange.Flags = 0;
    endpoint->dataRange->DataRange.SampleSize = 0;
    endpoint->dataRange->DataRange.Reserved = 0;
    endpoint->dataRange->DataRange.MajorFormat = KSDATAFORMAT_TYPE_AUDIO;
    endpoint->dataRange->DataRange.SubFormat = KSDATAFORMAT_SUBTYPE_PCM;
    endpoint->dataRange->DataRange.Specifier =
        KSDATAFORMAT_SPECIFIER_WAVEFORMATEX;
    endpoint->dataRange->MaximumBitsPerSample = fileContext->sampleDepth * 3;
    endpoint->dataRange->MinimumBitsPerSample = fileContext->sampleDepth * 3;
    endpoint->dataRange->MaximumSampleFrequency = fileContext->sampleRate;
    endpoint->dataRange->MinimumSampleFrequency = fileContext->sampleRate;
    endpoint->dataRange->MaximumChannels = request->channelCount;

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

    if (endpoint->pinDesc) {
        ExFreePoolWithTag(endpoint->pinDesc, SAR_TAG);
    }

    if (endpoint->filterDesc) {
        ExFreePoolWithTag(endpoint->filterDesc, SAR_TAG);
    }

    ExFreePoolWithTag(endpoint, SAR_TAG);
    return status;
}

NTSTATUS SarIrpCreate(PDEVICE_OBJECT deviceObject, PIRP irp)
{
    NTSTATUS status = STATUS_SUCCESS;
    UNICODE_STRING referencePath;
    PIO_STACK_LOCATION irpStack = IoGetCurrentIrpStackLocation(irp);
    SarDriverExtension *extension =
        (SarDriverExtension *)IoGetDriverObjectExtension(
            deviceObject->DriverObject, DriverEntry);
    SarFileContext fileContextTemplate;
    SarFileContext *fileContext;
    BOOLEAN isNew;

    SAR_LOG("Intercepted KS IRP_MJ_CREATE handler: %wZ",
        &irpStack->FileObject->FileName);
    RtlUnicodeStringInit(&referencePath, DEVICE_REFERENCE_STRING);

    if (RtlCompareUnicodeString(
        &irpStack->FileObject->FileName, &referencePath, TRUE) != 0) {
        SAR_LOG("Passthrough IRP_MJ_CREATE");
        return extension->ksDispatchCreate(deviceObject, irp);
    }

    fileContextTemplate.fileObject = irpStack->FileObject;
    ExAcquireFastMutex(&extension->fileContextLock);
    fileContext = (SarFileContext *)RtlInsertElementGenericTable(
        &extension->fileContextTable, (PVOID)&fileContextTemplate,
        sizeof(SarFileContext), &isNew);
    ExReleaseFastMutex(&extension->fileContextLock);
    ASSERT(isNew);

    if (!fileContext) {
        status = STATUS_INSUFFICIENT_RESOURCES;
        goto out;
    }

    if (!NT_SUCCESS(SarInitializeFileContext(fileContext))) {
        SarDeleteFileContext(extension, irp);
        status = STATUS_INSUFFICIENT_RESOURCES;
        goto out;
    }

    irpStack->FileObject->FsContext2 = fileContext;

out:
    irp->IoStatus.Status = status;
    IoCompleteRequest(irp, IO_NO_INCREMENT);
    return status;
}

NTSTATUS SarIrpClose(PDEVICE_OBJECT deviceObject, PIRP irp)
{
    NTSTATUS status;
    SarDriverExtension *extension =
        (SarDriverExtension *)IoGetDriverObjectExtension(
            deviceObject->DriverObject, DriverEntry);
    BOOLEAN deleted = SarDeleteFileContext(extension, irp);

    if (!deleted) {
        SAR_LOG("SarIrpClose Passthrough");
        return extension->ksDispatchClose(deviceObject, irp);
    }

    SAR_LOG("SarIrpClose");
    status = STATUS_SUCCESS;
    irp->IoStatus.Status = status;
    IoCompleteRequest(irp, IO_NO_INCREMENT);
    return status;
}

BOOLEAN SarDeleteFileContext(SarDriverExtension *extension, PIRP irp)
{
    PIO_STACK_LOCATION irpStack;
    SarFileContext *fileContext;
    SarFileContext fileContextTemplate;
    BOOLEAN deleted;

    irpStack = IoGetCurrentIrpStackLocation(irp);
    fileContextTemplate.fileObject = irpStack->FileObject;
    ExAcquireFastMutex(&extension->fileContextLock);
    fileContext = (SarFileContext *)RtlLookupElementGenericTable(
        &extension->fileContextTable, (PVOID)&fileContextTemplate);

    if (fileContext) {
        ExAcquireFastMutex(&fileContext->mutex);

        PLIST_ENTRY entry = fileContext->endpointList.Flink;

        while (entry != &fileContext->endpointList) {
            //SarEndpoint *endpoint =
            //    CONTAINING_RECORD(entry, SarEndpoint, listEntry);

            entry = entry->Flink;
            SAR_LOG("This should delete filters but the locking is tricky");
        }

        if (fileContext->workItem) {
            IoFreeWorkItem(fileContext->workItem);
        }

        ExReleaseFastMutex(&fileContext->mutex);
    }

    deleted = RtlDeleteElementGenericTable(
        &extension->fileContextTable, &fileContextTemplate);
    ExReleaseFastMutex(&extension->fileContextLock);
    return deleted;
}

NTSTATUS SarIrpCleanup(PDEVICE_OBJECT deviceObject, PIRP irp)
{
    UNREFERENCED_PARAMETER(deviceObject);
    NTSTATUS status;
    SarDriverExtension *extension =
        (SarDriverExtension *)IoGetDriverObjectExtension(
            deviceObject->DriverObject, DriverEntry);
    BOOLEAN deleted = SarDeleteFileContext(extension, irp);

    if (!deleted) {
        SAR_LOG("SarIrpCleanup Passthrough");
        return extension->ksDispatchCleanup(deviceObject, irp);
    }

    SAR_LOG("SarIrpCleanup");
    status = STATUS_SUCCESS;
    irp->IoStatus.Status = status;
    IoCompleteRequest(irp, IO_NO_INCREMENT);
    return status;
}

NTSTATUS SarIrpDeviceControl(PDEVICE_OBJECT deviceObject, PIRP irp)
{
    PIO_STACK_LOCATION irpStack;
    ULONG ioControlCode;
    NTSTATUS ntStatus = STATUS_SUCCESS;
    SarDriverExtension *extension =
        (SarDriverExtension *)IoGetDriverObjectExtension(
            deviceObject->DriverObject, DriverEntry);
    SarFileContext *fileContext;
    SarFileContext fileContextTemplate;

    irpStack = IoGetCurrentIrpStackLocation(irp);
    ioControlCode = irpStack->Parameters.DeviceIoControl.IoControlCode;

    SAR_LOG("(SAR) Before: DeviceIoControl IRP: %d", ioControlCode);

    fileContextTemplate.fileObject = irpStack->FileObject;
    ExAcquireFastMutex(&extension->fileContextLock);
    fileContext = (SarFileContext *)RtlLookupElementGenericTable(
        &extension->fileContextTable, (PVOID)&fileContextTemplate);
    ExReleaseFastMutex(&extension->fileContextLock);

    if (!fileContext) {
        SAR_LOG("Passthrough");
        return extension->ksDispatchDeviceControl(deviceObject, irp);
    }

    SAR_LOG("(SAR) DeviceIoControl IRP: %d context %p",
        ioControlCode, fileContext);

    switch (ioControlCode) {
        case SAR_REQUEST_SET_BUFFER_LAYOUT: {
            SAR_LOG("(SAR) create audio buffers");

            SarSetBufferLayoutRequest request;

            if (!ioctlInput(
                &ntStatus, irp, irpStack, (PVOID *)&request, sizeof(request))) {

                break;
            }

            ntStatus = SarSetBufferLayout(fileContext, &request);
            break;
        }
        case SAR_REQUEST_CREATE_ENDPOINT: {
            SAR_LOG("(SAR) Create endpoint");

            SarCreateEndpointRequest request;

            if (!ioctlInput(
                &ntStatus, irp, irpStack, (PVOID *)&request, sizeof(request))) {

                break;
            }

            SAR_LOG("(SAR) Create endpoint request: %d, %d, %d",
                request.type, request.index, request.channelCount);

            IoMarkIrpPending(irp);
            ntStatus = SarCreateEndpoint(
                deviceObject, irp, extension, fileContext, &request);
            break;
        }
        case SAR_REQUEST_MAP_AUDIO_BUFFER:
        case SAR_REQUEST_AUDIO_TICK:
        default:
            SAR_LOG("(SAR) Unknown ioctl %d");
            break;
    }

    if (ntStatus != STATUS_PENDING) {
        irp->IoStatus.Status = ntStatus;
        IoCompleteRequest(irp, IO_NO_INCREMENT);
    }

    return ntStatus;
}

VOID SarUnload(PDRIVER_OBJECT driverObject)
{
    SAR_LOG("SAR is unloading");

    SarDriverExtension *extension =
        (SarDriverExtension *)IoGetDriverObjectExtension(
            driverObject, DriverEntry);

    RtlFreeUnicodeString(&extension->sarInterfaceName);
}

NTSTATUS SarInitializeFileContext(SarFileContext *fileContext)
{
    ExInitializeFastMutex(&fileContext->mutex);
    InitializeListHead(&fileContext->endpointList);
    InitializeListHead(&fileContext->pendingEndpointList);
    fileContext->sampleRate = 0;
    fileContext->sampleDepth = 0;
    fileContext->bufferCount = 0;
    fileContext->bufferSize = 0;

    fileContext->workItem = IoAllocateWorkItem(
        fileContext->fileObject->DeviceObject);

    if (!fileContext->workItem) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    return STATUS_SUCCESS;
}

RTL_GENERIC_COMPARE_RESULTS NTAPI SarCompareFileContext(
    PRTL_GENERIC_TABLE table, PVOID lhs, PVOID rhs)
{
    UNREFERENCED_PARAMETER(table);
    SarFileContext *slhs = (SarFileContext *)lhs;
    SarFileContext *srhs = (SarFileContext *)rhs;

    return slhs->fileObject < srhs->fileObject ? GenericLessThan :
        slhs->fileObject == srhs->fileObject ? GenericEqual :
        GenericGreaterThan;
}

PVOID NTAPI SarAllocateFileContext(PRTL_GENERIC_TABLE table, CLONG byteSize)
{
    UNREFERENCED_PARAMETER(table);
    return ExAllocatePoolWithTag(PagedPool, byteSize, SAR_TAG);
}

VOID NTAPI SarFreeFileContext(PRTL_GENERIC_TABLE table, PVOID buffer)
{
    UNREFERENCED_PARAMETER(table);
    ExFreePoolWithTag(buffer, SAR_TAG);
}

extern "C" NTSTATUS DriverEntry(
    IN PDRIVER_OBJECT driverObject,
    IN PUNICODE_STRING registryPath)
{
    SarDriverExtension *extension;
    NTSTATUS status;

    SAR_LOG("SAR is loading.");

    status = IoAllocateDriverObjectExtension(
        driverObject, DriverEntry, sizeof(SarDriverExtension),
        (PVOID *)&extension);

    if (!NT_SUCCESS(status)) {
        return status;
    }

    status = KsInitializeDriver(driverObject, registryPath, &gDeviceDescriptor);

    if (!NT_SUCCESS(status)) {
        return status;
    }

    ExInitializeFastMutex(&extension->fileContextLock);
    RtlInitializeGenericTable(&extension->fileContextTable,
        SarCompareFileContext,
        SarAllocateFileContext,
        SarFreeFileContext,
        nullptr);
    extension->ksDispatchCreate = driverObject->MajorFunction[IRP_MJ_CREATE];
    extension->ksDispatchClose = driverObject->MajorFunction[IRP_MJ_CLOSE];
    extension->ksDispatchCleanup = driverObject->MajorFunction[IRP_MJ_CLEANUP];
    extension->ksDispatchDeviceControl =
        driverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL];
    extension->nextFilterId = 0;
    extension->sarInterfaceName = {};
    driverObject->MajorFunction[IRP_MJ_CREATE] = SarIrpCreate;
    driverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] = SarIrpDeviceControl;
    driverObject->MajorFunction[IRP_MJ_CLOSE] = SarIrpClose;
    driverObject->MajorFunction[IRP_MJ_CLEANUP] = SarIrpCleanup;
    return STATUS_SUCCESS;
}
