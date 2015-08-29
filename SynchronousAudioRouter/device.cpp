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

NTSTATUS SarKsDeviceAdd(IN PKSDEVICE device)
{
    NTSTATUS status;
    UNICODE_STRING referenceString;
    SarDriverExtension *extension =
        (SarDriverExtension *)IoGetDriverObjectExtension(
            device->FunctionalDeviceObject->DriverObject, DriverEntry);

    RtlUnicodeStringInit(&referenceString, SAR_CONTROL_REFERENCE_STRING + 1);
    status = IoRegisterDeviceInterface(device->PhysicalDeviceObject,
        &GUID_DEVINTERFACE_SYNCHRONOUSAUDIOROUTER, &referenceString,
        &extension->sarInterfaceName);

    if (!NT_SUCCESS(status)) {
        return status;
    }

    SAR_LOG("KSDevice was created for %p, dev interface: %wZ",
        device, &extension->sarInterfaceName);
    return status;
}

NTSTATUS SarKsDevicePostStart(IN PKSDEVICE device)
{
    SarDriverExtension *extension =
        (SarDriverExtension *)IoGetDriverObjectExtension(
            device->FunctionalDeviceObject->DriverObject, DriverEntry);

    return IoSetDeviceInterfaceState(&extension->sarInterfaceName, TRUE);
}
