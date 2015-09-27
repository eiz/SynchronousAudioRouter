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

SarDriverExtension *SarGetDriverExtension(PDRIVER_OBJECT driverObject)
{
    return (SarDriverExtension *)IoGetDriverObjectExtension(
        driverObject, DriverEntry);
}

SarDriverExtension *SarGetDriverExtensionFromIrp(PIRP irp)
{
    PIO_STACK_LOCATION irpStack = IoGetCurrentIrpStackLocation(irp);

    return SarGetDriverExtension(irpStack->DeviceObject->DriverObject);
}

SarFileContext *SarGetFileContextFromFileObject(
    SarDriverExtension *extension, PFILE_OBJECT fileObject)
{
    SarFileContext *fileContext;
    SarFileContext fileContextTemplate;

    fileContextTemplate.fileObject = fileObject;
    ExAcquireFastMutex(&extension->fileContextLock);
    fileContext = (SarFileContext *)RtlLookupElementGenericTable(
        &extension->fileContextTable, (PVOID)&fileContextTemplate);
    ExReleaseFastMutex(&extension->fileContextLock);

    return fileContext;
}

// TODO: gotta go fast
SarEndpoint *SarGetEndpointFromIrp(PIRP irp)
{
    PKSFILTER filter = KsGetFilterFromIrp(irp);
    PKSFILTERFACTORY factory = KsFilterGetParentFilterFactory(filter);
    SarDriverExtension *extension = SarGetDriverExtensionFromIrp(irp);
    PVOID restartKey = nullptr;
    SarEndpoint *found = nullptr;

    ExAcquireFastMutex(&extension->fileContextLock);

    FOR_EACH_GENERIC(
        &extension->fileContextTable, SarFileContext, fileContext, restartKey) {
        ExAcquireFastMutex(&fileContext->mutex);

        PLIST_ENTRY entry = fileContext->endpointList.Flink;

        while (entry != &fileContext->endpointList) {
            SarEndpoint *endpoint =
                CONTAINING_RECORD(entry, SarEndpoint, listEntry);

            entry = entry->Flink;

            if (endpoint->filterFactory == factory) {
                found = endpoint;
                break;
            }
        }

        ExReleaseFastMutex(&fileContext->mutex);

        if (found) {
            break;
        }
    }

    ExReleaseFastMutex(&extension->fileContextLock);
    return found;
}

NTSTATUS SarStringDuplicate(PUNICODE_STRING str, PUNICODE_STRING src)
{
    PWCH buffer = (PWCH)ExAllocatePoolWithTag(
        NonPagedPool, src->MaximumLength, SAR_TAG);

    if (!buffer) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    RtlCopyMemory(buffer, src->Buffer, src->MaximumLength);
    str->Length = src->Length;
    str->MaximumLength = src->MaximumLength;
    str->Buffer = buffer;
    return STATUS_SUCCESS;
}

NTSTATUS SarReadEndpointRegisters(
    SarEndpointRegisters *regs, SarEndpoint *endpoint)
{
    SarEndpointProcessContext *context;
    NTSTATUS status;

    status = SarGetOrCreateEndpointProcessContext(
        endpoint, PsGetCurrentProcess(), &context);

    if (!NT_SUCCESS(status)) {
        return status;
    }

    __try {
        SarEndpointRegisters *source =
            &context->registerFileUVA[endpoint->index];

        ProbeForRead(
            source, sizeof(SarEndpointRegisters), TYPE_ALIGNMENT(ULONG));
        RtlCopyMemory(regs, source, sizeof(SarEndpointRegisters));
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        return GetExceptionCode();
    }

    return STATUS_SUCCESS;
}

NTSTATUS SarWriteEndpointRegisters(
    SarEndpointRegisters *regs, SarEndpoint *endpoint)
{
    SarEndpointProcessContext *context;
    NTSTATUS status;

    status = SarGetOrCreateEndpointProcessContext(
        endpoint, PsGetCurrentProcess(), &context);

    if (!NT_SUCCESS(status)) {
        return status;
    }

    __try {
        SarEndpointRegisters *dest = &context->registerFileUVA[endpoint->index];

        ProbeForWrite(dest,
            sizeof(SarEndpointRegisters), TYPE_ALIGNMENT(ULONG));
        dest->positionRegister = regs->positionRegister;
        dest->clockRegister = regs->clockRegister;
        dest->bufferOffset = regs->bufferOffset;
        dest->bufferSize = regs->bufferSize;
        InterlockedExchangePointer(&dest->eventHandle, regs->eventHandle);
        InterlockedExchange((LONG *)&dest->generation, (ULONG)regs->generation);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        return GetExceptionCode();
    }

    return STATUS_SUCCESS;
}

VOID SarStringFree(PUNICODE_STRING str)
{
    ExFreePoolWithTag(str->Buffer, SAR_TAG);
}

NTSTATUS SarReadUserBuffer(PVOID dest, PIRP irp, ULONG size)
{
    PIO_STACK_LOCATION irpStack = IoGetCurrentIrpStackLocation(irp);

    if (irpStack->Parameters.DeviceIoControl.InputBufferLength < size) {
        return STATUS_BUFFER_OVERFLOW;
    }

    __try {
        ProbeForRead(
            irpStack->Parameters.DeviceIoControl.Type3InputBuffer, size,
            TYPE_ALIGNMENT(ULONG));
        RtlCopyMemory(
            dest, irpStack->Parameters.DeviceIoControl.Type3InputBuffer, size);
        return STATUS_SUCCESS;
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        return GetExceptionCode();
    }
}

NTSTATUS SarWriteUserBuffer(PVOID src, PIRP irp, ULONG size)
{
    PIO_STACK_LOCATION irpStack = IoGetCurrentIrpStackLocation(irp);

    irp->IoStatus.Information = size;

    if (irpStack->Parameters.DeviceIoControl.OutputBufferLength == 0) {
        return STATUS_BUFFER_OVERFLOW;
    }

    if (irpStack->Parameters.DeviceIoControl.OutputBufferLength < size) {
        return STATUS_BUFFER_TOO_SMALL;
    }

    __try {
        ProbeForWrite(irp->UserBuffer, size, TYPE_ALIGNMENT(ULONG));
        RtlCopyMemory(irp->UserBuffer, src, size);
        return STATUS_SUCCESS;
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        return GetExceptionCode();
    }
}

void SarInitializeHandleQueue(SarHandleQueue *queue)
{
    ExInitializeFastMutex(&queue->mutex);
    InitializeListHead(&queue->pendingIrps);
    InitializeListHead(&queue->pendingItems);
}

NTSTATUS SarTransferQueuedHandle(
    SarHandleQueueIrp *queuedIrp, ULONG responseIndex,
    HANDLE kernelProcessHandle, HANDLE userHandle, ULONG64 associatedData)
{
    PIO_STACK_LOCATION irpStack =
        IoGetCurrentIrpStackLocation(queuedIrp->irp);

    if (irpStack->Parameters.DeviceIoControl.OutputBufferLength <
        sizeof(SarHandleQueueResponse) * (responseIndex + 1)) {

        return STATUS_BUFFER_TOO_SMALL;
    }

    SarHandleQueueResponse *response =
        &((SarHandleQueueResponse *)queuedIrp->irp->AssociatedIrp.SystemBuffer)
            [responseIndex];

    response->associatedData = associatedData;
    return ZwDuplicateObject(
        kernelProcessHandle, userHandle,
        queuedIrp->kernelProcessHandle, &response->handle, 0, 0,
        DUPLICATE_SAME_ACCESS);
}

NTSTATUS SarPostHandleQueue(
    SarHandleQueue *queue, HANDLE userHandle, ULONG64 associatedData)
{
    NTSTATUS status = STATUS_SUCCESS;
    HANDLE kernelProcessHandle = nullptr;
    SarHandleQueueIrp *queuedIrp = nullptr;

    status = ObOpenObjectByPointerWithTag(
        PsGetCurrentProcess(), OBJ_KERNEL_HANDLE,
        nullptr, GENERIC_ALL, nullptr,
        KernelMode, SAR_TAG, &kernelProcessHandle);

    if (!NT_SUCCESS(status)) {
        return status;
    }

    ExAcquireFastMutex(&queue->mutex);

    if (!IsListEmpty(&queue->pendingIrps)) {
        PLIST_ENTRY entry = queue->pendingIrps.Flink;

        queuedIrp =
            CONTAINING_RECORD(entry, SarHandleQueueIrp, listEntry);
        RemoveEntryList(entry);
    }

    if (!queuedIrp) {
        SarHandleQueueItem *item = (SarHandleQueueItem *)ExAllocatePoolWithTag(
            NonPagedPool, sizeof(SarHandleQueueItem), SAR_TAG);

        if (!item) {
            ExReleaseFastMutex(&queue->mutex);
            ZwClose(kernelProcessHandle);
            return STATUS_INSUFFICIENT_RESOURCES;
        }

        item->associatedData = associatedData;
        item->userHandle = userHandle;
        item->kernelProcessHandle = kernelProcessHandle;
        InsertTailList(&queue->pendingItems, &item->listEntry);
    }

    ExReleaseFastMutex(&queue->mutex);

    if (queuedIrp) {
        status = SarTransferQueuedHandle(
            queuedIrp, 0, kernelProcessHandle, userHandle, associatedData);

        queuedIrp->irp->IoStatus.Status = status;
        IoCompleteRequest(queuedIrp->irp, IO_NO_INCREMENT);
        ZwClose(kernelProcessHandle);
    }

    return status;
}

NTSTATUS SarWaitHandleQueue(SarHandleQueue *queue, PIRP irp)
{
    UNREFERENCED_PARAMETER(queue);
    UNREFERENCED_PARAMETER(irp);
    return STATUS_NOT_IMPLEMENTED;
}