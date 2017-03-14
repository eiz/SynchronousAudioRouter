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

NDIS_HANDLE gFilterDriverHandle;

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

typedef struct SarNdisFilterModuleContext
{
    NDIS_HANDLE filterHandle;
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
    UNREFERENCED_PARAMETER(filterModuleContext);
    UNREFERENCED_PARAMETER(filterRestartParameters);

    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NDIS_STATUS SarNdisFilterPause(
    _In_ NDIS_HANDLE filterModuleContext,
    _In_ PNDIS_FILTER_PAUSE_PARAMETERS filterPauseParameters)
{
    SAR_LOG("SarNdisFilterPause");
    UNREFERENCED_PARAMETER(filterModuleContext);
    UNREFERENCED_PARAMETER(filterPauseParameters);

    return STATUS_SUCCESS;
}

_Use_decl_annotations_
void SarNdisDriverUnload(PDRIVER_OBJECT driverObject)
{
    SAR_LOG("SarNdisDriverUnload");
    UNREFERENCED_PARAMETER(driverObject);

    if (gFilterDriverHandle) {
        NdisFDeregisterFilterDriver(gFilterDriverHandle);
    }
}

_Use_decl_annotations_
void SarNdisFilterSendNetBufferLists(
    _In_ NDIS_HANDLE filterModuleContext,
    _In_ PNET_BUFFER_LIST netBufferLists,
    _In_ NDIS_PORT_NUMBER portNumber,
    _In_ ULONG sendFlags)
{
    // TODO: this needs to respect the pause state of the filter.
    SAR_LOG("SarNdisFilterSendNetBufferLists");
    UNREFERENCED_PARAMETER(portNumber);
    SarNdisFilterModuleContext *context =
        (SarNdisFilterModuleContext *)filterModuleContext;

    NdisFSendNetBufferListsComplete(
        context->filterHandle, netBufferLists,
        (sendFlags & NDIS_SEND_FLAGS_SWITCH_SINGLE_SOURCE) ?
        NDIS_SEND_COMPLETE_FLAGS_SWITCH_SINGLE_SOURCE : 0);
}

extern "C" NTSTATUS DriverEntry(
    IN PDRIVER_OBJECT driverObject,
    IN PUNICODE_STRING registryPath)
{
    SAR_LOG("SarNdis DriverEntry");
    UNREFERENCED_PARAMETER(registryPath);
    NDIS_FILTER_DRIVER_CHARACTERISTICS fdc = {};

    fdc.Header.Type = NDIS_OBJECT_TYPE_FILTER_DRIVER_CHARACTERISTICS;
    fdc.Header.Revision = NDIS_FILTER_CHARACTERISTICS_REVISION_2;
    fdc.Header.Size = NDIS_SIZEOF_FILTER_DRIVER_CHARACTERISTICS_REVISION_2;
    fdc.MajorNdisVersion = 6;
    fdc.MinorNdisVersion = 30;
    fdc.MajorDriverVersion = 1;
    fdc.MinorDriverVersion = 0;
    RtlUnicodeStringInit(&fdc.FriendlyName, L"Synchronous Audio Router");
    RtlUnicodeStringInit(&fdc.UniqueName, SAR_NDIS_GUID_STR);
    RtlUnicodeStringInit(&fdc.ServiceName, L"SarNdis");
    fdc.AttachHandler = SarNdisFilterAttach;
    fdc.DetachHandler = SarNdisFilterDetach;
    fdc.PauseHandler = SarNdisFilterPause;
    fdc.RestartHandler = SarNdisFilterRestart;
    fdc.SendNetBufferListsHandler = SarNdisFilterSendNetBufferLists;

    driverObject->DriverUnload = SarNdisDriverUnload;

    return NdisFRegisterFilterDriver(
        driverObject, (NDIS_HANDLE)driverObject, &fdc, &gFilterDriverHandle);
}