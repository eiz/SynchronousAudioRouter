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

#include <iostream>
#include <windows.h>
#include <SetupAPI.h>

#include <initguid.h>
#include <sar.h>

BOOL createEndpoint(
    HANDLE device, DWORD endpointType, DWORD index, DWORD channelCount,
    LPWSTR name)
{
    SarCreateEndpointRequest request = {};

    request.type = endpointType;
    request.index = index;
    request.channelCount = channelCount;
    wcscpy_s(request.name, name);

    if (!DeviceIoControl(device, SAR_REQUEST_CREATE_ENDPOINT,
            (LPVOID)&request, sizeof(request), nullptr, 0, nullptr, nullptr)) {
        return FALSE;
    }

    return TRUE;
}

BOOL setBufferLayout(
    HANDLE device, DWORD bufferSize,
    DWORD sampleRate, DWORD sampleDepth,
    SarEndpointRegisters **regsOut,
    LPVOID *bufferOut,
    DWORD *bufferSizeOut)
{
    SarSetBufferLayoutRequest request = {};
    SarSetBufferLayoutResponse response = {};

    request.bufferSize = bufferSize;
    request.sampleRate = sampleRate;
    request.sampleDepth = sampleDepth;

    if (!DeviceIoControl(device, SAR_REQUEST_SET_BUFFER_LAYOUT,
        (LPVOID)&request, sizeof(request), (LPVOID)&response, sizeof(response),
        nullptr, nullptr)) {

        return FALSE;
    }

    if (regsOut) {
        *regsOut = (SarEndpointRegisters *)
            ((PCH)response.virtualAddress + response.registerBase);
    }

    if (bufferOut) {
        *bufferOut = response.virtualAddress;
    }

    if (bufferSizeOut) {
        *bufferSizeOut = response.actualSize;
    }

    return TRUE;
}

BOOL audioTick(HANDLE device)
{
    return DeviceIoControl(device, SAR_REQUEST_AUDIO_TICK,
        nullptr, 0, nullptr, 0, nullptr, nullptr);
}

int main(int argc, char *argv[])
{
    HDEVINFO devinfo;
	HANDLE device;
    SP_DEVICE_INTERFACE_DATA interfaceData;
    PSP_DEVICE_INTERFACE_DETAIL_DATA interfaceDetail;
    DWORD requiredSize;

    interfaceData.cbSize = sizeof(SP_DEVICE_INTERFACE_DATA);
    devinfo = SetupDiGetClassDevs(
        &GUID_DEVINTERFACE_SYNCHRONOUSAUDIOROUTER, nullptr, nullptr,
        DIGCF_DEVICEINTERFACE | DIGCF_PRESENT);

    if (devinfo == INVALID_HANDLE_VALUE) {
        std::cerr << "Couldn't enumerate SAR devices: "
            << GetLastError() << std::endl;
        return 1;
    }

    if (!SetupDiEnumDeviceInterfaces(devinfo, NULL,
        &GUID_DEVINTERFACE_SYNCHRONOUSAUDIOROUTER, 0, &interfaceData)) {
        std::cerr << "Couldn't get interface data: "
            << GetLastError() << std::endl;
        return 1;
    }

    SetLastError(0);
    SetupDiGetDeviceInterfaceDetail(
        devinfo, &interfaceData, nullptr, 0, &requiredSize, nullptr);

    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
        std::cerr << "Weird SetupDiGetDeviceInterfaceDetail failure: "
            << GetLastError() << std::endl;
    }

    interfaceDetail = (PSP_DEVICE_INTERFACE_DETAIL_DATA)malloc(requiredSize);
    interfaceDetail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA);

    if (!SetupDiGetDeviceInterfaceDetail(
        devinfo, &interfaceData, interfaceDetail,
        requiredSize, nullptr, nullptr)) {

        std::cerr << "Couldn't get device interface details: "
            << GetLastError() << std::endl;
        return 1;
    }

    device = CreateFile(interfaceDetail->DevicePath,
        GENERIC_ALL, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);

    if (device == INVALID_HANDLE_VALUE) {
        std::cerr << "Couldn't open SAR device: "
            << GetLastError() << std::endl;
        return 1;
    }

    std::cout << "Opened SAR device." << std::endl;

    SarEndpointRegisters *regs = nullptr;

    if (!setBufferLayout(device, 1024 * 1024, 96000, 3, &regs, nullptr, nullptr)) {
        std::cerr << "Couldn't set buffer layout: "
            << GetLastError() << std::endl;
        return 1;
    }

    if (!createEndpoint(
        device, SAR_ENDPOINT_TYPE_PLAYBACK, 0, 2, L"Music Out (Stereo)")) {
        std::cerr << "Couldn't create endpoint: "
            << GetLastError() << std::endl;
        return 1;
    }

    while (true) {
        Sleep(1000);
        std::cout << "Active: " << regs->isActive << " Offset: "
                  << regs->bufferOffset << " Size: " << regs->bufferSize
                  << " Clock: " << regs->clockRegister << " Position: "
                  << regs->positionRegister << std::endl;
//        audioTick(device);
    }

    CloseHandle(device);
    return 0;
}
