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
#include <initguid.h>
#include "configui.h"
#include "dllmain.h"
#include "tinyasio.h"
#include "wrapper.h"
#include "utility.h"

using namespace Sar;

SarAsioWrapper *gActiveWrapper;

SarAsioWrapper::SarAsioWrapper()
{
    OutputDebugString(L"SarAsioWrapper::SarAsioWrapper");
    _config = DriverConfig::fromFile(ConfigurationPath("default.json"));
}

AsioBool SarAsioWrapper::init(void *sysHandle)
{
    OutputDebugString(L"SarAsioWrapper::init");

    _userTick = nullptr;
    _userTickWithTime = nullptr;
    _hwnd = (HWND)sysHandle;

    if (!initInnerDriver()) {
        _config.driverClsid = "";
    }

    initVirtualChannels();
    return AsioBool::True;
}

void SarAsioWrapper::getDriverName(char name[32])
{
    OutputDebugString(L"SarAsioWrapper::getDriverName");
    strcpy_s(name, 32, "Synchronous Audio Router");
}

long SarAsioWrapper::getDriverVersion()
{
    OutputDebugString(L"SarAsioWrapper::getDriverVersion");
    return 1;
}

void SarAsioWrapper::getErrorMessage(char str[124])
{
    OutputDebugString(L"SarAsioWrapper::getErrorMessage");
    strcpy_s(str, 124, "");
}

AsioStatus SarAsioWrapper::start()
{
    OutputDebugString(L"SarAsioWrapper::start");

    if (!_innerDriver) {
        return AsioStatus::NotPresent;
    }

    _sar = std::make_unique<SarClient>(_config, _bufferConfig);

    if (!_sar->start()) {
        OutputDebugString(_T("Failed to start SAR"));
        return AsioStatus::HardwareMalfunction;
    }

    return _innerDriver->start();
}

AsioStatus SarAsioWrapper::stop()
{
    OutputDebugString(L"SarAsioWrapper::stop");

    if (!_innerDriver) {
        return AsioStatus::NotPresent;
    }

    _sar->stop();
    return _innerDriver->stop();
}

AsioStatus SarAsioWrapper::getChannels(long *inputCount, long *outputCount)
{
    OutputDebugString(L"SarAsioWrapper::getChannels");

    if (!_innerDriver) {
        *inputCount = *outputCount = 0;
        return AsioStatus::NotPresent;
    }

    auto status = _innerDriver->getChannels(inputCount, outputCount);

    if (status != AsioStatus::OK) {
        return status;
    }

    *inputCount += (long)_virtualInputs.size();
    *outputCount += (long)_virtualOutputs.size();
    return AsioStatus::OK;
}

AsioStatus SarAsioWrapper::getLatencies(long *inputLatency, long *outputLatency)
{
    OutputDebugString(L"SarAsioWrapper::getLatencies");

    if (!_innerDriver) {
        *inputLatency = *outputLatency = 0;
        return AsioStatus::NotPresent;
    }

    return _innerDriver->getLatencies(inputLatency, outputLatency);
}

AsioStatus SarAsioWrapper::getBufferSize(
    long *minSize, long *maxSize, long *preferredSize, long *granularity)
{
    OutputDebugString(L"SarAsioWrapper::getBufferSize");

    if (!_innerDriver) {
        return AsioStatus::NotPresent;
    }

    return _innerDriver->getBufferSize(
        minSize, maxSize, preferredSize, granularity);
}

AsioStatus SarAsioWrapper::canSampleRate(double sampleRate)
{
    OutputDebugString(L"SarAsioWrapper::canSampleRate");

    if (!_innerDriver) {
        return AsioStatus::NotPresent;
    }

    return _innerDriver->canSampleRate(sampleRate);
}

AsioStatus SarAsioWrapper::getSampleRate(double *sampleRate)
{
    OutputDebugString(L"SarAsioWrapper::getSampleRate");

    if (!_innerDriver) {
        return AsioStatus::NotPresent;
    }

    return _innerDriver->getSampleRate(sampleRate);
}

AsioStatus SarAsioWrapper::setSampleRate(double sampleRate)
{
    OutputDebugString(L"SarAsioWrapper::setSampleRate");

    if (!_innerDriver) {
        return AsioStatus::NotPresent;
    }

    return _innerDriver->setSampleRate(sampleRate);
}

AsioStatus SarAsioWrapper::getClockSources(AsioClockSource *clocks, long *count)
{
    OutputDebugString(L"SarAsioWrapper::getClockSources");

    if (!_innerDriver) {
        return AsioStatus::NotPresent;
    }

    return _innerDriver->getClockSources(clocks, count);
}

AsioStatus SarAsioWrapper::setClockSource(long index)
{
    OutputDebugString(L"SarAsioWrapper::setClockSource");

    if (!_innerDriver) {
        return AsioStatus::NotPresent;
    }

    return _innerDriver->setClockSource(index);
}

AsioStatus SarAsioWrapper::getSamplePosition(int64_t *pos, int64_t *timestamp)
{
    if (!_innerDriver) {
        return AsioStatus::NotPresent;
    }

    return _innerDriver->getSamplePosition(pos, timestamp);
}

AsioStatus SarAsioWrapper::getChannelInfo(AsioChannelInfo *info)
{
    OutputDebugString(L"SarAsioWrapper::getChannelInfo");

    if (!_innerDriver) {
        return AsioStatus::NotPresent;
    }

    long inputChannels = 0, outputChannels = 0;
    auto status = _innerDriver->getChannels(&inputChannels, &outputChannels);
    AsioChannelInfo query;

    if (status != AsioStatus::OK) {
        return status;
    }

    // TODO: for now, we require at least one physical channel of the same kind
    // as the virtual to exist, so we can mimic the sample type of the
    // underlying hardware.
    if ((info->isInput == AsioBool::True && inputChannels == 0) ||
        (info->isInput == AsioBool::False && outputChannels == 0)) {
        return AsioStatus::NotPresent;
    }

    query.index = 0;
    query.isInput = info->isInput;
    status = _innerDriver->getChannelInfo(&query);

    if (status != AsioStatus::OK) {
        return status;
    }

    auto innerCount = info->isInput == AsioBool::True ?
        inputChannels : outputChannels;

    if (info->index < innerCount) {
        return _innerDriver->getChannelInfo(info);
    }

    auto& channels = info->isInput == AsioBool::True ?
        _virtualInputs : _virtualOutputs;
    auto index = info->index - innerCount;

    if (index < channels.size()) {
        info->group = 0;
        info->sampleType = query.sampleType;
        info->isActive = AsioBool::False; // TODO: when is this true?
        strcpy_s(info->name, channels[index].name.c_str());
        return AsioStatus::OK;
    }

    return AsioStatus::NotPresent;
}

AsioStatus SarAsioWrapper::createBuffers(
    AsioBufferInfo *infos, long channelCount, long bufferSize,
    AsioCallbacks *callbacks)
{
    std::ostringstream os;

    os << "SarAsioWrapper::createBuffers(infos, " << channelCount
       << ", " << bufferSize << ", callbacks)";
    OutputDebugStringA(os.str().c_str());
    _callbacks.tick = &SarAsioWrapper::onTickStub;
    _callbacks.tickWithTime = nullptr;
    _callbacks.asioMessage = callbacks->asioMessage;
    _callbacks.sampleRateDidChange = callbacks->sampleRateDidChange;

    std::vector<AsioBufferInfo> physicalChannelBuffers;
    std::vector<int> physicalChannelIndices;
    std::vector<int> virtualChannelIndices;
    long physicalInputCount = 0, physicalOutputCount = 0;
    double sampleRate = 0;
    AsioStatus status;

    if (!_innerDriver) {
        return AsioStatus::NotPresent;
    }

    status = _innerDriver->getChannels(
        &physicalInputCount, &physicalOutputCount);

    if (status != AsioStatus::OK) {
        return status;
    }

    status = _innerDriver->getSampleRate(&sampleRate);

    if (status != AsioStatus::OK) {
        return status;
    }

    for (long i = 0; i < channelCount; ++i) {
        auto count = infos[i].isInput == AsioBool::True ?
            physicalInputCount : physicalOutputCount;
        auto& channels = infos[i].isInput == AsioBool::True ?
            _virtualInputs : _virtualOutputs;

        if (infos[i].index >= count) {
            if (infos[i].index >= count + channels.size()) {
                return AsioStatus::NotPresent;
            }

            virtualChannelIndices.emplace_back(i);
        } else {
            physicalChannelBuffers.emplace_back(infos[i]);
            physicalChannelIndices.emplace_back(i);
        }
    }

    _userTick = callbacks->tick;

    if (callbacks->asioMessage(AsioMessage::SupportsTimeInfo,
        0, nullptr, nullptr)) {

        _userTickWithTime = callbacks->tickWithTime;
        _callbacks.tickWithTime = &SarAsioWrapper::onTickWithTimeStub;
    }

    status = _innerDriver->createBuffers(
        physicalChannelBuffers.data(), (long)physicalChannelBuffers.size(),
        bufferSize, &_callbacks);

    if (status != AsioStatus::OK) {
        return status;
    }

    for (long i = 0; i < physicalChannelBuffers.size(); ++i) {
        infos[physicalChannelIndices[i]] = physicalChannelBuffers[i];
    }

    _bufferConfig.frameSampleCount = bufferSize;
    _bufferConfig.sampleSize = 4; // TODO: handle sample types properly.
    // TODO: asio can report non-integer sample rates. do we need to care?
    _bufferConfig.sampleRate = (int)sampleRate;
    _bufferConfig.asioBuffers.clear();
    _bufferConfig.asioBuffers.resize(_config.endpoints.size());

    for (int i = 0; i < _config.endpoints.size(); ++i) {
        _bufferConfig.asioBuffers[i].resize(
            _config.endpoints[i].channelCount * 2);
    }

    for (auto i : virtualChannelIndices) {
        auto count = infos[i].isInput == AsioBool::True ?
            physicalInputCount : physicalOutputCount;
        auto& channels = infos[i].isInput == AsioBool::True ?
            _virtualInputs : _virtualOutputs;
        auto& channel = channels[infos[i].index - count];

        // TODO: size buffers based on sample type
        channel.asioBuffers[0] =
            infos[i].asioBuffers[0] = calloc(bufferSize, 4);
        channel.asioBuffers[1] =
            infos[i].asioBuffers[1] = calloc(bufferSize, 4);
        _bufferConfig
            .asioBuffers[channel.endpointIndex][channel.channelIndex * 2] =
                channel.asioBuffers[0];
        _bufferConfig
            .asioBuffers[channel.endpointIndex][channel.channelIndex * 2 + 1] =
                channel.asioBuffers[1];
    }

    InterlockedCompareExchangePointer((PVOID *)&gActiveWrapper, this, nullptr);

    // We need a thiscall thunk to support multiple active instances, and
    // there doesn't seem to be any reasonable use case for that, so for now
    // just use a global reference to our wrapper.
    if (gActiveWrapper != this) {
        disposeBuffers();
        return AsioStatus::HardwareMalfunction; // TODO: error code?
    }

    return AsioStatus::OK;
}

AsioStatus SarAsioWrapper::disposeBuffers()
{
    OutputDebugString(L"SarAsioWrapper::disposeBuffers");

    if (!_innerDriver) {
        return AsioStatus::NotPresent;
    }

    InterlockedExchangePointer((PVOID *)&gActiveWrapper, nullptr);
    return _innerDriver->disposeBuffers();
}

AsioStatus SarAsioWrapper::controlPanel()
{
    OutputDebugString(L"SarAsioWrapper::controlPanel");
    auto sheet = std::make_shared<ConfigurationPropertyDialog>(_config);

    if (sheet->show(_hwnd) > 0) {
        _config = sheet->newConfig();
        _config.writeFile(ConfigurationPath("default.json"));
    }

    return AsioStatus::OK;
}

AsioStatus SarAsioWrapper::future(long selector, void *opt)
{
    OutputDebugString(L"SarAsioWrapper::future");

    if (!_innerDriver) {
        return AsioStatus::NotPresent;
    }

    return _innerDriver->future(selector, opt);
}

AsioStatus SarAsioWrapper::outputReady()
{
    OutputDebugString(L"SarAsioWrapper::outputReady");

    if (!_innerDriver) {
        return AsioStatus::NotPresent;
    }

    return _innerDriver->outputReady();
}

bool SarAsioWrapper::initInnerDriver()
{
    _innerDriver = nullptr;

    for (auto driver : InstalledAsioDrivers()) {
        if (driver.clsid == _config.driverClsid) {
            if (!SUCCEEDED(driver.open(&_innerDriver))) {
                return false;
            }

            if (_innerDriver->init(_hwnd) != AsioBool::True) {
                return false;
            }

            return true;
        }
    }

    return false;
}

void SarAsioWrapper::initVirtualChannels()
{
    _virtualInputs.clear();
    _virtualOutputs.clear();

    int endpointIndex = 0;

    for (auto& endpoint : _config.endpoints) {
        for (int i = 0; i < endpoint.channelCount; ++i) {
            VirtualChannel chan;
            std::ostringstream os;

            os << endpoint.description << " " << (i + 1);
            chan.endpointIndex = endpointIndex;
            chan.channelIndex = i;
            chan.name = os.str();

            if (endpoint.type == EndpointType::Recording) {
                _virtualOutputs.emplace_back(chan);
            } else {
                _virtualInputs.emplace_back(chan);
            }
        }

        endpointIndex++;
    }
}

void SarAsioWrapper::onTick(long bufferIndex, AsioBool directProcess)
{
    _sar->tick(bufferIndex);
    _userTick(bufferIndex, directProcess);
}

void SarAsioWrapper::onTickStub(long bufferIndex, AsioBool directProcess)
{
    auto wrapper = gActiveWrapper;

    if (wrapper) {
        wrapper->onTick(bufferIndex, directProcess);
    }
}

AsioTime *SarAsioWrapper::onTickWithTime(
    AsioTime *time, long bufferIndex, AsioBool directProcess)
{
    _sar->tick(bufferIndex);
    return _userTickWithTime(time, bufferIndex, directProcess);
}

AsioTime *SarAsioWrapper::onTickWithTimeStub(
    AsioTime *time, long bufferIndex, AsioBool directProcess)
{
    auto wrapper = gActiveWrapper;

    if (wrapper) {
        return wrapper->onTickWithTime(time, bufferIndex, directProcess);
    }

    return time;
}
