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

#define SAR_INIT_GUID
#include "sar.h"

extern "C" DRIVER_INITIALIZE DriverEntry;

DRIVER_DISPATCH SarIrpDeviceControl;
DRIVER_UNLOAD SarUnload;

#define SAR_LOG(...) \
    DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_WARNING_LEVEL, __VA_ARGS__)

#define DEVICE_NT_NAME L"\\Device\\SynchronousAudioRouter"
#define DEVICE_WIN32_NAME L"\\DosDevices\\SynchronousAudioRouter"

static NTSTATUS SarKsDeviceAdd(IN PKSDEVICE device);

static KSDEVICE_DISPATCH gDeviceDispatch = {
    SarKsDeviceAdd, // Add
    nullptr, // Start
    nullptr, // PostStart
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

static KSDEVICE_DESCRIPTOR gDeviceDescriptor = {
    &gDeviceDispatch,
    0, nullptr,
    KSDEVICE_DESCRIPTOR_VERSION
};

static NTSTATUS SarKsDeviceAdd(IN PKSDEVICE device)
{
    NTSTATUS status;
    UNICODE_STRING symLinkName;

    status = IoRegisterDeviceInterface(device->PhysicalDeviceObject,
        &GUID_DEVINTERFACE_SYNCHRONOUSAUDIOROUTER, nullptr, &symLinkName);

    if (!NT_SUCCESS(status)) {
        SAR_LOG("Failed to create device interface.");
        return status;
    }

    SAR_LOG("KSDevice was created, dev interface: %wZ", &symLinkName);
    status = IoSetDeviceInterfaceState(&symLinkName, TRUE);

    if (!NT_SUCCESS(status)) {
        SAR_LOG("Failed to enable device interface.");
    }

    RtlFreeUnicodeString(&symLinkName);
    return status;
}

static BOOL checkIoctlInput(
    NTSTATUS *status, PIO_STACK_LOCATION irpStack, ULONG size)
{
    ULONG length = irpStack->Parameters.DeviceIoControl.InputBufferLength;

    if (length < size) {
        if (status) {
            *status = STATUS_BUFFER_TOO_SMALL;
        }

        return FALSE;
    }

    return TRUE;
}

// on driver entry: create ks device object
// on create endpoint: create ks filter & ks pin
//  - audio category
//    - for capture endpoints use KSCATEGORY_CAPTURE
//    - for playback endpoints use KSCATEGORY_RENDER
//  - filter has 1 pin
//  - pin has possibly multichannel data format (KSDATARANGE)
//  - pin supports a single KSDATARANGE
//  - pin flags: KSPIN_FLAG_DO_NOT_INITIATE_PROCESSING KSPIN_FLAG_PROCESS_IN_RUN_STATE_ONLY KSPIN_FLAG_FIXED_FORMAT KSPIN_FLAG_FRAMES_NOT_REQUIRED_FOR_PROCESSING

static NTSTATUS createEndpoint(SarCreateEndpointRequest *request)
{
    UNREFERENCED_PARAMETER(request);
    return STATUS_NOT_IMPLEMENTED;
}

NTSTATUS SarIrpDeviceControl(PDEVICE_OBJECT deviceObject, PIRP irp)
{
    UNREFERENCED_PARAMETER(deviceObject);
    PIO_STACK_LOCATION irpStack;
    ULONG ioControlCode;
    NTSTATUS ntStatus = STATUS_NOT_IMPLEMENTED;

    irpStack = IoGetCurrentIrpStackLocation(irp);
    ioControlCode = irpStack->Parameters.DeviceIoControl.IoControlCode;

    SAR_LOG("(SAR) DeviceIoControl IRP: %d", ioControlCode);

    switch (ioControlCode) {
        case SAR_REQUEST_CREATE_AUDIO_BUFFERS:
        case SAR_REQUEST_CREATE_ENDPOINT: {
            SAR_LOG("(SAR) Create endpoint");

            if (!checkIoctlInput(
                &ntStatus, irpStack, sizeof(SarCreateEndpointRequest))) {
                break;
            }

            SarCreateEndpointRequest *request =
                (SarCreateEndpointRequest *)irp->AssociatedIrp.SystemBuffer;

            SAR_LOG("(SAR) Create endpoint request: %d, %d, %d",
                request->type, request->index, request->channelCount);
            ntStatus = createEndpoint(request);
            break;
        }
        case SAR_REQUEST_MAP_AUDIO_BUFFER:
        case SAR_REQUEST_AUDIO_TICK:
        default:
            SAR_LOG("(SAR) Unknown ioctl %d");
            break;
    }

    irp->IoStatus.Status = ntStatus;
    IoCompleteRequest(irp, IO_NO_INCREMENT);
    return ntStatus;
}


VOID SarUnload(PDRIVER_OBJECT driverObject)
{
    UNREFERENCED_PARAMETER(driverObject);
    SAR_LOG("SAR is unloading");
}

extern "C" NTSTATUS DriverEntry(
    IN PDRIVER_OBJECT driverObject,
    IN PUNICODE_STRING registryPath)
{
    UNREFERENCED_PARAMETER(registryPath);

    NTSTATUS status = STATUS_SUCCESS;

    SAR_LOG("SAR is loading 2.");

    if (!NT_SUCCESS(status)) {
        return status;
    }

    KsInitializeDriver(driverObject, registryPath, &gDeviceDescriptor);
    driverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] = SarIrpDeviceControl;
    return STATUS_SUCCESS;
}
