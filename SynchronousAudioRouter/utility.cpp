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

        SAR_TRACE("Searching for factory %p", factory);

        while (entry != &controlContext->endpointList) {
            SarEndpoint *endpoint =
                CONTAINING_RECORD(entry, SarEndpoint, listEntry);

            entry = entry->Flink;

            SAR_TRACE(" Endpoint %p, topology %p", endpoint->filterFactory, endpoint->topologyFilterFactory);
            if (endpoint->filterFactory == factory || endpoint->topologyFilterFactory == factory) {
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
    PWCH buffer = (PWCH)ExAllocatePool2(
        POOL_FLAG_NON_PAGED, src->MaximumLength, SAR_TAG);

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
        dest->bufferOffset = regs->bufferOffset;
        dest->bufferSize = regs->bufferSize;
        dest->notificationCount = regs->notificationCount;
        dest->activeChannelCount = regs->activeChannelCount;
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
        if (irp->RequestorMode != KernelMode) {
            ProbeForRead(
                irpStack->Parameters.DeviceIoControl.Type3InputBuffer, size,
                TYPE_ALIGNMENT(ULONG));
        }
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

void SarInitializeHandleQueue(SarHandleQueue *queue, PKSPIN_LOCK lock)
{
    queue->lock = lock;
    queue->closed = FALSE;
    InitializeListHead(&queue->pendingIrps);
    InitializeListHead(&queue->pendingItems);
}

// Opens a handle to a notification event for the process that is collecting
// it, and writes the handle, with its associated data, into response slot
// responseIndex of that process's SAR_WAIT_HANDLE_QUEUE request. Only ever
// called on the requesting thread, so the handle goes into the right handle
// table without the driver having to attach to another process.
static NTSTATUS SarTransferQueuedEvent(
    PIRP irp, ULONG responseIndex, PKEVENT event, ULONG64 associatedData)
{
    PIO_STACK_LOCATION irpStack = IoGetCurrentIrpStackLocation(irp);

    if (irpStack->Parameters.DeviceIoControl.OutputBufferLength <
        sizeof(SarHandleQueueResponse) * (responseIndex + 1)) {

        return STATUS_BUFFER_TOO_SMALL;
    }

    HANDLE handle = nullptr;
    NTSTATUS status;

    // The host only ever signals the event.
    status = ObOpenObjectByPointer(
        event, 0, nullptr, EVENT_MODIFY_STATE | SYNCHRONIZE,
        *ExEventObjectType, KernelMode, &handle);

    if (!NT_SUCCESS(status)) {
        return status;
    }

    SarHandleQueueResponse *response =
        &((SarHandleQueueResponse *)irp->AssociatedIrp.SystemBuffer)
            [responseIndex];

    response->associatedData = associatedData;
    response->handle = handle;
    return STATUS_SUCCESS;
}

// Takes the next waiting request off a queue. Call with the queue lock held.
//
// Whoever clears a request's cancel routine owns the request and completes
// it. If the routine is already gone, IoCancelIrp has taken it and is about
// to call it, or already has: that request is unlinked here and left to
// SarCancelHandleQueueIrp.
static PIRP SarDequeueWaitingIrp(SarHandleQueue *queue)
{
    while (!IsListEmpty(&queue->pendingIrps)) {
        PLIST_ENTRY entry = RemoveHeadList(&queue->pendingIrps);
        PIRP irp = CONTAINING_RECORD(entry, IRP, Tail.Overlay.ListEntry);

        if (IoSetCancelRoutine(irp, nullptr)) {
            return irp;
        }

        // The cancel routine removes the entry again; make that harmless.
        InitializeListHead(entry);
    }

    return nullptr;
}

// Completes a request that was waiting on a queue, with no events in it. The
// caller must own the request.
static void SarCompleteWaitingIrp(PIRP irp, NTSTATUS status)
{
    irp->IoStatus.Status = status;
    irp->IoStatus.Information = 0;
    IoCompleteRequest(irp, IO_NO_INCREMENT);
}

void SarCloseHandleQueue(SarHandleQueue *queue)
{
    LIST_ENTRY irps, items;
    PIRP irp;
    KIRQL irql;

    InitializeListHead(&irps);
    InitializeListHead(&items);
    KeAcquireSpinLock(queue->lock, &irql);
    queue->closed = TRUE;

    while ((irp = SarDequeueWaitingIrp(queue)) != nullptr) {
        InsertTailList(&irps, &irp->Tail.Overlay.ListEntry);
    }

    while (!IsListEmpty(&queue->pendingItems)) {
        PLIST_ENTRY entry = RemoveHeadList(&queue->pendingItems);

        InsertTailList(&items, entry);
    }

    KeReleaseSpinLock(queue->lock, irql);

    while (!IsListEmpty(&irps)) {
        PLIST_ENTRY entry = RemoveHeadList(&irps);

        irp = CONTAINING_RECORD(entry, IRP, Tail.Overlay.ListEntry);
        SAR_INFO("Cancelling IRP %p", irp);
        SarCompleteWaitingIrp(irp, STATUS_CANCELLED);
    }

    while (!IsListEmpty(&items)) {
        PLIST_ENTRY entry = RemoveHeadList(&items);
        SarHandleQueueItem *item =
            CONTAINING_RECORD(entry, SarHandleQueueItem, listEntry);

        ObDereferenceObject(item->event);
        ExFreePoolWithTag(item, SAR_TAG);
    }
}

void SarCancelHandleQueueIrp(PDEVICE_OBJECT deviceObject, PIRP irp)
{
    UNREFERENCED_PARAMETER(deviceObject);
    KIRQL irql;

    IoReleaseCancelSpinLock(irp->CancelIrql);

    // IoCancelIrp cleared the cancel routine before calling it, so this
    // routine owns the request: nobody else completes it, and it is safe to
    // touch. It is either still linked on its queue, or a dequeuer has
    // unlinked it and left its entry pointing at itself. Removing it is
    // right in both cases and needs only the lock, which the request
    // carries and which outlives every queue.
    PKSPIN_LOCK lock = (PKSPIN_LOCK)irp->Tail.Overlay.DriverContext[0];

    KeAcquireSpinLock(lock, &irql);
    RemoveEntryList(&irp->Tail.Overlay.ListEntry);
    KeReleaseSpinLock(lock, irql);
    SarCompleteWaitingIrp(irp, STATUS_CANCELLED);
}

NTSTATUS SarPostHandleQueue(
    SarHandleQueue *queue, PKEVENT event, ULONG64 associatedData)
{
    NTSTATUS status = STATUS_SUCCESS;
    PIRP waitingIrp = nullptr;
    BOOLEAN closed;
    KIRQL irql;

    // Allocated before the lock is taken; it is only needed if nobody is
    // waiting.
    SarHandleQueueItem *item = (SarHandleQueueItem *)ExAllocatePool2(
        POOL_FLAG_NON_PAGED, sizeof(SarHandleQueueItem), SAR_TAG);

    if (!item) {
        ObDereferenceObject(event);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    item->event = event;
    item->associatedData = associatedData;
    KeAcquireSpinLock(queue->lock, &irql);
    closed = queue->closed;

    if (!closed) {
        InsertTailList(&queue->pendingItems, &item->listEntry);
        waitingIrp = SarDequeueWaitingIrp(queue);
    }

    KeReleaseSpinLock(queue->lock, irql);

    if (closed) {
        // The host closed its control handle; nobody will collect this.
        ObDereferenceObject(event);
        ExFreePoolWithTag(item, SAR_TAG);
        return STATUS_CANCELLED;
    }

    // This runs on the registering process's thread, and the handle has to be
    // created in the host's process. So a waiting request isn't handed the
    // event here. It is completed empty, which tells the host that there is
    // something to collect, and the host's next request collects it on its
    // own thread.
    if (waitingIrp) {
        SAR_DEBUG("wake handle queue");
        SarCompleteWaitingIrp(waitingIrp, STATUS_SUCCESS);
    }

    return status;
}

NTSTATUS SarWaitHandleQueue(SarHandleQueue *queue, PIRP irp)
{
    NTSTATUS status = STATUS_SUCCESS;
    PIO_STACK_LOCATION irpStack = IoGetCurrentIrpStackLocation(irp);
    DWORD maxItems = irpStack->Parameters.DeviceIoControl.OutputBufferLength /
        sizeof(SarHandleQueueResponse);
    DWORD nextItem = 0;
    BOOLEAN cancelled = FALSE;
    LIST_ENTRY toComplete;
    KIRQL irql;

    InitializeListHead(&toComplete);
    irp->IoStatus.Information = 0;

    if (maxItems == 0) {
        // SarIrpDeviceControl completes the IRP for any non-pending status,
        // so it must not be completed here as well.
        irp->IoStatus.Information = sizeof(SarHandleQueueResponse);
        return STATUS_BUFFER_TOO_SMALL;
    }

    KeAcquireSpinLock(queue->lock, &irql);

    while (!IsListEmpty(&queue->pendingItems) && nextItem < maxItems) {
        PLIST_ENTRY entry = RemoveHeadList(&queue->pendingItems);

        InsertTailList(&toComplete, entry);
        nextItem++;
    }

    if (!IsListEmpty(&toComplete)) {
        // Transferred below, once the lock is released.
    } else if (queue->closed) {
        status = STATUS_CANCELLED;
    } else {
        irp->Tail.Overlay.DriverContext[0] = queue->lock;
        IoMarkIrpPending(irp);
        InsertTailList(&queue->pendingIrps, &irp->Tail.Overlay.ListEntry);
        IoSetCancelRoutine(irp, SarCancelHandleQueueIrp);

        // If the request was cancelled before the routine was set, nothing
        // is going to call it. Taking the routine back means it never ran
        // and never will, so the request is this function's to complete.
        if (irp->Cancel && IoSetCancelRoutine(irp, nullptr)) {
            RemoveEntryList(&irp->Tail.Overlay.ListEntry);
            cancelled = TRUE;
        }

        // From here on a waiting request may be completed by someone else
        // the moment the lock is released, and must not be touched.
        status = STATUS_PENDING;
    }

    KeReleaseSpinLock(queue->lock, irql);

    if (cancelled) {
        // Marked pending above, so pending is what the dispatch routine has
        // to return even though the request is finished.
        SarCompleteWaitingIrp(irp, STATUS_CANCELLED);
        return STATUS_PENDING;
    }

    nextItem = 0;

    while (!IsListEmpty(&toComplete)) {
        PLIST_ENTRY entry = RemoveHeadList(&toComplete);
        SarHandleQueueItem *queueItem =
            CONTAINING_RECORD(entry, SarHandleQueueItem, listEntry);

        // An event that can't be handed over only loses that one item.
        // Failing the request would drop the other endpoints' events in
        // this batch along with it. The next item reuses the response slot.
        NTSTATUS transferStatus = SarTransferQueuedEvent(
            irp, nextItem, queueItem->event, queueItem->associatedData);

        if (NT_SUCCESS(transferStatus)) {
            nextItem++;
            irp->IoStatus.Information += sizeof(SarHandleQueueResponse);
        } else {
            SAR_WARNING("Couldn't transfer a queued event: %08X",
                transferStatus);
        }

        ObDereferenceObject(queueItem->event);
        ExFreePoolWithTag(queueItem, SAR_TAG);
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
    return ExAllocatePool2(POOL_FLAG_NON_PAGED, byteSize, SAR_TAG);
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
    return ExAllocatePool2(POOL_FLAG_NON_PAGED, byteSize, SAR_TAG);
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
