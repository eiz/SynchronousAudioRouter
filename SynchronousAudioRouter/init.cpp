#include "sar.h"

extern "C" DRIVER_INITIALIZE DriverEntry;

DRIVER_DISPATCH SarIrpUnsupported;
DRIVER_DISPATCH SarIrpCreate;
DRIVER_DISPATCH SarIrpDeviceControl;
DRIVER_DISPATCH SarIrpClose;

DRIVER_UNLOAD SarUnload;

#define SAR_LOG(...) \
    DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_WARNING_LEVEL, __VA_ARGS__)

#define DEVICE_NT_NAME L"\\Device\\SynchronousAudioRouter"
#define DEVICE_WIN32_NAME L"\\DosDevices\\SynchronousAudioRouter"

NTSTATUS SarIrpUnsupported(PDEVICE_OBJECT deviceObject, PIRP irp)
{
    UNREFERENCED_PARAMETER(deviceObject);
    PIO_STACK_LOCATION irpStack;

    irpStack = IoGetCurrentIrpStackLocation(irp);
    SAR_LOG("(SAR) IRP %d not implemented", irpStack->MajorFunction);
    irp->IoStatus.Status = STATUS_NOT_IMPLEMENTED;
    IoCompleteRequest(irp, IO_NO_INCREMENT);
    return STATUS_NOT_IMPLEMENTED;
}

NTSTATUS SarIrpCreate(PDEVICE_OBJECT deviceObject, PIRP irp)
{
    UNREFERENCED_PARAMETER(deviceObject);

    SAR_LOG("(SAR) Device opened");
    irp->IoStatus.Status = STATUS_SUCCESS;
    IoCompleteRequest(irp, IO_NO_INCREMENT);
    return STATUS_SUCCESS;
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

NTSTATUS SarIrpClose(PDEVICE_OBJECT deviceObject, PIRP irp)
{
    UNREFERENCED_PARAMETER(deviceObject);

    SAR_LOG("(SAR) Device closed");
    irp->IoStatus.Status = STATUS_SUCCESS;
    IoCompleteRequest(irp, IO_NO_INCREMENT);
    return STATUS_SUCCESS;
}

VOID SarUnload(PDRIVER_OBJECT driverObject)
{
    UNICODE_STRING win32DeviceName;

    SAR_LOG("SAR is unloading");
    RtlInitUnicodeString(&win32DeviceName, DEVICE_WIN32_NAME);
    IoDeleteSymbolicLink(&win32DeviceName);
    IoDeleteDevice(driverObject->DeviceObject);
}

extern "C" NTSTATUS DriverEntry(
    IN PDRIVER_OBJECT driverObject,
    IN PUNICODE_STRING registryPath)
{
    UNREFERENCED_PARAMETER(registryPath);

    NTSTATUS status = STATUS_SUCCESS;
    UNICODE_STRING ntDeviceName, win32DeviceName;
    PDEVICE_OBJECT deviceObject;

    SAR_LOG("Hello, Kernel 3!");
    RtlInitUnicodeString(&ntDeviceName, DEVICE_NT_NAME);
    RtlInitUnicodeString(&win32DeviceName, DEVICE_WIN32_NAME);
    status = IoCreateDevice(
        driverObject,
        0,
        &ntDeviceName,
        FILE_DEVICE_SOUND,
        FILE_DEVICE_SECURE_OPEN,
        FALSE,
        &deviceObject);

    if (!NT_SUCCESS(status)) {
        return status;
    }

    status = IoCreateSymbolicLink(&win32DeviceName, &ntDeviceName);

    if (!NT_SUCCESS(status)) {
        IoDeleteDevice(deviceObject);
        return status;
    }

    for (int i = 0; i < IRP_MJ_MAXIMUM_FUNCTION; ++i) {
        driverObject->MajorFunction[i] = SarIrpUnsupported;
    }

    driverObject->MajorFunction[IRP_MJ_CREATE] = SarIrpCreate;
    driverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] = SarIrpDeviceControl;
    driverObject->MajorFunction[IRP_MJ_CLOSE] = SarIrpClose;
    driverObject->DriverUnload = SarUnload;
    deviceObject->Flags |= DO_BUFFERED_IO;
    deviceObject->Flags &= ~DO_DEVICE_INITIALIZING;
    return STATUS_SUCCESS;
}
