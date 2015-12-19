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

#include <initguid.h>
#include "sar.h"

DRIVER_DISPATCH SarIrpCreate;
DRIVER_DISPATCH SarIrpDeviceControl;
DRIVER_DISPATCH SarIrpClose;
DRIVER_DISPATCH SarIrpCleanup;
DRIVER_UNLOAD SarUnload;

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

SarControlContext *SarCreateControlContext(PFILE_OBJECT fileObject)
{
    SarControlContext *controlContext;

    controlContext = (SarControlContext *)ExAllocatePoolWithTag(
        NonPagedPool, sizeof(SarControlContext), SAR_TAG);

    if (!controlContext) {
        return nullptr;
    }

    RtlZeroMemory(controlContext, sizeof(SarControlContext));
    controlContext->refs = 1;
    controlContext->fileObject = fileObject;
    ExInitializeFastMutex(&controlContext->mutex);
    InitializeListHead(&controlContext->endpointList);
    InitializeListHead(&controlContext->pendingEndpointList);
    SarInitializeHandleQueue(&controlContext->handleQueue);
    controlContext->workItem = IoAllocateWorkItem(
        controlContext->fileObject->DeviceObject);

    if (!controlContext->workItem) {
        ExFreePoolWithTag(controlContext, SAR_TAG);
        return nullptr;
    }

    return controlContext;
}

VOID SarDeleteControlContext(SarControlContext *controlContext)
{
    while (!IsListEmpty(&controlContext->endpointList)) {
        PLIST_ENTRY entry = controlContext->endpointList.Flink;
        SarEndpoint *endpoint =
            CONTAINING_RECORD(entry, SarEndpoint, listEntry);

        RemoveEntryList(&endpoint->listEntry);
        SarReleaseEndpoint(endpoint);
    }

    while (!IsListEmpty(&controlContext->pendingEndpointList)) {
        PLIST_ENTRY entry = controlContext->pendingEndpointList.Flink;
        SarEndpoint *endpoint =
            CONTAINING_RECORD(entry, SarEndpoint, listEntry);

        RemoveEntryList(&endpoint->listEntry);
        SarReleaseEndpoint(endpoint);
    }

    if (controlContext->workItem) {
        IoFreeWorkItem(controlContext->workItem);
        controlContext->workItem = nullptr;
    }

    if (controlContext->bufferSection) {
        ZwClose(controlContext->bufferSection);
        controlContext->bufferSection = nullptr;
    }

    ExFreePoolWithTag(controlContext, SAR_TAG);
}

BOOLEAN SarOrphanControlContext(SarDriverExtension *extension, PIRP irp)
{
    PIO_STACK_LOCATION irpStack = IoGetCurrentIrpStackLocation(irp);
    SarControlContext *controlContext;
    LIST_ENTRY orphanEndpoints;

    ExAcquireFastMutex(&extension->mutex);
    controlContext = (SarControlContext *)SarGetTableEntry(
        &extension->controlContextTable, irpStack->FileObject);

    if (controlContext) {
        SarRemoveTableEntry(
            &extension->controlContextTable, irpStack->FileObject);
    }

    ExReleaseFastMutex(&extension->mutex);

    if (!controlContext) {
        return FALSE;
    }

    ExAcquireFastMutex(&controlContext->mutex);
    controlContext->orphan = TRUE;
    InitializeListHead(&orphanEndpoints);

    if (!IsListEmpty(&controlContext->endpointList)) {
        PLIST_ENTRY entry = controlContext->endpointList.Flink;

        RemoveEntryList(&controlContext->endpointList);
        InitializeListHead(&controlContext->endpointList);
        AppendTailList(&orphanEndpoints, entry);
    }

    ExReleaseFastMutex(&controlContext->mutex);

    while (!IsListEmpty(&orphanEndpoints)) {
        SarEndpoint *endpoint =
            CONTAINING_RECORD(orphanEndpoints.Flink, SarEndpoint, listEntry);

        RemoveEntryList(&endpoint->listEntry);
        SarOrphanEndpoint(endpoint);
    }

    SarReleaseControlContext(controlContext);
    return TRUE;
}

NTSTATUS SarIrpCreate(PDEVICE_OBJECT deviceObject, PIRP irp)
{
    NTSTATUS status = STATUS_SUCCESS;
    UNICODE_STRING referencePath;
    PIO_STACK_LOCATION irpStack = IoGetCurrentIrpStackLocation(irp);
    SarDriverExtension *extension =
        (SarDriverExtension *)IoGetDriverObjectExtension(
            deviceObject->DriverObject, DriverEntry);
    SarControlContext *controlContext = nullptr;

    RtlUnicodeStringInit(&referencePath, SAR_CONTROL_REFERENCE_STRING);

    if (RtlCompareUnicodeString(
        &irpStack->FileObject->FileName, &referencePath, TRUE) != 0) {

        return extension->ksDispatchCreate(deviceObject, irp);
    }

    controlContext = SarCreateControlContext(irpStack->FileObject);

    if (!controlContext) {
        status = STATUS_INSUFFICIENT_RESOURCES;
        goto out;
    }

    ExAcquireFastMutex(&extension->mutex);
    status = SarInsertTableEntry(
        &extension->controlContextTable, irpStack->FileObject,
        controlContext);
    ExReleaseFastMutex(&extension->mutex);

    if (!NT_SUCCESS(status)) {
        goto out;
    }

    irpStack->FileObject->FsContext2 = controlContext;

out:
    if (controlContext && !NT_SUCCESS(status)) {
        ExFreePoolWithTag(controlContext, SAR_TAG);
    }

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
    BOOLEAN deleted = SarOrphanControlContext(extension, irp);

    if (!deleted) {
        return extension->ksDispatchClose(deviceObject, irp);
    }

    status = STATUS_SUCCESS;
    irp->IoStatus.Status = status;
    IoCompleteRequest(irp, IO_NO_INCREMENT);
    return status;
}

NTSTATUS SarIrpCleanup(PDEVICE_OBJECT deviceObject, PIRP irp)
{
    UNREFERENCED_PARAMETER(deviceObject);
    NTSTATUS status;
    SarDriverExtension *extension =
        (SarDriverExtension *)IoGetDriverObjectExtension(
            deviceObject->DriverObject, DriverEntry);
    BOOLEAN deleted = SarOrphanControlContext(extension, irp);

    if (!deleted) {
        return extension->ksDispatchCleanup(deviceObject, irp);
    }

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
    SarControlContext *controlContext;

    irpStack = IoGetCurrentIrpStackLocation(irp);
    ioControlCode = irpStack->Parameters.DeviceIoControl.IoControlCode;

#ifdef LOUD
    SarDumpKsIoctl(irp);
#endif

    ExAcquireFastMutex(&extension->mutex);
    controlContext = (SarControlContext *)SarGetTableEntry(
        &extension->controlContextTable, irpStack->FileObject);
    ExReleaseFastMutex(&extension->mutex);

    if (!controlContext) {
        return extension->ksDispatchDeviceControl(deviceObject, irp);
    }

    switch (ioControlCode) {
        case SAR_SET_BUFFER_LAYOUT: {
            SAR_LOG("(SAR) create audio buffers");

            SarSetBufferLayoutRequest request;
            SarSetBufferLayoutResponse response;

            ntStatus = SarReadUserBuffer(
                &request, irp, sizeof(SarSetBufferLayoutRequest));

            if (!NT_SUCCESS(ntStatus)) {
                break;
            }

            ntStatus = SarSetBufferLayout(controlContext, &request, &response);

            if (!NT_SUCCESS(ntStatus)) {
                break;
            }

            ntStatus = SarWriteUserBuffer(
                &response, irp, sizeof(SarSetBufferLayoutResponse));
            break;
        }
        case SAR_CREATE_ENDPOINT: {
            SarCreateEndpointRequest request;

            ntStatus = SarReadUserBuffer(
                &request, irp, sizeof(SarCreateEndpointRequest));

            if (!NT_SUCCESS(ntStatus)) {
                break;
            }

            IoMarkIrpPending(irp);
            ntStatus = SarCreateEndpoint(
                deviceObject, irp, controlContext, &request);
            break;
        }
        case SAR_WAIT_HANDLE_QUEUE: {
            ntStatus = SarWaitHandleQueue(&controlContext->handleQueue, irp);
            break;
        }
        case SAR_START_REGISTRY_FILTER: {
            UNICODE_STRING filterAltitude;

            RtlUnicodeStringInit(&filterAltitude, L"360000");
            KeEnterCriticalRegion();
            ExAcquireFastMutexUnsafe(&extension->mutex);

            if (extension->filterCookie.QuadPart) {
                ExReleaseFastMutexUnsafe(&extension->mutex);
                KeLeaveCriticalRegion();
                ntStatus = STATUS_RESOURCE_IN_USE;
                break;
            }

            ntStatus = SarCopyProcessUser(
                PsGetCurrentProcess(), &extension->filterUser);

            if (!NT_SUCCESS(ntStatus)) {
                ExReleaseFastMutexUnsafe(&extension->mutex);
                KeLeaveCriticalRegion();
                break;
            }

            ntStatus = CmRegisterCallbackEx(
                SarRegistryCallback,
                &filterAltitude,
                deviceObject->DriverObject,
                extension,
                &extension->filterCookie,
                nullptr);
            ExReleaseFastMutexUnsafe(&extension->mutex);
            KeLeaveCriticalRegion();
            break;
        }
        case SAR_SEND_FORMAT_CHANGE_EVENT:
            ntStatus = SarSendFormatChangeEvent(extension);
            break;
        default:
            SAR_LOG("(SAR) Unknown ioctl %d", ioControlCode);
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

    if (extension->filterCookie.QuadPart) {
        CmUnRegisterCallback(extension->filterCookie);
    }

    if (extension->filterUser) {
        ExFreePool(extension->filterUser);
    }
}

BOOL SarFilterMatchesCurrentProcess(SarDriverExtension *extension)
{
    PTOKEN_USER tokenUser = nullptr;
    NTSTATUS status = SarCopyProcessUser(PsGetCurrentProcess(), &tokenUser);

    if (!NT_SUCCESS(status)) {
        return FALSE;
    }

    BOOL isMatch = RtlEqualSid(
        extension->filterUser->User.Sid,
        tokenUser->User.Sid);

    ExFreePool(tokenUser);
    return isMatch;
}

NTSTATUS SarFilterMMDeviceQuery(
    PREG_QUERY_VALUE_KEY_INFORMATION queryInfo,
    PUNICODE_STRING wrapperRegistrationPath)
{
    NTSTATUS status = STATUS_SUCCESS;
    OBJECT_ATTRIBUTES oa;
    HANDLE wrapperKey;
    ULONG resultLength = 0;
    UNICODE_STRING valueName = {};
    PVOID buffer = nullptr;

    InitializeObjectAttributes(
        &oa, wrapperRegistrationPath, OBJ_KERNEL_HANDLE,
        nullptr, nullptr);
    status = ZwOpenKeyEx(&wrapperKey, KEY_ALL_ACCESS, &oa, 0);

    if (!NT_SUCCESS(status)) {
        return STATUS_SUCCESS;
    }

    buffer = ExAllocatePoolWithTag(PagedPool, queryInfo->Length, SAR_TAG);

    if (!buffer) {
        ZwClose(wrapperKey);
        return STATUS_SUCCESS;
    }

    __try {
        RtlCopyMemory(
            buffer, queryInfo->KeyValueInformation, queryInfo->Length);
        status = ZwQueryValueKey(
            wrapperKey,
            &valueName,
            queryInfo->KeyValueInformationClass,
            buffer,
            queryInfo->Length,
            &resultLength);
        RtlCopyMemory(
            queryInfo->KeyValueInformation, buffer, queryInfo->Length);
        *queryInfo->ResultLength = resultLength;

        if (NT_SUCCESS(status)) {
            status = STATUS_CALLBACK_BYPASS;
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        status = GetExceptionCode();
    }

    ExFreePool(buffer);
    ZwClose(wrapperKey);
    return status;
}

NTSTATUS SarFilterMMDeviceEnum(
    PREG_ENUMERATE_VALUE_KEY_INFORMATION queryInfo,
    PUNICODE_STRING wrapperRegistrationPath)
{
    NTSTATUS status = STATUS_SUCCESS;
    OBJECT_ATTRIBUTES oa;
    HANDLE wrapperKey;
    ULONG resultLength = 0;
    UNICODE_STRING valueName = {};
    PVOID buffer = nullptr;

    InitializeObjectAttributes(
        &oa, wrapperRegistrationPath, OBJ_KERNEL_HANDLE,
        nullptr, nullptr);
    status = ZwOpenKeyEx(&wrapperKey, KEY_ALL_ACCESS, &oa, 0);

    if (!NT_SUCCESS(status)) {
        return STATUS_SUCCESS;
    }

    buffer = ExAllocatePoolWithTag(PagedPool, queryInfo->Length, SAR_TAG);

    if (!buffer) {
        ZwClose(wrapperKey);
        return STATUS_SUCCESS;
    }

    __try {
        RtlCopyMemory(
            buffer, queryInfo->KeyValueInformation, queryInfo->Length);
        status = ZwEnumerateValueKey(
            wrapperKey,
            queryInfo->Index,
            queryInfo->KeyValueInformationClass,
            buffer,
            queryInfo->Length,
            &resultLength);
        RtlCopyMemory(
            queryInfo->KeyValueInformation, buffer, queryInfo->Length);
        *queryInfo->ResultLength = resultLength;

        if (NT_SUCCESS(status)) {
            status = STATUS_CALLBACK_BYPASS;
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        status = GetExceptionCode();
    }

    ExFreePool(buffer);
    ZwClose(wrapperKey);
    return status;
}

#define CLSID_ROOT L"\\REGISTRY\\MACHINE\\SOFTWARE\\Classes\\CLSID\\"
#define WOW64_CLSID_ROOT \
    L"\\REGISTRY\\MACHINE\\SOFTWARE\\Classes\\Wow6432Node\\CLSID\\"

NTSTATUS SarRegistryCallback(PVOID context, PVOID argument1, PVOID argument2)
{
    UNREFERENCED_PARAMETER(argument2);

    UNICODE_STRING mmDeviceRegistrationPath;
    UNICODE_STRING wrapperRegistrationPath;
    NTSTATUS status;
    REG_NOTIFY_CLASS notifyClass = (REG_NOTIFY_CLASS)(ULONG_PTR)argument1;
    SarDriverExtension *extension = (SarDriverExtension *)context;

#ifdef _WIN64
    if (IoIs32bitProcess(nullptr)) {
        RtlUnicodeStringInit(&mmDeviceRegistrationPath,
            WOW64_CLSID_ROOT
            L"{BCDE0395-E52F-467C-8E3D-C4579291692E}\\InprocServer32");
        RtlUnicodeStringInit(&wrapperRegistrationPath,
            WOW64_CLSID_ROOT
            L"{9FB96668-9EDD-4574-AD77-76BD89659D5D}\\InprocServer32");
    } else {
#endif
        RtlUnicodeStringInit(&mmDeviceRegistrationPath,
            CLSID_ROOT
            L"{BCDE0395-E52F-467C-8E3D-C4579291692E}\\InprocServer32");
        RtlUnicodeStringInit(&wrapperRegistrationPath,
            CLSID_ROOT
            L"{9FB96668-9EDD-4574-AD77-76BD89659D5D}\\InprocServer32");
#ifdef _WIN64
    }
#endif

    switch (notifyClass) {
        case RegNtQueryValueKey: {
            PREG_QUERY_VALUE_KEY_INFORMATION queryInfo =
                (PREG_QUERY_VALUE_KEY_INFORMATION)argument2;
            PCUNICODE_STRING objectName;

            // Only filter the default value
            if (queryInfo->ValueName->Length != 0) {
                break;
            }

            status = CmCallbackGetKeyObjectID(
                &extension->filterCookie, queryInfo->Object,
                nullptr, &objectName);

            if (!NT_SUCCESS(status)) {
                break;
            }

            if (RtlCompareUnicodeString(
                &mmDeviceRegistrationPath, objectName, TRUE)) {

                break;
            }

            if (!SarFilterMatchesCurrentProcess(extension)) {
                break;
            }

            return SarFilterMMDeviceQuery(
                queryInfo, &wrapperRegistrationPath);
        }
        case RegNtEnumerateValueKey: {
            PREG_ENUMERATE_VALUE_KEY_INFORMATION queryInfo =
                (PREG_ENUMERATE_VALUE_KEY_INFORMATION)argument2;
            PCUNICODE_STRING objectName;

            if (queryInfo->Index != 0) {
                break;
            }

            status = CmCallbackGetKeyObjectID(
                &extension->filterCookie, queryInfo->Object,
                nullptr, &objectName);

            if (!NT_SUCCESS(status)) {
                break;
            }

            if (RtlCompareUnicodeString(
                &mmDeviceRegistrationPath, objectName, TRUE)) {

                break;
            }

            if (!SarFilterMatchesCurrentProcess(extension)) {
                break;
            }

            return SarFilterMMDeviceEnum(
                queryInfo, &wrapperRegistrationPath);
        }
    }

    return STATUS_SUCCESS;
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

    RtlZeroMemory(extension, sizeof(SarDriverExtension));
    ExInitializeFastMutex(&extension->mutex);
    SarInitializeTable(&extension->controlContextTable);
    extension->ksDispatchCreate = driverObject->MajorFunction[IRP_MJ_CREATE];
    extension->ksDispatchClose = driverObject->MajorFunction[IRP_MJ_CLOSE];
    extension->ksDispatchCleanup = driverObject->MajorFunction[IRP_MJ_CLEANUP];
    extension->ksDispatchDeviceControl =
        driverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL];
    extension->sarInterfaceName = {};
    driverObject->DriverUnload = SarUnload;
    driverObject->MajorFunction[IRP_MJ_CREATE] = SarIrpCreate;
    driverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] = SarIrpDeviceControl;
    driverObject->MajorFunction[IRP_MJ_CLOSE] = SarIrpClose;
    driverObject->MajorFunction[IRP_MJ_CLEANUP] = SarIrpCleanup;
    return STATUS_SUCCESS;
}
