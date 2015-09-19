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

#include "stdafx.h"
#include "sarclient.h"

namespace Sar {

SarClient::SarClient(
    const DriverConfig& driverConfig,
    const BufferConfig& bufferConfig)
    : _driverConfig(driverConfig), _bufferConfig(bufferConfig),
      _device(INVALID_HANDLE_VALUE)
{
}

void SarClient::tick(long bufferIndex)
{
    // for each endpoint
    // read isActive, generation and buffer offset/size
    //   if offset/size invalid, skip endpoint (fill asio buffers with 0)
    // if playback device:
    //   consume frameSampleCount * channelCount samples, demux to asio frames
    // if recording device:
    //   mux from asio frames
    // read isActive, generation
    //   if conflicted, skip endpoint and fill asio frames with 0
    // else increment position register
}

bool SarClient::start()
{
    if (!openControlDevice()) {
        return false;
    }

    if (!setBufferLayout()) {
        stop();
        return false;
    }

    if (!createEndpoints()) {
        stop();
        return false;
    }

    return true;
}

void SarClient::stop()
{
    if (_device != INVALID_HANDLE_VALUE) {
        CloseHandle(_device);
        _device = INVALID_HANDLE_VALUE;
    }
}

bool SarClient::openControlDevice()
{
    HDEVINFO devinfo;
    SP_DEVICE_INTERFACE_DATA interfaceData;
    PSP_DEVICE_INTERFACE_DETAIL_DATA interfaceDetail;
    DWORD requiredSize;

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

        return false;
    }

    _device = CreateFile(interfaceDetail->DevicePath,
        GENERIC_ALL, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);

    if (_device == INVALID_HANDLE_VALUE) {
        return false;
    }

    return true;
}

bool SarClient::setBufferLayout()
{
    SarSetBufferLayoutRequest request = {};
    SarSetBufferLayoutResponse response = {};

    request.bufferSize = 1024 * 1024; // TODO: size based on endpoint config
    request.frameSize =
        _bufferConfig.frameSampleCount * _bufferConfig.sampleSize;
    request.sampleRate = _bufferConfig.sampleRate;
    request.sampleSize = _bufferConfig.sampleSize;

    if (!DeviceIoControl(_device, SAR_REQUEST_SET_BUFFER_LAYOUT,
        (LPVOID)&request, sizeof(request), (LPVOID)&response, sizeof(response),
        nullptr, nullptr)) {

        OutputDebugStringA("Nope =(");
        return false;
    }

    _sharedBuffer = response.virtualAddress;
    _bufferSize = response.actualSize;
    _registers = (SarEndpointRegisters *)
        ((char *)response.virtualAddress + response.registerBase);

    return true;
}

bool SarClient::createEndpoints()
{
    return true;
}

} // namespace Sar