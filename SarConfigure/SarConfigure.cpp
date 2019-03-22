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

#include <initguid.h>
#include <windows.h>
#include "tinyasio.h"
#include <SetupAPI.h>
#include <sar.h>

#define GUID_FORMAT "{%08lx-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x}"
#define GUID_VALUES(g) (g).Data1, (g).Data2, (g).Data3, \
    (g).Data4[0], \
    (g).Data4[1], \
    (g).Data4[2], \
    (g).Data4[3], \
    (g).Data4[4], \
    (g).Data4[5], \
    (g).Data4[6], \
    (g).Data4[7]

static int openConfigurationDialog(Sar::AsioDriver& driver) {
    HRESULT result;
    Sar::IASIO* driverInterface;

    result = driver.open(&driverInterface);
    if (!SUCCEEDED(result)) {
        printf("CoCreateInstance failed on SAR CLSID %s: 0x%x\n", driver.name.c_str(), result);
        return result;
    }

    printf("Initializing ASIO driver\n");
    driverInterface->init(NULL);

    printf("Opening configuration dialog\n");
    Sar::AsioStatus status = driverInterface->controlPanel();

    driverInterface->Release();

    if (status == Sar::AsioStatus::OK) {
        printf("Dialog closed Ok\n");
        return 0;
    }
    else {
        printf("Dialog error %d\n", (int)status);
        return (int)(status);
    }
}

static bool broadcastPinFormatChange() {
    HDEVINFO devinfo;
    SP_DEVICE_INTERFACE_DATA interfaceData;
    PSP_DEVICE_INTERFACE_DETAIL_DATA interfaceDetail;
    DWORD requiredSize;
    HANDLE _device;
    DWORD dummy;

    interfaceData.cbSize = sizeof(SP_DEVICE_INTERFACE_DATA);
    devinfo = SetupDiGetClassDevs(
        &GUID_DEVINTERFACE_SYNCHRONOUSAUDIOROUTER, nullptr, nullptr,
        DIGCF_DEVICEINTERFACE | DIGCF_PRESENT);

    if (devinfo == INVALID_HANDLE_VALUE) {
        return false;
    }

    if (!SetupDiEnumDeviceInterfaces(devinfo, NULL,
        &GUID_DEVINTERFACE_SYNCHRONOUSAUDIOROUTER, 0, &interfaceData)) {

        return false;
    }

    SetLastError(0);
    SetupDiGetDeviceInterfaceDetail(
        devinfo, &interfaceData, nullptr, 0, &requiredSize, nullptr);

    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
        return false;
    }

    interfaceDetail = (PSP_DEVICE_INTERFACE_DETAIL_DATA)malloc(requiredSize);
    interfaceDetail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA);

    if (!SetupDiGetDeviceInterfaceDetail(
        devinfo, &interfaceData, interfaceDetail,
        requiredSize, nullptr, nullptr)) {

        free(interfaceDetail);
        return false;
    }

    _device = CreateFile(interfaceDetail->DevicePath,
        GENERIC_ALL, 0, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED, nullptr);

    if (_device == INVALID_HANDLE_VALUE) {
        free(interfaceDetail);
        return false;
    }

    free(interfaceDetail);

    DeviceIoControl(_device, SAR_SEND_FORMAT_CHANGE_EVENT,
        nullptr, 0, nullptr, 0, &dummy, nullptr);
    printf("Sent SAR_SEND_FORMAT_CHANGE_EVENT\n");

    CloseHandle(_device);

    return true;
}

int main(int argc, char *argv[])
{

    if (argc >= 2 && !strcmp(argv[1], "-u")) {
        broadcastPinFormatChange();
    }
    else {
        CoInitialize(NULL);

        std::vector<Sar::AsioDriver> asioDrivers = Sar::InstalledAsioDrivers();

        printf("\n");
        for (size_t i = 0; i < asioDrivers.size(); i++) {
            printf("Checking driver: %s == %s\n", asioDrivers[i].clsid.c_str(), CLSID_STR_SynchronousAudioRouter);
            if (asioDrivers[i].clsid == CLSID_STR_SynchronousAudioRouter) {
                Sar::AsioDriver& driver = asioDrivers[i];
                printf("Found SAR ASIO driver: %s\n", driver.name.c_str());
                return openConfigurationDialog(driver);
            }
        }

        CoUninitialize();
    }
}
