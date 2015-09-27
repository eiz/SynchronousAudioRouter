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
      _device(INVALID_HANDLE_VALUE), _completionPort(nullptr),
      _handleQueueStarted(false)
{
    ZeroMemory(&_handleQueueCompletion, sizeof(HandleQueueCompletion));
}

void SarClient::tick(long bufferIndex)
{
    ATLASSERT(bufferIndex == 0 || bufferIndex == 1);

    updateNotificationHandles();

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
        auto generation = _registers[i].generation;
        auto endpointBufferOffset = _registers[i].bufferOffset;
        auto endpointBufferSize = _registers[i].bufferSize;
        auto positionRegister = _registers[i].positionRegister;
        auto ntargets = (int)(asioBuffers.size() / 2);
        auto frameSize = asioBufferSize * ntargets;
        auto notificationCount = _registers[i].notificationCount;
        void **targetBuffers = (void **)alloca(sizeof(void *) * ntargets);

        //std::ostringstream os;

        //os << i << "(" << generation << "): Offset: " << endpointBufferOffset
        //   << " Size: " << endpointBufferSize << " Pos: " << positionRegister
        //   << " FrameSize: " << frameSize << " NotificationCount: "
        //   << notificationCount;
        //OutputDebugStringA(os.str().c_str());

        for (int bi = bufferIndex, ti = 0; bi < asioBuffers.size(); bi += 2) {
            targetBuffers[ti++] = asioBuffers[bi];
        }

        if (!GENERATION_IS_ACTIVE(generation) || !endpointBufferSize) {
            for (int ti = 0; ti < ntargets; ++ti) {
                if (targetBuffers[ti]) {
                    ZeroMemory(targetBuffers[ti], asioBufferSize);
                }
            }

            continue;
        }

        auto nextPositionRegister =
            (positionRegister + frameSize) % endpointBufferSize;
        auto previousPositionRegister = positionRegister > 0 ?
            (positionRegister - frameSize) % endpointBufferSize :
            endpointBufferSize - frameSize;
        auto position = positionRegister + endpointBufferOffset;
        void *endpointData = ((char *)_sharedBuffer) + position;

        if (endpointBufferOffset + endpointBufferSize > _sharedBufferSize ||
            position + frameSize > _sharedBufferSize ||
            positionRegister + frameSize > endpointBufferSize) {

            continue;
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

        auto lateGeneration = _registers[i].generation;

        if (!GENERATION_IS_ACTIVE(lateGeneration) ||
            (GENERATION_NUMBER(generation) !=
             GENERATION_NUMBER(lateGeneration))) {

            for (int ti = 0; ti < ntargets; ++ti) {
                if (targetBuffers[ti]) {
                    ZeroMemory(targetBuffers[ti], asioBufferSize);
                }
            }
        } else {
            // TODO: this can still race if the endpoint is stopped and started
            // while we're updating the position register. Maybe use DCAS on the
            // generation+position? Does it actually matter?
            _registers[i].positionRegister = nextPositionRegister;

            if ((notificationCount >= 1 && nextPositionRegister == 0) ||
                (notificationCount >= 2 &&
                 nextPositionRegister >= endpointBufferSize / 2 &&
                 positionRegister < endpointBufferSize / 2)) {

                auto evt = _notificationHandles[i].handle;

                if (evt) {
                    if (!SetEvent(evt)) {
                        auto error = GetLastError();
                        std::ostringstream os;

                        os << "SetEvent error " << error;
                        OutputDebugStringA(os.str().c_str());
                    }
                }
            }
        }
    }
}

bool SarClient::start()
{
    if (!openControlDevice()) {
        OutputDebugString(_T("Couldn't open control device"));
        return false;
    }

    if (!setBufferLayout()) {
        OutputDebugString(_T("Couldn't set layout"));
        stop();
        return false;
    }

    if (!createEndpoints()) {
        OutputDebugString(_T("Couldn't create endpoints"));
        stop();
        return false;
    }

    return true;
}

void SarClient::stop()
{
    if (_device != INVALID_HANDLE_VALUE) {
        CancelIoEx(_device, nullptr);
        CloseHandle(_device);
        _device = INVALID_HANDLE_VALUE;
    }

    if (_completionPort) {
        CloseHandle(_completionPort);
        _completionPort = nullptr;
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
        GENERIC_ALL, 0, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL|FILE_FLAG_OVERLAPPED, nullptr);

    if (_device == INVALID_HANDLE_VALUE) {
        return false;
    }

    _completionPort = CreateIoCompletionPort(_device, nullptr, 0, 0);

    if (!_completionPort) {
        CloseHandle(_device);
        _device = nullptr;
        return false;
    }

    _notificationHandles.clear();
    _notificationHandles.resize(_driverConfig.endpoints.size());
    return true;
}

bool SarClient::setBufferLayout()
{
    SarSetBufferLayoutRequest request = {};
    SarSetBufferLayoutResponse response = {};

    request.bufferSize = 1024 * 1024 * 16; // TODO: size based on endpoint config
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

void SarClient::updateNotificationHandles()
{
    bool startNewOperation = false;
    BOOL status;
    DWORD error;

    if (_handleQueueStarted) {
        DWORD bytes = 0;
        ULONG_PTR key = 0;
        LPOVERLAPPED overlapped = nullptr;

        if (GetQueuedCompletionStatus(
            _completionPort, &bytes, &key, &overlapped, 0)) {

            processNotificationHandleUpdates(
                bytes / sizeof(SarHandleQueueResponse));
            startNewOperation = true;
        } else if (overlapped) {
            OutputDebugStringA("Dequeued a failed operation"); // TODO
            startNewOperation = true;
        }
    } else {
        _handleQueueStarted = true;
        startNewOperation = true;
    }

    if (!startNewOperation) {
        return;
    }

    ZeroMemory((LPOVERLAPPED)&_handleQueueCompletion, sizeof(OVERLAPPED));
    status = DeviceIoControl(
        _device, SAR_REQUEST_GET_NOTIFICATION_EVENTS, nullptr, 0,
        _handleQueueCompletion.responses,
        sizeof(_handleQueueCompletion.responses),
        nullptr, &_handleQueueCompletion);
    error = GetLastError();

    // DeviceIoControl completed synchronously, so process the result.
    if (status) {
        processNotificationHandleUpdates(
            (int)(_handleQueueCompletion.InternalHigh /
                  sizeof(SarHandleQueueResponse)));
        _handleQueueStarted = false;
    }

    if (error != ERROR_IO_PENDING) {
        OutputDebugStringA("SAR_REQUEST_GET_NOTIFICATION_EVENTS failed");
        _handleQueueStarted = false;
    }
}

void SarClient::processNotificationHandleUpdates(int updateCount)
{
    for (int i = 0; i < updateCount; ++i) {
        SarHandleQueueResponse *response = &_handleQueueCompletion.responses[i];
        DWORD endpointIndex = (DWORD)(response->associatedData >> 32);
        DWORD generation = (DWORD)(response->associatedData & 0xFFFFFFFF);

        if (_notificationHandles[endpointIndex].handle) {
            CloseHandle(_notificationHandles[endpointIndex].handle);
        }

        _notificationHandles[endpointIndex].generation = generation;
        _notificationHandles[endpointIndex].handle = response->handle;
    }
}

void SarClient::demux(
    void *muxBuffer, void **targetBuffers, int ntargets,
    size_t targetSize, int sampleSize)
{
    // TODO: gotta go fast
    for (int i = 0; i < ntargets; ++i) {
        auto buf = ((char *)muxBuffer) + sampleSize * i;

        if (!targetBuffers[i]) {
            continue;
        }

        for (int j = 0; j < targetSize; j += sampleSize) {
            memcpy((char *)(targetBuffers[i]) + j, buf, sampleSize);
            buf += sampleSize * ntargets;
        }
    }
}

// TODO: copypasta
void SarClient::mux(
    void *muxBuffer, void **targetBuffers, int ntargets,
    size_t targetSize, int sampleSize)
{
    // TODO: gotta go fast
    for (int i = 0; i < ntargets; ++i) {
        auto buf = ((char *)muxBuffer) + sampleSize * i;

        if (!targetBuffers[i]) {
            continue;
        }

        for (int j = 0; j < targetSize; j += sampleSize) {
            memcpy(buf, (char *)(targetBuffers[i]) + j, sampleSize);
            buf += sampleSize * ntargets;
        }
    }
}

} // namespace Sar
