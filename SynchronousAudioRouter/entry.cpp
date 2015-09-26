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

    RtlUnicodeStringInit(&referencePath, SAR_CONTROL_REFERENCE_STRING);

    if (RtlCompareUnicodeString(
        &irpStack->FileObject->FileName, &referencePath, TRUE) != 0) {

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
        return extension->ksDispatchClose(deviceObject, irp);
    }

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
    ExReleaseFastMutex(&extension->fileContextLock);

    if (fileContext) {
        ExAcquireFastMutex(&fileContext->mutex);

        PLIST_ENTRY entry = fileContext->endpointList.Flink;

        while (entry != &fileContext->endpointList) {
            //SarEndpoint *endpoint =
            //    CONTAINING_RECORD(entry, SarEndpoint, listEntry);

            entry = entry->Flink;
            // TODO: destroy ks filters when sar control device is closed
            SAR_LOG("This should delete filters but the locking is tricky");
        }

        if (fileContext->workItem) {
            IoFreeWorkItem(fileContext->workItem);
        }

        ExReleaseFastMutex(&fileContext->mutex);

        if (fileContext->bufferSection) {
            ZwClose(fileContext->bufferSection);
        }
    }

    ExAcquireFastMutex(&extension->fileContextLock);
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
    SarFileContext *fileContext;
    SarFileContext fileContextTemplate;

    irpStack = IoGetCurrentIrpStackLocation(irp);
    ioControlCode = irpStack->Parameters.DeviceIoControl.IoControlCode;

#ifdef LOUD
    SarDumpKsIoctl(irp);
#endif

    fileContextTemplate.fileObject = irpStack->FileObject;
    ExAcquireFastMutex(&extension->fileContextLock);
    fileContext = (SarFileContext *)RtlLookupElementGenericTable(
        &extension->fileContextTable, (PVOID)&fileContextTemplate);
    ExReleaseFastMutex(&extension->fileContextLock);

    if (!fileContext) {
        return extension->ksDispatchDeviceControl(deviceObject, irp);
    }

    switch (ioControlCode) {
        case SAR_REQUEST_SET_BUFFER_LAYOUT: {
            SAR_LOG("(SAR) create audio buffers");

            SarSetBufferLayoutRequest request;
            SarSetBufferLayoutResponse response;

            ntStatus = SarReadUserBuffer(
                &request, irp, sizeof(SarSetBufferLayoutRequest));

            if (!NT_SUCCESS(ntStatus)) {
                break;
            }

            ntStatus = SarSetBufferLayout(fileContext, &request, &response);

            if (!NT_SUCCESS(ntStatus)) {
                break;
            }

            ntStatus = SarWriteUserBuffer(
                &response, irp, sizeof(SarSetBufferLayoutResponse));
            break;
        }
        case SAR_REQUEST_CREATE_ENDPOINT: {
            SarCreateEndpointRequest request;

            ntStatus = SarReadUserBuffer(
                &request, irp, sizeof(SarCreateEndpointRequest));

            if (!NT_SUCCESS(ntStatus)) {
                break;
            }

            IoMarkIrpPending(irp);
            ntStatus = SarCreateEndpoint(
                deviceObject, irp, extension, fileContext, &request);
            break;
        }
        case SAR_REQUEST_GET_NOTIFICATION_EVENTS: {
            ntStatus = SarWaitHandleQueue(
                &fileContext->handleQueue, PsGetCurrentProcess(), irp);
            break;
        }
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
}

NTSTATUS SarInitializeFileContext(SarFileContext *fileContext)
{
    PFILE_OBJECT fileObject = fileContext->fileObject;

    RtlZeroMemory(fileContext, sizeof(SarFileContext));
    fileContext->fileObject = fileObject;
    ExInitializeFastMutex(&fileContext->mutex);
    InitializeListHead(&fileContext->endpointList);
    InitializeListHead(&fileContext->pendingEndpointList);
    SarInitializeHandleQueue(&fileContext->handleQueue);
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
    return ExAllocatePoolWithTag(NonPagedPool, byteSize, SAR_TAG);
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
