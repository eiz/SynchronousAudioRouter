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

static void SarDeleteRegistryRedirect(PVOID ptr);

DRIVER_UNLOAD SarUnload;
EX_CALLBACK_FUNCTION SarRegistryCallback;
_Dispatch_type_(IRP_MJ_CREATE) DRIVER_DISPATCH SarIrpCreate;
_Dispatch_type_(IRP_MJ_DEVICE_CONTROL) DRIVER_DISPATCH SarIrpDeviceControl;
_Dispatch_type_(IRP_MJ_CLOSE) DRIVER_DISPATCH SarIrpClose;
_Dispatch_type_(IRP_MJ_CLEANUP) DRIVER_DISPATCH SarIrpCleanup;

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

    if (controlContext->bufferMapStorage) {
        ExFreePoolWithTag(controlContext->bufferMapStorage, SAR_TAG);
        controlContext->bufferMapStorage = nullptr;
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

    SarCancelAllHandleQueueIrps(&controlContext->handleQueue);

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
        SarReleaseControlContext(controlContext);
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
            ntStatus = SarSendFormatChangeEvent(deviceObject, extension);
            break;
        default:
            SAR_LOG("(SAR) Unknown ioctl %lu", ioControlCode);
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
    KIRQL irql;
    SarDriverExtension *extension =
        (SarDriverExtension *)IoGetDriverObjectExtension(
            driverObject, DriverEntry);

    if (extension->sarInterfaceName.Buffer) {
        RtlFreeUnicodeString(&extension->sarInterfaceName);
    }

    if (extension->filterCookie.QuadPart) {
        CmUnRegisterCallback(extension->filterCookie);
    }

    irql = ExAcquireSpinLockExclusive(&extension->registryRedirectLock);
    SarClearStringTable(
        &extension->registryRedirectTableWow64, SarDeleteRegistryRedirect);
    SarClearStringTable(
        &extension->registryRedirectTable, SarDeleteRegistryRedirect);
    ExReleaseSpinLockExclusive(&extension->registryRedirectLock, irql);

    if (extension->filterUser) {
        // Allocated by SeQueryInformationToken
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

BOOL SarFilterMatchesPath(
    SarDriverExtension *extension,
    PCUNICODE_STRING path,
    PUNICODE_STRING redirectPath)
{
    PRTL_AVL_TABLE table = &extension->registryRedirectTable;
    PVOID entry = nullptr;
    KIRQL irql;
    UNICODE_STRING nonpagedPath;
    NTSTATUS status = STATUS_SUCCESS;

    if (!path) {
        return FALSE;
    }

#ifdef _WIN64
    if (IoIs32bitProcess(nullptr)) {
        table = &extension->registryRedirectTableWow64;
    }
#endif

    // TODO: we get a path that's in paged memory from the configuration manager
    // so we can't access it with IRQL raised to dispatch level by the spinlock.
    // Probably better to just use an ERESOURCE or something here instead.
    status = SarStringDuplicate(&nonpagedPath, path);

    if (!NT_SUCCESS(status)) {
        return FALSE;
    }

    irql = ExAcquireSpinLockShared(&extension->registryRedirectLock);
    entry = SarGetStringTableEntry(table, &nonpagedPath);

    if (entry) {
        *redirectPath = *(PCUNICODE_STRING)entry;
    }

    ExReleaseSpinLockShared(&extension->registryRedirectLock, irql);
    SarStringFree(&nonpagedPath);
    return entry != nullptr;
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

    ExFreePoolWithTag(buffer, SAR_TAG);
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

    ExFreePoolWithTag(buffer, SAR_TAG);
    ZwClose(wrapperKey);
    return status;
}

#define CLSID_ROOT L"\\REGISTRY\\MACHINE\\SOFTWARE\\Classes\\CLSID\\"
#define WOW64_CLSID_ROOT \
    L"\\REGISTRY\\MACHINE\\SOFTWARE\\Classes\\Wow6432Node\\CLSID\\"

NTSTATUS SarRegistryCallback(PVOID context, PVOID argument1, PVOID argument2)
{
    UNREFERENCED_PARAMETER(argument2);

    UNICODE_STRING redirectPath;
    NTSTATUS status;
    REG_NOTIFY_CLASS notifyClass = (REG_NOTIFY_CLASS)(ULONG_PTR)argument1;
    SarDriverExtension *extension = (SarDriverExtension *)context;

    switch (notifyClass) {
        case RegNtQueryValueKey: {
            PREG_QUERY_VALUE_KEY_INFORMATION queryInfo =
                (PREG_QUERY_VALUE_KEY_INFORMATION)argument2;
            PCUNICODE_STRING objectName;

            // Only filter the default value
            if (!queryInfo || queryInfo->ValueName->Length != 0) {
                break;
            }

            status = CmCallbackGetKeyObjectID(
                &extension->filterCookie, queryInfo->Object,
                nullptr, &objectName);

            if (!NT_SUCCESS(status)) {
                break;
            }

            if (!SarFilterMatchesPath(extension, objectName, &redirectPath)) {
                break;
            }

            if (!SarFilterMatchesCurrentProcess(extension)) {
                break;
            }

            return SarFilterMMDeviceQuery(queryInfo, &redirectPath);
        }
        case RegNtEnumerateValueKey: {
            PREG_ENUMERATE_VALUE_KEY_INFORMATION queryInfo =
                (PREG_ENUMERATE_VALUE_KEY_INFORMATION)argument2;
            PCUNICODE_STRING objectName;

            if (!queryInfo || queryInfo->Index != 0) {
                break;
            }

            status = CmCallbackGetKeyObjectID(
                &extension->filterCookie, queryInfo->Object,
                nullptr, &objectName);

            if (!NT_SUCCESS(status)) {
                break;
            }

            if (!SarFilterMatchesPath(extension, objectName, &redirectPath)) {
                break;
            }

            if (!SarFilterMatchesCurrentProcess(extension)) {
                break;
            }

            return SarFilterMMDeviceEnum(queryInfo, &redirectPath);
        }
    }

    return STATUS_SUCCESS;
}

static void SarDeleteRegistryRedirect(PVOID ptr)
{
    PUNICODE_STRING str = (PUNICODE_STRING)ptr;

    if (str->Buffer) {
        SarStringFree(str);
    }

    ExFreePoolWithTag(str, SAR_TAG);
}

static NTSTATUS SarAddRegistryRedirect(
    PRTL_AVL_TABLE table, NTSTRSAFE_PCWSTR src, NTSTRSAFE_PCWSTR dst)
{
    NTSTATUS status = STATUS_SUCCESS;
    UNICODE_STRING srcLocal = {}, dstLocal = {};
    PUNICODE_STRING dstHeap = nullptr;

    RtlUnicodeStringInit(&srcLocal, src);
    RtlUnicodeStringInit(&dstLocal, dst);

    dstHeap = (PUNICODE_STRING)ExAllocatePoolWithTag(
        NonPagedPool, sizeof(UNICODE_STRING), SAR_TAG);

    if (!dstHeap) {
        status = STATUS_INSUFFICIENT_RESOURCES;
        goto err;
    }

    RtlZeroMemory(dstHeap, sizeof(UNICODE_STRING));
    status = SarStringDuplicate(dstHeap, &dstLocal);

    if (!NT_SUCCESS(status)) {
        goto err;
    }

    status = SarInsertStringTableEntry(table, &srcLocal, dstHeap);

    if (!NT_SUCCESS(status)) {
        goto err;
    }

    return STATUS_SUCCESS;

err:
    if (dstHeap) {
        if (dstHeap->Buffer) {
            SarStringFree(dstHeap);
        }

        ExFreePoolWithTag(dstHeap, SAR_TAG);
    }

    return status;
}

#define REDIRECT_INPROC_WOW64(src, dst) \
    do { \
        status = SarAddRegistryRedirect(wow64, \
            WOW64_CLSID_ROOT src L"\\InprocServer32", \
            WOW64_CLSID_ROOT dst L"\\InprocServer32"); \
        if (!NT_SUCCESS(status)) { \
            return status; \
        } \
    } while (0)
#define REDIRECT_INPROC(src, dst) \
    do { \
        status = SarAddRegistryRedirect(table, \
            CLSID_ROOT src L"\\InprocServer32", \
            CLSID_ROOT dst L"\\InprocServer32"); \
        if (!NT_SUCCESS(status)) { \
            return status; \
        } \
    } while (0)
#define REDIRECT(src, dst) \
    do { \
        REDIRECT_INPROC_WOW64(src, dst); \
        REDIRECT_INPROC(src, dst); \
    } while (0)

static NTSTATUS SarAddAllRegistryRedirects(SarDriverExtension *extension)
{
    NTSTATUS status = STATUS_SUCCESS;
    PVOID p;
    PRTL_AVL_TABLE wow64 = &extension->registryRedirectTableWow64;
    PRTL_AVL_TABLE table = &extension->registryRedirectTable;

    // MMDeviceEnumerator
    REDIRECT(
        L"{BCDE0395-E52F-467C-8E3D-C4579291692E}",
        L"{9FB96668-9EDD-4574-AD77-76BD89659D5D}");
    // ActivateAudioInterfaceWorker
    REDIRECT(
        L"{E2F7A62A-862B-40AE-BBC2-5C0CA9A5B7E1}",
        L"{739191CC-CCBE-45D8-8D24-828D8E989E8E}"
    );

    for (p = RtlEnumerateGenericTableAvl(table, TRUE); p;
         p = RtlEnumerateGenericTableAvl(table, FALSE)) {

        SarStringTableEntry *entry = (SarStringTableEntry *)p;
        SAR_LOG("Registry mapping: %wZ -> %wZ", &entry->key, entry->value);
    }

    for (p = RtlEnumerateGenericTableAvl(wow64, TRUE); p;
         p = RtlEnumerateGenericTableAvl(wow64, FALSE)) {

        SarStringTableEntry *entry = (SarStringTableEntry *)p;
        SAR_LOG("WOW64 Registry mapping: %wZ -> %wZ",
            &entry->key, entry->value);
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
    SarInitializeStringTable(&extension->registryRedirectTableWow64);
    SarInitializeStringTable(&extension->registryRedirectTable);
    status = SarAddAllRegistryRedirects(extension);

    if (!NT_SUCCESS(status)) {
        SarUnload(driverObject);
        return status;
    }

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
