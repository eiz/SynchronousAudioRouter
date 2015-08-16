#include "sar.hpp"

extern "C" DRIVER_INITIALIZE DriverEntry;

DRIVER_DISPATCH SarIrpUnsupported;
DRIVER_DISPATCH SarIrpCreate;
DRIVER_DISPATCH SarIrpDeviceControl;
DRIVER_DISPATCH SarIrpClose;

DRIVER_UNLOAD SarUnload;

#define DEVICE_NT_NAME L"\\Device\\SynchronousAudioRouter"
#define DEVICE_WIN32_NAME L"\\DosDevices\\SynchronousAudioRouter"

NTSTATUS SarIrpUnsupported(PDEVICE_OBJECT deviceObject, PIRP irp)
{
    UNREFERENCED_PARAMETER(deviceObject);
    PIO_STACK_LOCATION irpStack;

    irpStack = IoGetCurrentIrpStackLocation(irp);
    DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_WARNING_LEVEL,
        "(SAR) IRP %d not implemented", irpStack->MajorFunction);
    irp->IoStatus.Status = STATUS_NOT_IMPLEMENTED;
    IoCompleteRequest(irp, IO_NO_INCREMENT);
    return STATUS_NOT_IMPLEMENTED;
}

NTSTATUS SarIrpCreate(PDEVICE_OBJECT deviceObject, PIRP irp)
{
    UNREFERENCED_PARAMETER(deviceObject);

    DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_WARNING_LEVEL,
        "(SAR) Device opened");
    irp->IoStatus.Status = STATUS_SUCCESS;
    IoCompleteRequest(irp, IO_NO_INCREMENT);
    return STATUS_SUCCESS;
}

NTSTATUS SarIrpDeviceControl(PDEVICE_OBJECT deviceObject, PIRP irp)
{
    UNREFERENCED_PARAMETER(deviceObject);

    DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_WARNING_LEVEL,
        "(SAR) DeviceIoControl IRP not implemented");
    irp->IoStatus.Status = STATUS_NOT_IMPLEMENTED;
    IoCompleteRequest(irp, IO_NO_INCREMENT);
    return STATUS_NOT_IMPLEMENTED;
}

NTSTATUS SarIrpClose(PDEVICE_OBJECT deviceObject, PIRP irp)
{
    UNREFERENCED_PARAMETER(deviceObject);

    DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_WARNING_LEVEL,
        "(SAR) Device closed");
    irp->IoStatus.Status = STATUS_SUCCESS;
    IoCompleteRequest(irp, IO_NO_INCREMENT);
    return STATUS_SUCCESS;
}

VOID SarUnload(PDRIVER_OBJECT driverObject)
{
    UNICODE_STRING win32DeviceName;

    DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_WARNING_LEVEL,
        "SAR is unloading");
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

    DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_WARNING_LEVEL, "Hello, Kernel 3!");
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
