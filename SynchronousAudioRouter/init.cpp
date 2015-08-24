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
#define SAR_LOG(...) \
    DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_WARNING_LEVEL, __VA_ARGS__)

#define DEVICE_NT_NAME L"\\Device\\SynchronousAudioRouter"
#define DEVICE_WIN32_NAME L"\\DosDevices\\SynchronousAudioRouter"
#define DEVICE_REFERENCE_STRING L"\\{0EB287D4-6C04-4926-AE19-3C066A4C3F3A}"

static NTSTATUS SarKsDeviceAdd(IN PKSDEVICE device);

static KSDEVICE_DISPATCH gDeviceDispatch = {
    SarKsDeviceAdd, // Add
    nullptr, // Start
    nullptr, // PostStart
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
    nullptr, // TODO: allocator framing
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

static KSFILTER_DESCRIPTOR gFilterDescriptor = {
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

static NTSTATUS SarKsDeviceAdd(IN PKSDEVICE device)
{
    NTSTATUS status;
    UNICODE_STRING symLinkName;
    UNICODE_STRING referenceString;

    RtlUnicodeStringInit(&referenceString, DEVICE_REFERENCE_STRING + 1);

    status = IoRegisterDeviceInterface(device->PhysicalDeviceObject,
        &GUID_DEVINTERFACE_SYNCHRONOUSAUDIOROUTER, &referenceString, &symLinkName);

    if (!NT_SUCCESS(status)) {
        SAR_LOG("Failed to create device interface.");
        return status;
    }

    SAR_LOG("KSDevice was created for %p, dev interface: %wZ",
        device, &symLinkName);
    status = IoSetDeviceInterfaceState(&symLinkName, TRUE);

    if (!NT_SUCCESS(status)) {
        SAR_LOG("Failed to enable device interface.");
    }

    RtlFreeUnicodeString(&symLinkName);
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

// on driver entry: create ks device object
// on create endpoint: create ks filter & ks pin
//  - audio category
//    - for capture endpoints use KSCATEGORY_CAPTURE
//    - for playback endpoints use KSCATEGORY_RENDER
//  - filter has 1 pin
//  - pin has possibly multichannel data format (KSDATARANGE)
//  - pin supports a single KSDATARANGE
//  - pin flags: KSPIN_FLAG_DO_NOT_INITIATE_PROCESSING KSPIN_FLAG_PROCESS_IN_RUN_STATE_ONLY KSPIN_FLAG_FIXED_FORMAT KSPIN_FLAG_FRAMES_NOT_REQUIRED_FOR_PROCESSING

NTSTATUS SarCreateEndpoint(
    PDEVICE_OBJECT device,
    SarDriverExtension *extension,
    SarFileContext *fileContext,
    SarCreateEndpointRequest *request)
{
    UNREFERENCED_PARAMETER(fileContext);
    WCHAR buf[20] = {};
    UNICODE_STRING referenceString = { 0, sizeof(buf), buf };
    UNICODE_STRING deviceName;
    NTSTATUS status = STATUS_INSUFFICIENT_RESOURCES;
    PKSDEVICE ksDevice = KsGetDeviceForDeviceObject(device);
    SarEndpoint* filterInfo;

    if (request->type != SAR_ENDPOINT_TYPE_CAPTURE &&
        request->type != SAR_ENDPOINT_TYPE_PLAYBACK) {
        return STATUS_UNSUCCESSFUL;
    }

    if (request->channelCount > SAR_MAX_CHANNEL_COUNT) {
        return STATUS_UNSUCCESSFUL;
    }

    request->name[MAX_ENDPOINT_NAME_LENGTH] = '\0';
    RtlInitUnicodeString(&deviceName, request->name);

    LONG filterId = InterlockedIncrement(&extension->nextFilterId);

    filterInfo = (SarEndpoint *)
        ExAllocatePoolWithTag(PagedPool,
            SarEndpointSize(request->channelCount), SAR_TAG);

    if (!filterInfo) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    RtlZeroMemory(filterInfo, SarEndpointSize(request->channelCount));

    filterInfo->channelCount = request->channelCount;
    filterInfo->filterDesc = (PKSFILTER_DESCRIPTOR)
        ExAllocatePoolWithTag(PagedPool, sizeof(KSFILTER_DESCRIPTOR), SAR_TAG);

    if (!filterInfo->filterDesc) {
        goto err_out;
    }

    filterInfo->pinDesc = (PKSPIN_DESCRIPTOR_EX)
        ExAllocatePoolWithTag(PagedPool, sizeof(KSPIN_DESCRIPTOR_EX), SAR_TAG);

    if (!filterInfo->pinDesc) {
        goto err_out;
    }

    filterInfo->dataRange = (PKSDATARANGE_AUDIO)
        ExAllocatePoolWithTag(PagedPool, sizeof(KSDATARANGE_AUDIO), SAR_TAG);

    if (!filterInfo->dataRange) {
        goto err_out;
    }

    filterInfo->allocatorFraming = (PKSALLOCATOR_FRAMING_EX)
        ExAllocatePoolWithTag(PagedPool, sizeof(KSALLOCATOR_FRAMING_EX), SAR_TAG);

    if (!filterInfo->allocatorFraming) {
        goto err_out;
    }

    *filterInfo->filterDesc = gFilterDescriptor;
    *filterInfo->pinDesc = gPinDescriptorTemplate;
    filterInfo->filterDesc->CategoriesCount = 2;
    filterInfo->filterDesc->Categories =
        request->type == SAR_ENDPOINT_TYPE_CAPTURE ?
        gCategoriesTableCapture : gCategoriesTableRender;
    filterInfo->filterDesc->PinDescriptors = filterInfo->pinDesc;
    filterInfo->filterDesc->PinDescriptorsCount = 1;
    filterInfo->filterDesc->PinDescriptorSize = sizeof(KSPIN_DESCRIPTOR_EX);

    PKSPIN_DESCRIPTOR pinDesc = &filterInfo->pinDesc->PinDescriptor;

    pinDesc->DataRangesCount = 1;
    pinDesc->DataRanges = (PKSDATARANGE *)&filterInfo->dataRange;
    pinDesc->Communication =
        request->type == SAR_ENDPOINT_TYPE_CAPTURE ?
        KSPIN_COMMUNICATION_SOURCE : KSPIN_COMMUNICATION_SINK;
    pinDesc->DataFlow =
        request->type == SAR_ENDPOINT_TYPE_CAPTURE ?
        KSPIN_DATAFLOW_OUT : KSPIN_DATAFLOW_IN;

    filterInfo->dataRange->DataRange.FormatSize = sizeof(KSDATARANGE_AUDIO);
    filterInfo->dataRange->DataRange.Flags = 0;
    filterInfo->dataRange->DataRange.SampleSize = 0;
    filterInfo->dataRange->DataRange.Reserved = 0;
    filterInfo->dataRange->DataRange.MajorFormat = KSDATAFORMAT_TYPE_AUDIO;
    filterInfo->dataRange->DataRange.SubFormat = KSDATAFORMAT_SUBTYPE_PCM;
    filterInfo->dataRange->DataRange.Specifier =
        KSDATAFORMAT_SPECIFIER_WAVEFORMATEX;
    filterInfo->dataRange->MaximumBitsPerSample = fileContext->sampleDepth * 3;
    filterInfo->dataRange->MinimumBitsPerSample = fileContext->sampleDepth * 3;
    filterInfo->dataRange->MaximumSampleFrequency = fileContext->sampleRate;
    filterInfo->dataRange->MinimumSampleFrequency = fileContext->sampleRate;
    filterInfo->dataRange->MaximumChannels = request->channelCount;

    RtlIntegerToUnicodeString(filterId, 10, &referenceString);
    KsAcquireDevice(ksDevice);
    status = KsCreateFilterFactory(
        device, filterInfo->filterDesc, buf, nullptr, KSCREATE_ITEM_FREEONSTOP,
        nullptr, nullptr, &filterInfo->filterFactory);
    KsReleaseDevice(ksDevice);

    if (!NT_SUCCESS(status)) {
        goto err_out;
    }

    PUNICODE_STRING symlink =
        KsFilterFactoryGetSymbolicLink(filterInfo->filterFactory);

    status = IoSetDeviceInterfacePropertyData(symlink,
        &DEVPKEY_DeviceInterface_FriendlyName, LOCALE_NEUTRAL, 0,
        DEVPROP_TYPE_STRING, deviceName.Length + sizeof(WCHAR), deviceName.Buffer);

    if (!NT_SUCCESS(status)) {
        SAR_LOG("Couldn't set friendly name: %08X", status);
        goto err_out;
    }

    status = KsFilterFactorySetDeviceClassesState(filterInfo->filterFactory, TRUE);

    if (!NT_SUCCESS(status)) {
        SAR_LOG("Couldn't enable KS filter factory");
        goto err_out;
    }

    ExAcquireFastMutex(&fileContext->mutex);
    InsertTailList(&fileContext->endpointList, &filterInfo->listEntry);
    ExReleaseFastMutex(&fileContext->mutex);
    return status;

err_out:
    if (filterInfo->filterFactory) {
        KsAcquireDevice(ksDevice);
        KsDeleteFilterFactory(filterInfo->filterFactory);
        KsReleaseDevice(ksDevice);
    }

    if (filterInfo->allocatorFraming) {
        ExFreePoolWithTag(filterInfo->allocatorFraming, SAR_TAG);
    }

    if (filterInfo->dataRange) {
        ExFreePoolWithTag(filterInfo->dataRange, SAR_TAG);
    }

    if (filterInfo->pinDesc) {
        ExFreePoolWithTag(filterInfo->pinDesc, SAR_TAG);
    }

    if (filterInfo->filterDesc) {
        ExFreePoolWithTag(filterInfo->filterDesc, SAR_TAG);
    }

    ExFreePoolWithTag(filterInfo, SAR_TAG);
    return status;
}

NTSTATUS SarIrpCreate(PDEVICE_OBJECT deviceObject, PIRP irp)
{
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
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    SarInitializeFileContext(fileContext);
    irpStack->FileObject->FsContext2 = fileContext;
    irp->IoStatus.Status = STATUS_SUCCESS;
    IoCompleteRequest(irp, IO_NO_INCREMENT);
    return STATUS_SUCCESS;
}

NTSTATUS SarIrpClose(PDEVICE_OBJECT deviceObject, PIRP irp)
{
    NTSTATUS status;
    PIO_STACK_LOCATION irpStack;
    SarDriverExtension *extension =
        (SarDriverExtension *)IoGetDriverObjectExtension(
            deviceObject->DriverObject, DriverEntry);
    SarFileContext fileContextTemplate;
    BOOLEAN deleted;

    irpStack = IoGetCurrentIrpStackLocation(irp);
    fileContextTemplate.fileObject = irpStack->FileObject;
    ExAcquireFastMutex(&extension->fileContextLock);
    deleted = RtlDeleteElementGenericTable(
        &extension->fileContextTable, &fileContextTemplate);
    ExReleaseFastMutex(&extension->fileContextLock);

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

NTSTATUS SarIrpCleanup(PDEVICE_OBJECT deviceObject, PIRP irp)
{
    UNREFERENCED_PARAMETER(deviceObject);
    NTSTATUS status;
    PIO_STACK_LOCATION irpStack;
    SarDriverExtension *extension =
        (SarDriverExtension *)IoGetDriverObjectExtension(
            deviceObject->DriverObject, DriverEntry);
    SarFileContext fileContextTemplate;
    BOOLEAN deleted;

    irpStack = IoGetCurrentIrpStackLocation(irp);
    fileContextTemplate.fileObject = irpStack->FileObject;
    ExAcquireFastMutex(&extension->fileContextLock);
    deleted = RtlDeleteElementGenericTable(
        &extension->fileContextTable, &fileContextTemplate);
    ExReleaseFastMutex(&extension->fileContextLock);

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
        case SAR_REQUEST_CREATE_AUDIO_BUFFERS:
        case SAR_REQUEST_CREATE_ENDPOINT: {
            SAR_LOG("(SAR) Create endpoint");

            SarCreateEndpointRequest request;

            if (!ioctlInput(
                &ntStatus, irp, irpStack, (PVOID *)&request, sizeof(request))) {

                break;
            }

            SAR_LOG("(SAR) Create endpoint request: %d, %d, %d",
                request.type, request.index, request.channelCount);
            ntStatus = SarCreateEndpoint(
                deviceObject, extension, fileContext, &request);
            break;
        }
        case SAR_REQUEST_MAP_AUDIO_BUFFER:
        case SAR_REQUEST_AUDIO_TICK:
        default:
            SAR_LOG("(SAR) Unknown ioctl %d");
            break;
    }

    irp->IoStatus.Status = ntStatus;
    IoCompleteRequest(irp, IO_NO_INCREMENT);
    return ntStatus;
}

VOID SarUnload(PDRIVER_OBJECT driverObject)
{
    UNREFERENCED_PARAMETER(driverObject);
    SAR_LOG("SAR is unloading");
}

void SarInitializeFileContext(SarFileContext *fileContext)
{
    InitializeListHead(&fileContext->endpointList);
    ExInitializeFastMutex(&fileContext->mutex);
    fileContext->sampleRate = 0;
    fileContext->sampleDepth = 0;
    fileContext->bufferCount = 0;
    fileContext->bufferSize = 0;
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
    driverObject->MajorFunction[IRP_MJ_CREATE] = SarIrpCreate;
    driverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] = SarIrpDeviceControl;
    driverObject->MajorFunction[IRP_MJ_CLOSE] = SarIrpClose;
    driverObject->MajorFunction[IRP_MJ_CLEANUP] = SarIrpCleanup;
    return STATUS_SUCCESS;
}
