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
#include "mmwrapper.h"
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
    bool hasUpdatedNotificationHandles = false;

    if (_updateSampleRateOnTick.exchange(false)) {
        DWORD dummy;

        DeviceIoControl(_device, SAR_SEND_FORMAT_CHANGE_EVENT,
            nullptr, 0, nullptr, 0, &dummy, nullptr);
    }

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
    for (size_t i = 0; i < _driverConfig.endpoints.size(); ++i) {
        auto& endpoint = _driverConfig.endpoints[i];
        auto& asioBuffers = _bufferConfig.asioBuffers[i];
        auto asioBufferSize = (DWORD)(
            _bufferConfig.frameSampleCount * _bufferConfig.sampleSize);
        auto generation = _registers[i].generation;
        auto endpointBufferOffset = _registers[i].bufferOffset;
        auto endpointBufferSize = _registers[i].bufferSize;
        auto positionRegister = _registers[i].positionRegister;
        auto ntargets = (int)(asioBuffers.size() / 2);
        auto frameSize = asioBufferSize * ntargets;
        auto notificationCount = _registers[i].notificationCount;
        void **targetBuffers = (void **)alloca(sizeof(void *) * ntargets);

        for (size_t bi = bufferIndex, ti = 0;
             bi < asioBuffers.size();
             bi += 2) {

            targetBuffers[ti++] = asioBuffers[bi];
        }

        if (!hasUpdatedNotificationHandles && notificationCount &&
            (GENERATION_NUMBER(generation) !=
             GENERATION_NUMBER(_notificationHandles[i].generation))) {

            updateNotificationHandles();
            hasUpdatedNotificationHandles = true;
        }

        if (!GENERATION_IS_ACTIVE(generation) ||
            !endpointBufferSize ||
            positionRegister > endpointBufferSize ||
            endpointBufferOffset + endpointBufferSize > _sharedBufferSize) {
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
        void *endpointDataFirst = ((char *)_sharedBuffer) + position;
        void *endpointDataSecond =
            ((char *)_sharedBuffer) + endpointBufferOffset;
        auto firstSize = min(frameSize, endpointBufferSize - positionRegister);
        auto secondSize = frameSize - firstSize;

        if (endpoint.type == EndpointType::Playback) {
            demux(
                endpointDataFirst, firstSize,
                endpointDataSecond, secondSize,
                targetBuffers, ntargets,
                asioBufferSize, _bufferConfig.sampleSize);
        } else {
            mux(
                endpointDataFirst, firstSize,
                endpointDataSecond, secondSize,
                targetBuffers, ntargets,
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
            if ((notificationCount >= 1 &&
                 positionRegister >= endpointBufferSize / 2 &&
                 nextPositionRegister < endpointBufferSize / 2) ||
                (notificationCount >= 2 &&
                 nextPositionRegister >= endpointBufferSize / 2 &&
                 positionRegister < endpointBufferSize / 2)) {

                auto evt = _notificationHandles[i].handle;
                auto targetGeneration = _notificationHandles[i].generation;

                if (evt &&
                    (GENERATION_NUMBER(targetGeneration) ==
                     GENERATION_NUMBER(generation))) {

                    _registers[i].positionRegister = nextPositionRegister;

                    if (!SetEvent(evt)) {
                        LOG(ERROR) << "SetEvent error " << GetLastError();
                    }
                } else {
                    for (int ti = 0; ti < ntargets; ++ti) {
                        if (targetBuffers[ti]) {
                            ZeroMemory(targetBuffers[ti], asioBufferSize);
                        }
                    }
                }
            } else {
                _registers[i].positionRegister = nextPositionRegister;
            }
        }
    }
}

bool SarClient::start()
{
    if (!openControlDevice()) {
        LOG(ERROR) << "Couldn't open control device";
        return false;
    }

    if (!openMmNotificationClient()) {
        LOG(ERROR) << "Couldn't open MMDevice notification client";
        stop();
        return false;
    }

    if (!setBufferLayout()) {
        LOG(ERROR) << "Couldn't set layout";
        stop();
        return false;
    }

    if (!createEndpoints()) {
        LOG(ERROR) << "Couldn't create endpoints";
        stop();
        return false;
    }

    if (_driverConfig.enableApplicationRouting && !enableRegistryFilter()) {
        LOG(ERROR) << "Couldn't enable registry filter";
    }

    return true;
}

void SarClient::stop()
{
    if (_mmNotificationClientRegistered) {
        _mmEnumerator->UnregisterEndpointNotificationCallback(
            _mmNotificationClient);
        _mmNotificationClientRegistered = false;
    }

    if (_mmNotificationClient) {
        _mmNotificationClient->Release();
        _mmNotificationClient = nullptr;
    }

    if (_mmEnumerator) {
        _mmEnumerator = nullptr;
    }

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

        free(interfaceDetail);
        return false;
    }

    _device = CreateFile(interfaceDetail->DevicePath,
        GENERIC_ALL, 0, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL|FILE_FLAG_OVERLAPPED, nullptr);

    if (_device == INVALID_HANDLE_VALUE) {
        free(interfaceDetail);
        return false;
    }

    _completionPort = CreateIoCompletionPort(_device, nullptr, 0, 0);

    if (!_completionPort) {
        CloseHandle(_device);
        _device = nullptr;
        free(interfaceDetail);
        return false;
    }

    _notificationHandles.clear();
    _notificationHandles.resize(_driverConfig.endpoints.size());
    free(interfaceDetail);
    return true;
}

bool SarClient::openMmNotificationClient()
{
    if (FAILED(CoCreateInstance(
        __uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
        __uuidof(IMMDeviceEnumerator), (LPVOID *)&_mmEnumerator))) {

        return false;
    }

    if (FAILED(CComObject<NotificationClient>::CreateInstance(
        &_mmNotificationClient))) {

        return false;
    }

    _mmNotificationClient->AddRef();
    _mmNotificationClient->setClient(shared_from_this());

    if (FAILED(_mmEnumerator->RegisterEndpointNotificationCallback(
        _mmNotificationClient))) {

        return false;
    }

    _mmNotificationClientRegistered = true;
    return true;
}

bool SarClient::setBufferLayout()
{
    SarSetBufferLayoutRequest request = {};
    SarSetBufferLayoutResponse response = {};
    DWORD dummy;

    request.bufferSize = 1024 * 1024 * 16; // TODO: size based on endpoint config
    request.frameSize =
        _bufferConfig.frameSampleCount * _bufferConfig.sampleSize;
    request.sampleRate = _bufferConfig.sampleRate;
    request.sampleSize = _bufferConfig.sampleSize;

    if (_driverConfig.waveRtMinimumFrames >= 2) {
        request.minimumFrameCount = _driverConfig.waveRtMinimumFrames;
    }

    if (!DeviceIoControl(_device, SAR_SET_BUFFER_LAYOUT,
        (LPVOID)&request, sizeof(request), (LPVOID)&response, sizeof(response),
        &dummy, nullptr)) {

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
    DWORD dummy;

    for (auto& endpoint : _driverConfig.endpoints) {
        SarCreateEndpointRequest request = {};

        request.type = endpoint.type == EndpointType::Playback ?
            SAR_ENDPOINT_TYPE_PLAYBACK : SAR_ENDPOINT_TYPE_RECORDING;
        request.channelCount = endpoint.channelCount;
        request.index = i++;
        wcscpy_s(request.name, UTF8ToWide(endpoint.description).c_str());
        wcscpy_s(request.id, UTF8ToWide(endpoint.id).c_str());

        if (!DeviceIoControl(_device, SAR_CREATE_ENDPOINT,
            (LPVOID)&request, sizeof(request), nullptr, 0, &dummy, nullptr)) {

            LOG(ERROR) << "Endpoint creation for " << endpoint.description
               << " failed.";
            return false;
        }
    }

    return true;
}

bool SarClient::enableRegistryFilter()
{
    DWORD dummy;

    return DeviceIoControl(_device, SAR_START_REGISTRY_FILTER,
        nullptr, 0, nullptr, 0, &dummy, nullptr) == TRUE;
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
            // Ignore failed operations. Can happen due to tick/stop races.
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
        _device, SAR_WAIT_HANDLE_QUEUE, nullptr, 0,
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
        // Ignore failed operations. Can happen due to tick/stop races.
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
    void *muxBufferFirst, size_t firstSize,
    void *muxBufferSecond, size_t secondSize,
    void **targetBuffers, int ntargets,
    size_t targetSize, int sampleSize)
{
    size_t stride = (size_t)(sampleSize * ntargets);

    // TODO: gotta go fast
    for (int i = 0; i < ntargets; ++i) {
        auto buf = ((char *)muxBufferFirst) + sampleSize * i;
        auto remaining = firstSize;

        if (!targetBuffers[i]) {
            continue;
        }

        for (size_t j = 0;
             j < targetSize && remaining >= stride;
             j += sampleSize) {

            memcpy((char *)(targetBuffers[i]) + j, buf, sampleSize);
            buf += stride;
            remaining -= stride;

            if (!remaining) {
                buf = ((char *)muxBufferSecond) + sampleSize * i;
                remaining = secondSize;
            }
        }
    }
}

// TODO: copypasta
void SarClient::mux(
    void *muxBufferFirst, size_t firstSize,
    void *muxBufferSecond, size_t secondSize,
    void **targetBuffers, int ntargets,
    size_t targetSize, int sampleSize)
{
    size_t stride = (size_t)(sampleSize * ntargets);

    // TODO: gotta go fast
    for (int i = 0; i < ntargets; ++i) {
        auto buf = ((char *)muxBufferFirst) + sampleSize * i;
        auto remaining = firstSize;

        if (!targetBuffers[i]) {
            continue;
        }

        for (size_t j = 0;
             j < targetSize && remaining >= stride;
             j += sampleSize) {

            memcpy(buf, (char *)(targetBuffers[i]) + j, sampleSize);
            buf += sampleSize * ntargets;
            remaining -= sampleSize * ntargets;

            if (!remaining) {
                buf = ((char *)muxBufferSecond) + sampleSize * i;
                remaining = secondSize;
            }
        }
    }
}

HRESULT STDMETHODCALLTYPE SarClient::NotificationClient::OnDeviceStateChanged(
    _In_  LPCWSTR pwstrDeviceId,
    _In_  DWORD dwNewState)
{
    // When a SAR endpoint is re-activated after its initial creation, its
    // supported sample rate may be different. To force the audio engine to
    // notice the possible format change, we listen for device state change
    // events and tell the kernel mode driver to broadcast a
    // KSEVENT_PINCAPS_FORMATCHANGE event, which causes the audio engine to
    // re-query the pin capabilities. This isn't needed for newly added
    // endpoints or non-SAR endpoints, so we filter out those events.
    if (dwNewState != DEVICE_STATE_ACTIVE) {
        return S_OK;
    }

    if (auto client = _client.lock()) {
        do {
            CComPtr<IMMDeviceEnumerator> mmEnumerator;
            CComPtr<IMMDevice> device;
            CComPtr<IPropertyStore> ps;
            PROPVARIANT pvalue = {};

            // This seems a bit shady, but MSDN's device events example
            // initializes COM the same way. It's not clear what the ownership
            // of the thread that delivers the IMMNotificationClient events is.
            CoInitialize(NULL);

            if (FAILED(CoCreateInstance(
                __uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                __uuidof(IMMDeviceEnumerator), (LPVOID *)&mmEnumerator))) {

                break;
            }

            if (FAILED(mmEnumerator->GetDevice(pwstrDeviceId, &device))) {
                break;
            }

            if (FAILED(device->OpenPropertyStore(STGM_READ, &ps))) {
                break;
            }

            if (FAILED(ps->GetValue(
                PKEY_SynchronousAudioRouter_EndpointId, &pvalue))) {

                break;
            }

            client->updateSampleRateOnTick();
            PropVariantClear(&pvalue);
        } while(false);

        CoUninitialize();
    }

    return S_OK;
}

HRESULT STDMETHODCALLTYPE SarClient::NotificationClient::OnDeviceAdded(
    _In_  LPCWSTR pwstrDeviceId)
{
    return S_OK;
}

HRESULT STDMETHODCALLTYPE SarClient::NotificationClient::OnDeviceRemoved(
    _In_  LPCWSTR pwstrDeviceId)
{
    return S_OK;
}

HRESULT STDMETHODCALLTYPE SarClient::NotificationClient::OnDefaultDeviceChanged(
    _In_  EDataFlow flow,
    _In_  ERole role,
    _In_  LPCWSTR pwstrDefaultDeviceId)
{
    return S_OK;
}

HRESULT STDMETHODCALLTYPE SarClient::NotificationClient::OnPropertyValueChanged(
    _In_  LPCWSTR pwstrDeviceId,
    _In_  const PROPERTYKEY key)
{
    return S_OK;
}

} // namespace Sar
