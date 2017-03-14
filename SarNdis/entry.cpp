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

_Use_decl_annotations_
NDIS_STATUS SarNdisFilterAttach(
    _In_ NDIS_HANDLE ndisFilterHandle,
    _In_ NDIS_HANDLE filterDriverContext,
    _In_ PNDIS_FILTER_ATTACH_PARAMETERS attachParameters)
{
    UNREFERENCED_PARAMETER(ndisFilterHandle);
    UNREFERENCED_PARAMETER(filterDriverContext);
    UNREFERENCED_PARAMETER(attachParameters);

    SAR_LOG("SarNdisFilterAttach");
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
void SarNdisFilterDetach(_In_ NDIS_HANDLE filterModuleContext)
{
    UNREFERENCED_PARAMETER(filterModuleContext);
    SAR_LOG("SarNdisFilterDetach");
}

_Use_decl_annotations_
NDIS_STATUS SarNdisFilterRestart(
    _In_ NDIS_HANDLE filterModuleContext,
    _In_ PNDIS_FILTER_RESTART_PARAMETERS filterRestartParameters)
{
    UNREFERENCED_PARAMETER(filterModuleContext);
    UNREFERENCED_PARAMETER(filterRestartParameters);

    SAR_LOG("SarNdisFilterRestart");
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NDIS_STATUS SarNdisFilterPause(
    _In_ NDIS_HANDLE filterModuleContext,
    _In_ PNDIS_FILTER_PAUSE_PARAMETERS filterPauseParameters)
{
    UNREFERENCED_PARAMETER(filterModuleContext);
    UNREFERENCED_PARAMETER(filterPauseParameters);

    SAR_LOG("SarNdisFilterPause");
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
void SarNdisDriverUnload(PDRIVER_OBJECT driverObject)
{
    UNREFERENCED_PARAMETER(driverObject);

    if (gFilterDriverHandle) {
        NdisFDeregisterFilterDriver(gFilterDriverHandle);
    }
}

extern "C" NTSTATUS DriverEntry(
    IN PDRIVER_OBJECT driverObject,
    IN PUNICODE_STRING registryPath)
{
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

    driverObject->DriverUnload = SarNdisDriverUnload;

    return NdisFRegisterFilterDriver(
        driverObject, (NDIS_HANDLE)driverObject, &fdc, &gFilterDriverHandle);
}