// SynchronousAudioRouter
// Copyright (C) 2017 Mackenzie Straight
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

#define SAR_NDIS_GUID_STR L"{013656D5-F214-4EF1-BEA7-7151C06EF895}"
#define SAR_NDIS_FRIENDLY_NAME L"Synchronous Audio Router"
#define SAR_NDIS_SERVICE_NAME L"SarNdis"
#define SAR_NDIS_DEVICE_NAME L"\\Device\\SarNdis"
#define SAR_NDIS_SYMBOLIC_NAME L"\\DosDevices\\SarNdis"
#define SAR_NDIS_SDDL_STRING L"D:P(A;;GA;;;AU)"

NDIS_HANDLE gFilterDriverHandle;
NDIS_HANDLE gDeviceHandle;
PDEVICE_OBJECT gDeviceObject;

DRIVER_UNLOAD SarNdisDriverUnload;

FILTER_SET_OPTIONS SarNdisFilterSetOptions;
FILTER_SET_MODULE_OPTIONS SarNdisFilterSetModuleOptions;
FILTER_ATTACH SarNdisFilterAttach;
FILTER_DETACH SarNdisFilterDetach;
FILTER_RESTART SarNdisFilterRestart;
FILTER_PAUSE SarNdisFilterPause;
FILTER_SEND_NET_BUFFER_LISTS SarNdisFilterSendNetBufferLists;
FILTER_SEND_NET_BUFFER_LISTS_COMPLETE SarNdisFilterSendNetBufferListsComplete;
FILTER_CANCEL_SEND_NET_BUFFER_LISTS SarNdisFilterCancelSendNetBufferLists;
FILTER_RECEIVE_NET_BUFFER_LISTS SarNdisFilterReceiveNetBufferLists;
FILTER_RETURN_NET_BUFFER_LISTS SarNdisFilterReturnNetBufferLists;
FILTER_OID_REQUEST SarNdisFilterOidRequest;
FILTER_OID_REQUEST_COMPLETE SarNdisFilterOidRequestComplete;
FILTER_CANCEL_OID_REQUEST SarNdisFilterCancelOidRequest;
FILTER_DEVICE_PNP_EVENT_NOTIFY SarNdisFilterDevicePnPEventNotify;
FILTER_NET_PNP_EVENT SarNdisFilterNetPnPEvent;
FILTER_STATUS SarNdisFilterStatus;
FILTER_DIRECT_OID_REQUEST SarNdisFilterDirectOidRequest;
FILTER_DIRECT_OID_REQUEST_COMPLETE SarNdisFilterDirectOidRequestComplete;
FILTER_CANCEL_DIRECT_OID_REQUEST SarNdisFilterCancelDirectOidRequest;
DRIVER_DISPATCH SarNdisIrpCreate;
DRIVER_DISPATCH SarNdisIrpClose;
DRIVER_DISPATCH SarNdisIrpCleanup;
DRIVER_DISPATCH SarNdisIrpDeviceIoControl;

typedef struct SarNdisFilterModuleContext
{
    NDIS_HANDLE filterHandle;
    bool running;
    bool enabled;
} SarNdisFilterModuleContext;

_Use_decl_annotations_
NDIS_STATUS SarNdisFilterAttach(
    _In_ NDIS_HANDLE ndisFilterHandle,
    _In_ NDIS_HANDLE filterDriverContext,
    _In_ PNDIS_FILTER_ATTACH_PARAMETERS attachParameters)
{
    SAR_LOG("SarNdisFilterAttach");
    UNREFERENCED_PARAMETER(filterDriverContext);
    UNREFERENCED_PARAMETER(attachParameters);
    NTSTATUS status = STATUS_SUCCESS;
    NDIS_FILTER_ATTRIBUTES filterAttributes;
    SarNdisFilterModuleContext *filterModuleContext =
        (SarNdisFilterModuleContext *)ExAllocatePoolWithTag(
            NonPagedPool, sizeof(SarNdisFilterModuleContext), SAR_TAG);

    if (!filterModuleContext) {
        status = STATUS_INSUFFICIENT_RESOURCES;
        goto err_out;
    }

    RtlZeroMemory(filterModuleContext, sizeof(SarNdisFilterModuleContext));
    filterModuleContext->filterHandle = ndisFilterHandle;
    filterAttributes.Header.Type = NDIS_OBJECT_TYPE_FILTER_ATTRIBUTES;
    filterAttributes.Header.Revision = NDIS_FILTER_ATTRIBUTES_REVISION_1;
    filterAttributes.Header.Size = NDIS_SIZEOF_FILTER_ATTRIBUTES_REVISION_1;
    filterAttributes.Flags = 0;

    status = NdisFSetAttributes(
        ndisFilterHandle, filterModuleContext, &filterAttributes);

    if (!NT_SUCCESS(status)) {
        goto err_out;
    }

    return STATUS_SUCCESS;

err_out:
    if (filterModuleContext != NULL) {
        ExFreePoolWithTag(filterModuleContext, SAR_TAG);
    }

    return status;
}

_Use_decl_annotations_
void SarNdisFilterDetach(_In_ NDIS_HANDLE filterModuleContext)
{
    SAR_LOG("SarNdisFilterDetach");
    UNREFERENCED_PARAMETER(filterModuleContext);
}

_Use_decl_annotations_
NDIS_STATUS SarNdisFilterRestart(
    _In_ NDIS_HANDLE filterModuleContext,
    _In_ PNDIS_FILTER_RESTART_PARAMETERS filterRestartParameters)
{
    SAR_LOG("SarNdisFilterRestart");
    UNREFERENCED_PARAMETER(filterRestartParameters);
    SarNdisFilterModuleContext *context =
        (SarNdisFilterModuleContext *)filterModuleContext;

    context->running = true;
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NDIS_STATUS SarNdisFilterPause(
    _In_ NDIS_HANDLE filterModuleContext,
    _In_ PNDIS_FILTER_PAUSE_PARAMETERS filterPauseParameters)
{
    SAR_LOG("SarNdisFilterPause");
    UNREFERENCED_PARAMETER(filterPauseParameters);
    SarNdisFilterModuleContext *context =
        (SarNdisFilterModuleContext *)filterModuleContext;

    context->running = false;
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
void SarNdisFilterSendNetBufferLists(
    _In_ NDIS_HANDLE filterModuleContext,
    _In_ PNET_BUFFER_LIST netBufferLists,
    _In_ NDIS_PORT_NUMBER portNumber,
    _In_ ULONG sendFlags)
{
    SAR_LOG("SarNdisFilterSendNetBufferLists");
    UNREFERENCED_PARAMETER(portNumber);
    SarNdisFilterModuleContext *context =
        (SarNdisFilterModuleContext *)filterModuleContext;

    if (context->running && context->enabled) {
        NdisFSendNetBufferListsComplete(
            context->filterHandle, netBufferLists,
            (sendFlags & NDIS_SEND_FLAGS_SWITCH_SINGLE_SOURCE) ?
            NDIS_SEND_COMPLETE_FLAGS_SWITCH_SINGLE_SOURCE : 0);
    } else {
        NdisFSendNetBufferLists(
            context->filterHandle, netBufferLists, portNumber, sendFlags);
    }
}

_Use_decl_annotations_
NTSTATUS SarNdisIrpCreate(
    _In_ PDEVICE_OBJECT deviceObject,
    _In_ PIRP irp)
{
    SAR_LOG("SarNdisIrpCreate");
    UNREFERENCED_PARAMETER(deviceObject);

    irp->IoStatus.Status = STATUS_SUCCESS;
    IoCompleteRequest(irp, IO_NO_INCREMENT);
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS SarNdisIrpClose(
    _In_ PDEVICE_OBJECT deviceObject,
    _In_ PIRP irp)
{
    SAR_LOG("SarNdisIrpClose");
    UNREFERENCED_PARAMETER(deviceObject);

    irp->IoStatus.Status = STATUS_SUCCESS;
    IoCompleteRequest(irp, IO_NO_INCREMENT);
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS SarNdisIrpCleanup(
    _In_ PDEVICE_OBJECT deviceObject,
    _In_ PIRP irp)
{
    SAR_LOG("SarNdisIrpCleanup");
    UNREFERENCED_PARAMETER(deviceObject);

    irp->IoStatus.Status = STATUS_SUCCESS;
    IoCompleteRequest(irp, IO_NO_INCREMENT);
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS SarNdisIrpDeviceIoControl(
    _In_ PDEVICE_OBJECT deviceObject,
    _In_ PIRP irp)
{
    SAR_LOG("SarNdisIrpDeviceIoControl");
    UNREFERENCED_PARAMETER(deviceObject);

    irp->IoStatus.Status = STATUS_UNSUCCESSFUL;
    IoCompleteRequest(irp, IO_NO_INCREMENT);
    return STATUS_UNSUCCESSFUL;
}

_Use_decl_annotations_
void SarNdisDriverUnload(PDRIVER_OBJECT driverObject)
{
    SAR_LOG("SarNdisDriverUnload");
    UNREFERENCED_PARAMETER(driverObject);

    if (gDeviceHandle) {
        NdisDeregisterDeviceEx(gDeviceHandle);
        gDeviceHandle = NULL;
        gDeviceObject = NULL;
    }

    if (gFilterDriverHandle) {
        NdisFDeregisterFilterDriver(gFilterDriverHandle);
        gFilterDriverHandle = NULL;
    }
}

extern "C" NTSTATUS DriverEntry(
    IN PDRIVER_OBJECT driverObject,
    IN PUNICODE_STRING registryPath)
{
    SAR_LOG("SarNdis DriverEntry");
    UNREFERENCED_PARAMETER(registryPath);
    NDIS_FILTER_DRIVER_CHARACTERISTICS fdc = {};
    NDIS_DEVICE_OBJECT_ATTRIBUTES doa = {};
    UNICODE_STRING deviceName = {}, deviceLink = {}, sddlString = {};
    DRIVER_DISPATCH *dispatch[IRP_MJ_MAXIMUM_FUNCTION + 1] = {};

    NTSTATUS status = STATUS_SUCCESS;

    fdc.Header.Type = NDIS_OBJECT_TYPE_FILTER_DRIVER_CHARACTERISTICS;
    fdc.Header.Revision = NDIS_FILTER_CHARACTERISTICS_REVISION_2;
    fdc.Header.Size = NDIS_SIZEOF_FILTER_DRIVER_CHARACTERISTICS_REVISION_2;
    fdc.MajorNdisVersion = 6;
    fdc.MinorNdisVersion = 30;
    fdc.MajorDriverVersion = 1;
    fdc.MinorDriverVersion = 0;
    RtlUnicodeStringInit(&fdc.FriendlyName, SAR_NDIS_FRIENDLY_NAME);
    RtlUnicodeStringInit(&fdc.UniqueName, SAR_NDIS_GUID_STR);
    RtlUnicodeStringInit(&fdc.ServiceName, SAR_NDIS_SERVICE_NAME);
    fdc.AttachHandler = SarNdisFilterAttach;
    fdc.DetachHandler = SarNdisFilterDetach;
    fdc.PauseHandler = SarNdisFilterPause;
    fdc.RestartHandler = SarNdisFilterRestart;
    fdc.SendNetBufferListsHandler = SarNdisFilterSendNetBufferLists;

    driverObject->DriverUnload = SarNdisDriverUnload;

    status = NdisFRegisterFilterDriver(
        driverObject, (NDIS_HANDLE)driverObject, &fdc, &gFilterDriverHandle);

    if (!NT_SUCCESS(status)) {
        return status;
    }

    doa.Header.Type = NDIS_OBJECT_TYPE_DEVICE_OBJECT_ATTRIBUTES;
    doa.Header.Revision = NDIS_DEVICE_OBJECT_ATTRIBUTES_REVISION_1;
    doa.Header.Size = NDIS_SIZEOF_DEVICE_OBJECT_ATTRIBUTES_REVISION_1;
    RtlUnicodeStringInit(&deviceName, SAR_NDIS_DEVICE_NAME);
    doa.DeviceName = &deviceName;
    RtlUnicodeStringInit(&deviceLink, SAR_NDIS_SYMBOLIC_NAME);
    doa.SymbolicName = &deviceLink;
    dispatch[IRP_MJ_CREATE] = SarNdisIrpCreate;
    dispatch[IRP_MJ_CLOSE] = SarNdisIrpClose;
    dispatch[IRP_MJ_CLEANUP] = SarNdisIrpCleanup;
    dispatch[IRP_MJ_DEVICE_CONTROL] = SarNdisIrpDeviceIoControl;
    doa.MajorFunctions = dispatch;

    // Grants access to the device for all users. TODO: configure this as a
    // parameter from the installer.
    RtlUnicodeStringInit(&sddlString, SAR_NDIS_SDDL_STRING);
    doa.DefaultSDDLString = &sddlString;

    status = NdisRegisterDeviceEx(
        gFilterDriverHandle, &doa, &gDeviceObject, &gDeviceHandle);

    if (!NT_SUCCESS(status)) {
        NdisFDeregisterFilterDriver(gFilterDriverHandle);
        return status;
    }

    return STATUS_SUCCESS;
}