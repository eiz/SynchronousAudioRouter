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
        dest->notificationCount = regs->notificationCount;
        MemoryBarrier();
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
    KeInitializeSpinLock(&queue->lock);
    InitializeListHead(&queue->pendingIrps);
    InitializeListHead(&queue->pendingItems);
}

NTSTATUS SarTransferQueuedHandle(
    PIRP irp, HANDLE kernelTargetProcessHandle, ULONG responseIndex,
    HANDLE kernelProcessHandle, HANDLE userHandle, ULONG64 associatedData)
{
    PIO_STACK_LOCATION irpStack = IoGetCurrentIrpStackLocation(irp);

    if (irpStack->Parameters.DeviceIoControl.OutputBufferLength <
        sizeof(SarHandleQueueResponse) * (responseIndex + 1)) {

        return STATUS_BUFFER_TOO_SMALL;
    }

    SarHandleQueueResponse *response =
        &((SarHandleQueueResponse *)irp->AssociatedIrp.SystemBuffer)
            [responseIndex];

    response->associatedData = associatedData;
    response->handle = nullptr;
    return ZwDuplicateObject(
        kernelProcessHandle, userHandle,
        kernelTargetProcessHandle, &response->handle, 0, 0,
        DUPLICATE_SAME_ACCESS);
}

void SarCancelHandleQueueIrp(PDEVICE_OBJECT deviceObject, PIRP irp)
{
    UNREFERENCED_PARAMETER(deviceObject);
    IoReleaseCancelSpinLock(irp->CancelIrql);

    PIO_STACK_LOCATION irpStack = IoGetCurrentIrpStackLocation(irp);
    SarDriverExtension *extension = SarGetDriverExtensionFromIrp(irp);
    SarFileContext *fileContext =
        SarGetFileContextFromFileObject(extension, irpStack->FileObject);
    PLIST_ENTRY entry;
    SarHandleQueueIrp *toCancel = nullptr;
    KIRQL irql;

    if (!fileContext) {
        return;
    }

    KeAcquireSpinLock(&fileContext->handleQueue.lock,  &irql);
    entry = fileContext->handleQueue.pendingIrps.Flink;

    while (entry != &fileContext->handleQueue.pendingIrps) {
        SarHandleQueueIrp *pendingIrp =
            CONTAINING_RECORD(entry, SarHandleQueueIrp, listEntry);

        entry = entry->Flink;

        if (pendingIrp->irp == irp) {
            RemoveEntryList(entry);
            toCancel = pendingIrp;
            break;
        }
    }

    KeReleaseSpinLock(&fileContext->handleQueue.lock, irql);

    if (toCancel) {
        ZwClose(toCancel->kernelProcessHandle);
        ExFreePoolWithTag(toCancel, SAR_TAG);
        irp->IoStatus.Information = 0;
        irp->IoStatus.Status = STATUS_CANCELLED;
        IoCompleteRequest(irp, IO_NO_INCREMENT);
    }
}

NTSTATUS SarPostHandleQueue(
    SarHandleQueue *queue, HANDLE userHandle, ULONG64 associatedData)
{
    NTSTATUS status = STATUS_SUCCESS;
    HANDLE kernelProcessHandle = nullptr;
    SarHandleQueueIrp *queuedIrp = nullptr;
    KIRQL irql;

    status = ObOpenObjectByPointerWithTag(
        PsGetCurrentProcess(), OBJ_KERNEL_HANDLE,
        nullptr, GENERIC_ALL, nullptr,
        KernelMode, SAR_TAG, &kernelProcessHandle);

    if (!NT_SUCCESS(status)) {
        return status;
    }

    KeAcquireSpinLock(&queue->lock, &irql);

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
            KeReleaseSpinLock(&queue->lock, irql);
            ZwClose(kernelProcessHandle);
            return STATUS_INSUFFICIENT_RESOURCES;
        }

        item->associatedData = associatedData;
        item->userHandle = userHandle;
        item->kernelProcessHandle = kernelProcessHandle;
        InsertTailList(&queue->pendingItems, &item->listEntry);
    }

    KeReleaseSpinLock(&queue->lock, irql);

    if (queuedIrp) {
        status = SarTransferQueuedHandle(
            queuedIrp->irp, queuedIrp->kernelProcessHandle,
            0, kernelProcessHandle, userHandle, associatedData);

        queuedIrp->irp->IoStatus.Status = status;
        queuedIrp->irp->IoStatus.Information = sizeof(SarHandleQueueResponse);
        IoSetCancelRoutine(queuedIrp->irp, nullptr);
        IoCompleteRequest(queuedIrp->irp, IO_NO_INCREMENT);
        ZwClose(kernelProcessHandle);
        ExFreePoolWithTag(queuedIrp, SAR_TAG);
    }

    return status;
}

NTSTATUS SarWaitHandleQueue(SarHandleQueue *queue, PIRP irp)
{
    NTSTATUS status = STATUS_SUCCESS;
    HANDLE kernelProcessHandle = nullptr;
    PIO_STACK_LOCATION irpStack = IoGetCurrentIrpStackLocation(irp);
    DWORD maxItems = irpStack->Parameters.DeviceIoControl.OutputBufferLength /
        sizeof(SarHandleQueueResponse);
    DWORD nextItem = 0;
    LIST_ENTRY toComplete;
    KIRQL irql;

    InitializeListHead(&toComplete);
    irp->IoStatus.Information = 0;

    if (maxItems == 0) {
        irp->IoStatus.Information = sizeof(SarHandleQueueResponse);
        irp->IoStatus.Status = STATUS_BUFFER_TOO_SMALL;
        IoCompleteRequest(irp, IO_NO_INCREMENT);
        return STATUS_BUFFER_TOO_SMALL;
    }

    status = ObOpenObjectByPointerWithTag(
        PsGetCurrentProcess(), OBJ_KERNEL_HANDLE,
        nullptr, GENERIC_ALL, nullptr,
        KernelMode, SAR_TAG, &kernelProcessHandle);

    if (!NT_SUCCESS(status)) {
        return status;
    }

    KeAcquireSpinLock(&queue->lock, &irql);

    while (!IsListEmpty(&queue->pendingItems) && nextItem < maxItems) {
        PLIST_ENTRY entry = RemoveHeadList(&queue->pendingItems);

        InsertTailList(&toComplete, entry);
        nextItem++;
    }

    if (IsListEmpty(&toComplete)) {
        SarHandleQueueIrp *queuedIrp = (SarHandleQueueIrp *)
            ExAllocatePoolWithTag(
                NonPagedPool, sizeof(SarHandleQueueIrp), SAR_TAG);

        if (!queuedIrp) {
            KeReleaseSpinLock(&queue->lock, irql);
            ZwClose(kernelProcessHandle);
            return STATUS_INSUFFICIENT_RESOURCES;
        }

        IoMarkIrpPending(irp);
        IoSetCancelRoutine(irp, SarCancelHandleQueueIrp);
        queuedIrp->irp = irp;
        queuedIrp->kernelProcessHandle = kernelProcessHandle;
        InsertTailList(&queue->pendingIrps, &queuedIrp->listEntry);
        status = STATUS_PENDING;
    }

    KeReleaseSpinLock(&queue->lock, irql);

    if (!IsListEmpty(&toComplete)) {
        nextItem = 0;

        while (!IsListEmpty(&toComplete)) {
            PLIST_ENTRY entry = RemoveHeadList(&toComplete);
            SarHandleQueueItem *queueItem =
                CONTAINING_RECORD(entry, SarHandleQueueItem, listEntry);

            status = SarTransferQueuedHandle(
                irp, kernelProcessHandle, nextItem++,
                queueItem->kernelProcessHandle, queueItem->userHandle,
                queueItem->associatedData);
            ZwClose(queueItem->kernelProcessHandle);
            ExFreePoolWithTag(queueItem, SAR_TAG);
            irp->IoStatus.Information += sizeof(SarHandleQueueResponse);

            if (!NT_SUCCESS(status)) {
                break;
            }
        }

        irp->IoStatus.Status = status;
        IoCompleteRequest(irp, IO_NO_INCREMENT);
        ZwClose(kernelProcessHandle);
    }

    return status;
}
