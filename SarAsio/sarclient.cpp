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
#include "utility.h"

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
    ATLASSERT(bufferIndex == 0 || bufferIndex == 1);

    // for each endpoint
    // read isActive, generation and buffer offset/size/position
    //   if offset/size invalid, skip endpoint (fill asio buffers with 0)
    // if playback device:
    //   consume frameSampleCount * channelCount samples, demux to asio frames
    // if recording device:
    //   mux from asio frames
    // read isActive, generation
    //   if conflicted, skip endpoint and fill asio frames with 0
    // else increment position register
    for (int i = 0; i < _driverConfig.endpoints.size(); ++i) {
        auto& endpoint = _driverConfig.endpoints[i];
        auto& asioBuffers = _bufferConfig.asioBuffers[i];
        auto asioBufferSize =
            _bufferConfig.frameSampleCount * _bufferConfig.sampleSize;
        auto isActive = _registers[i].isActive;
        auto generation = _registers[i].generation;
        auto endpointBufferOffset = _registers[i].bufferOffset;
        auto endpointBufferSize = _registers[i].bufferSize;
        auto positionRegister = _registers[i].positionRegister;
        auto position = positionRegister + endpointBufferOffset;
        void *endpointData = ((char *)_sharedBuffer) + position;
        auto ntargets = (int)(asioBuffers.size() / 2);
        void **targetBuffers = (void **)alloca(sizeof(void *) * ntargets);

        if (!isActive) {
            continue;
        }

        if (endpointBufferOffset + endpointBufferSize > _sharedBufferSize ||
            position + asioBufferSize > _sharedBufferSize ||
            positionRegister + asioBufferSize > endpointBufferSize) {
            continue;
        }

        for (int bi = bufferIndex, ti = 0; bi < asioBuffers.size(); bi += 2) {
            targetBuffers[ti++] = asioBuffers[bi];
        }

        if (endpoint.type == EndpointType::Playback) {
            demux(
                endpointData, targetBuffers, ntargets,
                asioBufferSize, _bufferConfig.sampleSize);
        } else {
            mux(
                endpointData, targetBuffers, ntargets,
                asioBufferSize, _bufferConfig.sampleSize);
        }

        auto lateIsActive = _registers[i].isActive;
        auto lateGeneration = _registers[i].generation;

        if (!lateIsActive || generation != lateGeneration) {
            for (int ti = 0; ti < ntargets; ++ti) {
                ZeroMemory(targetBuffers[ti], asioBufferSize);
            }
        } else {
            // TODO: this can still race if the endpoint is stopped and started
            // while we're updating the position register. Maybe use DCAS on the
            // generation+position?
            _registers[i].positionRegister =
                (positionRegister + asioBufferSize) % endpointBufferSize;
        }
    }
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

        return false;
    }

    _sharedBuffer = response.virtualAddress;
    _sharedBufferSize = response.actualSize;
    _registers = (SarEndpointRegisters *)
        ((char *)response.virtualAddress + response.registerBase);
    return true;
}

bool SarClient::createEndpoints()
{
    int i = 0;

    for (auto& endpoint : _driverConfig.endpoints) {
        SarCreateEndpointRequest request = {};

        request.type = endpoint.type == EndpointType::Playback ?
            SAR_ENDPOINT_TYPE_PLAYBACK : SAR_ENDPOINT_TYPE_RECORDING;
        request.channelCount = endpoint.channelCount;
        request.index = i++;
        wcscpy_s(request.name, UTF8ToWide(endpoint.description).c_str());

        if (!DeviceIoControl(_device, SAR_REQUEST_CREATE_ENDPOINT,
            (LPVOID)&request, sizeof(request), nullptr, 0, nullptr, nullptr)) {

            std::ostringstream os;

            os << "Endpoint creation for " << endpoint.description
               << " failed.";
            OutputDebugStringA(os.str().c_str());
            return false;
        }
    }

    return true;
}

void SarClient::demux(
    void *muxBuffer, void **targetBuffers, int ntargets,
    size_t targetSize, int sampleSize)
{
    // TODO: gotta go fast
    for (int i = 0; i < ntargets; ++i) {
        auto buf = ((char *)muxBuffer) + sampleSize * i;

        for (int j = 0; j < targetSize; j += sampleSize) {
            memcpy((char *)(targetBuffers[i]) + j, buf, sampleSize);
            buf += sampleSize * ntargets;
        }
    }
}

void SarClient::mux(
    void *muxBuffer, void **targetBuffers, int ntargets,
    size_t targetSize, int sampleSize)
{
}

} // namespace Sar
