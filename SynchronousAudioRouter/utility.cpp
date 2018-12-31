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

DRIVER_CANCEL SarCancelHandleQueueIrp;
RTL_GENERIC_COMPARE_ROUTINE SarCompareTableEntry;
RTL_GENERIC_ALLOCATE_ROUTINE SarAllocateTableEntry;
RTL_GENERIC_FREE_ROUTINE SarFreeTableEntry;
RTL_AVL_COMPARE_ROUTINE SarCompareStringTableEntry;
RTL_AVL_ALLOCATE_ROUTINE SarAllocateStringTableEntry;
RTL_AVL_FREE_ROUTINE SarFreeStringTableEntry;

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

SarControlContext *SarGetControlContextFromFileObject(
    SarDriverExtension *extension, PFILE_OBJECT fileObject)
{
    SarControlContext *controlContext;

    ExAcquireFastMutex(&extension->mutex);
    controlContext = (SarControlContext *)SarGetTableEntry(
        &extension->controlContextTable, fileObject);
    ExReleaseFastMutex(&extension->mutex);

    return controlContext;
}

// TODO: gotta go fast
#ifndef SAR_NDIS
SarEndpoint *SarGetEndpointFromIrp(PIRP irp, BOOLEAN retain)
{
    PKSFILTER filter = KsGetFilterFromIrp(irp);
    PKSFILTERFACTORY factory = KsFilterGetParentFilterFactory(filter);
    SarDriverExtension *extension = SarGetDriverExtensionFromIrp(irp);
    PVOID restartKey = nullptr;
    SarEndpoint *found = nullptr;

    ExAcquireFastMutex(&extension->mutex);

    FOR_EACH_GENERIC(
        &extension->controlContextTable, SarTableEntry,
        tableEntry, restartKey) {

        SarControlContext *controlContext =
            (SarControlContext *)tableEntry->value;

        ExAcquireFastMutex(&controlContext->mutex);

        PLIST_ENTRY entry = controlContext->endpointList.Flink;

        while (entry != &controlContext->endpointList) {
            SarEndpoint *endpoint =
                CONTAINING_RECORD(entry, SarEndpoint, listEntry);

            entry = entry->Flink;

            if (endpoint->filterFactory == factory) {
                found = endpoint;

                if (retain) {
                    SarRetainControlContext(controlContext);
                    SarRetainEndpoint(endpoint);
                }

                break;
            }
        }

        ExReleaseFastMutex(&controlContext->mutex);

        if (found) {
            break;
        }
    }

    ExReleaseFastMutex(&extension->mutex);
    return found;
}
#endif

NTSTATUS SarStringDuplicate(PUNICODE_STRING str, PCUNICODE_STRING src)
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

#ifndef SAR_NDIS
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
#endif

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

    HANDLE result = nullptr;
    NTSTATUS status;

    status = ZwDuplicateObject(
        kernelProcessHandle, userHandle,
        kernelTargetProcessHandle, &result, 0, 0,
        DUPLICATE_SAME_ACCESS);
    response->handle = result;
    return status;
}

void SarCancelAllHandleQueueIrps(SarHandleQueue *handleQueue)
{
    KIRQL irql;
    LIST_ENTRY pendingIrqsToCancel;

    InitializeListHead(&pendingIrqsToCancel);

    KeAcquireSpinLock(&handleQueue->lock, &irql);

    if (!IsListEmpty(&handleQueue->pendingIrps)) {
        PLIST_ENTRY entry = handleQueue->pendingIrps.Flink;

        RemoveEntryList(&handleQueue->pendingIrps);
        InitializeListHead(&handleQueue->pendingIrps);
        AppendTailList(&pendingIrqsToCancel, entry);
    }

    KeReleaseSpinLock(&handleQueue->lock, irql);

    while (!IsListEmpty(&pendingIrqsToCancel)) {
        SarHandleQueueIrp *pendingIrp =
            CONTAINING_RECORD(pendingIrqsToCancel.Flink, SarHandleQueueIrp, listEntry);
        PIRP irp = pendingIrp->irp;

        RemoveEntryList(&pendingIrp->listEntry);

        ZwClose(pendingIrp->kernelProcessHandle);
        ExFreePoolWithTag(pendingIrp, SAR_TAG);

        irp->IoStatus.Information = 0;
        irp->IoStatus.Status = STATUS_CANCELLED;
        IoSetCancelRoutine(irp, nullptr);
        IoCompleteRequest(irp, IO_NO_INCREMENT);
    }
}

void SarCancelHandleQueueIrp(PDEVICE_OBJECT deviceObject, PIRP irp)
{
    UNREFERENCED_PARAMETER(deviceObject);
    IoReleaseCancelSpinLock(irp->CancelIrql);

    PIO_STACK_LOCATION irpStack = IoGetCurrentIrpStackLocation(irp);
    SarDriverExtension *extension = SarGetDriverExtensionFromIrp(irp);
    SarControlContext *controlContext =
        SarGetControlContextFromFileObject(extension, irpStack->FileObject);
    PLIST_ENTRY entry;
    SarHandleQueueIrp *toCancel = nullptr;
    KIRQL irql;

    if (!controlContext) {
        return;
    }

    KeAcquireSpinLock(&controlContext->handleQueue.lock,  &irql);
    entry = controlContext->handleQueue.pendingIrps.Flink;

    while (entry != &controlContext->handleQueue.pendingIrps) {
        SarHandleQueueIrp *pendingIrp =
            CONTAINING_RECORD(entry, SarHandleQueueIrp, listEntry);

        entry = entry->Flink;

        if (pendingIrp->irp == irp) {
            RemoveEntryList(&pendingIrp->listEntry);
            toCancel = pendingIrp;
            break;
        }
    }

    KeReleaseSpinLock(&controlContext->handleQueue.lock, irql);

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

        ZwClose(kernelProcessHandle);
    }

    return status;
}

RTL_GENERIC_COMPARE_RESULTS NTAPI SarCompareTableEntry(
    PRTL_GENERIC_TABLE table, PVOID lhs, PVOID rhs)
{
    UNREFERENCED_PARAMETER(table);
    SarTableEntry *le = (SarTableEntry *)lhs;
    SarTableEntry *re = (SarTableEntry *)rhs;

    return le->key < re->key ? GenericLessThan :
        le->key == re->key ? GenericEqual : GenericGreaterThan;
}

PVOID NTAPI SarAllocateTableEntry(PRTL_GENERIC_TABLE table, CLONG byteSize)
{
    UNREFERENCED_PARAMETER(table);
    return ExAllocatePoolWithTag(NonPagedPool, byteSize, SAR_TAG);
}

VOID NTAPI SarFreeTableEntry(PRTL_GENERIC_TABLE table, PVOID buffer)
{
    UNREFERENCED_PARAMETER(table);
    ExFreePoolWithTag(buffer, SAR_TAG);
}

VOID SarInitializeTable(PRTL_GENERIC_TABLE table)
{
    RtlInitializeGenericTable(table,
        SarCompareTableEntry,  SarAllocateTableEntry, SarFreeTableEntry,
        nullptr);
}

NTSTATUS SarInsertTableEntry(PRTL_GENERIC_TABLE table, PVOID key, PVOID value)
{
    SarTableEntry entry = { key, value };
    BOOLEAN isNew = FALSE;
    PVOID result =
        RtlInsertElementGenericTable(table, &entry, sizeof(entry), &isNew);

    if (!result) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    if (!isNew) {
        return STATUS_OBJECT_NAME_EXISTS;
    }

    return STATUS_SUCCESS;
}

BOOLEAN SarRemoveTableEntry(PRTL_GENERIC_TABLE table, PVOID key)
{
    SarTableEntry entry = { key, nullptr };

    return RtlDeleteElementGenericTable(table, &entry);
}

PVOID SarGetTableEntry(PRTL_GENERIC_TABLE table, PVOID key)
{
    SarTableEntry entry = { key, nullptr };
    SarTableEntry *result =
        (SarTableEntry *)RtlLookupElementGenericTable(table, &entry);

    if (result) {
        return result->value;
    }

    return nullptr;
}

RTL_GENERIC_COMPARE_RESULTS NTAPI SarCompareStringTableEntry(
    PRTL_AVL_TABLE table, PVOID lhs, PVOID rhs)
{
    UNREFERENCED_PARAMETER(table);
    SarStringTableEntry *le = (SarStringTableEntry *)lhs;
    SarStringTableEntry *re = (SarStringTableEntry *)rhs;
    LONG result = RtlCompareUnicodeString(&le->key, &re->key, TRUE);

    return result < 0 ? GenericLessThan :
        result > 0 ? GenericGreaterThan : GenericEqual;
}

NTSTATUS SarInsertStringTableEntry(
    PRTL_AVL_TABLE table, PUNICODE_STRING key, PVOID value)
{
    NTSTATUS status = STATUS_SUCCESS;
    SarStringTableEntry entry = { {}, value };
    BOOLEAN isNew = FALSE;
    PVOID result = nullptr;

    status = SarStringDuplicate(&entry.key, key);

    if (!NT_SUCCESS(status)) {
        goto err;
    }

    result = RtlInsertElementGenericTableAvl(
        table, &entry, sizeof(entry), &isNew);

    if (!result) {
        status = STATUS_INSUFFICIENT_RESOURCES;
        goto err;
    }

    if (!isNew) {
        status = STATUS_OBJECT_NAME_EXISTS;
        goto err;
    }

    return STATUS_SUCCESS;

err:
    if (entry.key.Buffer) {
        SarStringFree(&entry.key);
    }

    return status;
}

BOOLEAN SarRemoveStringTableEntry(
    PRTL_AVL_TABLE table, PUNICODE_STRING key)
{
    SarStringTableEntry entry = { *key, nullptr };

    return RtlDeleteElementGenericTableAvl(table, &entry);
}

PVOID NTAPI SarAllocateStringTableEntry(PRTL_AVL_TABLE table, CLONG byteSize)
{
    UNREFERENCED_PARAMETER(table);
    return ExAllocatePoolWithTag(NonPagedPool, byteSize, SAR_TAG);
}

VOID NTAPI SarFreeStringTableEntry(PRTL_AVL_TABLE table, PVOID buffer)
{
    UNREFERENCED_PARAMETER(table);
    SarStringTableEntry *entry = (SarStringTableEntry *)(
        (PCHAR)buffer + sizeof(RTL_BALANCED_LINKS));

    if (entry->key.Buffer) {
        SarStringFree(&entry->key);
    }

    ExFreePoolWithTag(buffer, SAR_TAG);
}

PVOID SarGetStringTableEntry(PRTL_AVL_TABLE table, PCUNICODE_STRING key)
{
    SarStringTableEntry entry = { *key, nullptr };
    SarStringTableEntry *result =
        (SarStringTableEntry *)RtlLookupElementGenericTableAvl(table, &entry);

    if (result) {
        return result->value;
    }

    return nullptr;
}

VOID SarInitializeStringTable(PRTL_AVL_TABLE table)
{
    RtlInitializeGenericTableAvl(table,
        SarCompareStringTableEntry,
        SarAllocateStringTableEntry,
        SarFreeStringTableEntry,
        nullptr);
}

VOID SarClearStringTable(PRTL_AVL_TABLE table, VOID (*freeCb)(PVOID))
{
    PVOID entry;

    while ((entry = RtlGetElementGenericTableAvl(table, 0)) != NULL) {
        SarStringTableEntry *stEntry = (SarStringTableEntry *)entry;
        PVOID value = stEntry->value;

        RtlDeleteElementGenericTableAvl(table, entry);
        freeCb(value);
    }

    NT_ASSERT(RtlIsGenericTableEmptyAvl(table));
}

NTSTATUS SarCopyProcessUser(PEPROCESS process, PTOKEN_USER *outTokenUser)
{
    NT_ASSERT(process);
    NT_ASSERT(outTokenUser);

    PACCESS_TOKEN token = PsReferencePrimaryToken(process);
    NTSTATUS status = SeQueryInformationToken(
        token, TokenUser, (PVOID *)outTokenUser);

    PsDereferencePrimaryToken(token);
    return status;
}
