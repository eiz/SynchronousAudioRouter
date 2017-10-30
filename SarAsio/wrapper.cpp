// SynchronousAudioRouter
// Copyright (C) 2015, 2016 Mackenzie Straight
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

static const char kNoInterfaceSelected[] = "No Interface Selected";

SarAsioWrapper::SarAsioWrapper()
{
    LOG(INFO) << "SarAsioWrapper::SarAsioWrapper";
    _config = DriverConfig::fromFile(ConfigurationPath("default.json"));
}

AsioBool SarAsioWrapper::init(void *sysHandle)
{
    LOG(INFO) << "SarAsioWrapper::init";

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
    LOG(INFO) << "SarAsioWrapper::getDriverName";
    strcpy_s(name, 32, "Synchronous Audio Router");
}

long SarAsioWrapper::getDriverVersion()
{
    LOG(INFO) << "SarAsioWrapper::getDriverVersion";
    return 1;
}

void SarAsioWrapper::getErrorMessage(char str[124])
{
    LOG(INFO) << "SarAsioWrapper::getErrorMessage";
    strcpy_s(str, 124, "");
}

AsioStatus SarAsioWrapper::start()
{
    LOG(INFO) << "SarAsioWrapper::start";

    if (!_innerDriver) {
        return AsioStatus::OK;
    }

    _sar = std::make_shared<SarClient>(_config, _bufferConfig);

    if (!_sar->start()) {
        LOG(INFO) << "Failed to start SAR";
        return AsioStatus::HardwareMalfunction;
    }

    return _innerDriver->start();
}

AsioStatus SarAsioWrapper::stop()
{
    LOG(INFO) << "SarAsioWrapper::stop";

    if (!_innerDriver) {
        return AsioStatus::OK;
    }

    if(_sar)
        _sar->stop();

    return _innerDriver->stop();
}

AsioStatus SarAsioWrapper::getChannels(long *inputCount, long *outputCount)
{
    LOG(INFO) << "SarAsioWrapper::getChannels";

    if (!_innerDriver) {
        *inputCount = *outputCount = 1;
        return AsioStatus::OK;
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
    LOG(INFO) << "SarAsioWrapper::getLatencies";

    if (!_innerDriver) {
        *inputLatency = *outputLatency = 1;
        return AsioStatus::OK;
    }

    return _innerDriver->getLatencies(inputLatency, outputLatency);
}

AsioStatus SarAsioWrapper::getBufferSize(
    long *minSize, long *maxSize, long *preferredSize, long *granularity)
{
    LOG(INFO) << "SarAsioWrapper::getBufferSize";

    if (!_innerDriver) {
        *minSize = *maxSize = *preferredSize = 64;
        *granularity = -1;
        return AsioStatus::OK;
    }

    auto status = _innerDriver->getBufferSize(
        minSize, maxSize, preferredSize, granularity);

    if (status != AsioStatus::OK) {
        return status;
    }

    // Limit the maximum buffer size to the system audio engine's periodicity if
    // possible. This prevents audio glitches caused by the ASIO event loop not
    // keeping up with the audio engine's event loop.
    double sampleRate;

    status = getSampleRate(&sampleRate);

    if (status != AsioStatus::OK) {
        return status;
    }

    // The audio engine's periodicity is always 10ms unless overriden by the
    // audio driver, which we don't do.
    long maxBufferSize = (long)(sampleRate / 100.0);

    // If we can't effectively limit the sample rate, give up and force the
    // minimum buffer size to be used.
    if (*minSize > maxBufferSize || !*granularity) {
        *minSize = *preferredSize = *maxSize;
        *granularity = 0;
        return AsioStatus::OK;
    }

    // Handle power of 2 buffer sizing.
    if (*granularity == -1) {
        while (*maxSize > maxBufferSize && *maxSize > *minSize) {
            *maxSize /= 2;
        }
    } else {
        while (*maxSize > maxBufferSize &&
               *maxSize - *minSize >= *granularity) {

            *maxSize -= *granularity;
        }
    }

    if (*preferredSize > *maxSize) {
        *preferredSize = *maxSize;
    }

    return AsioStatus::OK;
}

AsioStatus SarAsioWrapper::canSampleRate(double sampleRate)
{
    LOG(INFO) << "SarAsioWrapper::canSampleRate";

    if (!_innerDriver) {
        return AsioStatus::OK;
    }

    return _innerDriver->canSampleRate(sampleRate);
}

AsioStatus SarAsioWrapper::getSampleRate(double *sampleRate)
{
    LOG(INFO) << "SarAsioWrapper::getSampleRate";

    if (!_innerDriver) {
        *sampleRate = 48000.0;
        return AsioStatus::OK;
    }

    return _innerDriver->getSampleRate(sampleRate);
}

AsioStatus SarAsioWrapper::setSampleRate(double sampleRate)
{
    LOG(INFO) << "SarAsioWrapper::setSampleRate";

    if (!_innerDriver) {
        return AsioStatus::OK;
    }

    return _innerDriver->setSampleRate(sampleRate);
}

AsioStatus SarAsioWrapper::getClockSources(AsioClockSource *clocks, long *count)
{
    LOG(INFO) << "SarAsioWrapper::getClockSources";

    if (!_innerDriver) {
        if (*count < 1) {
            return AsioStatus::InvalidParameter;
        }

        clocks->index = 0;
        clocks->channel = 0;
        clocks->group = 0;
        clocks->isCurrentSource = AsioBool::True;
        strcpy_s(clocks->name, 32, kNoInterfaceSelected);
        return AsioStatus::OK;
    }

    return _innerDriver->getClockSources(clocks, count);
}

AsioStatus SarAsioWrapper::setClockSource(long index)
{
    LOG(INFO) << "SarAsioWrapper::setClockSource";

    if (!_innerDriver) {
        return AsioStatus::OK;
    }

    return _innerDriver->setClockSource(index);
}

AsioStatus SarAsioWrapper::getSamplePosition(int64_t *pos, int64_t *timestamp)
{
    if (!_innerDriver) {
        *pos = 0;
        *timestamp = 0;
        return AsioStatus::OK;
    }

    return _innerDriver->getSamplePosition(pos, timestamp);
}

AsioStatus SarAsioWrapper::getChannelInfo(AsioChannelInfo *info)
{
    if (!_innerDriver) {
        if (info->index != 0) {
            return AsioStatus::NotPresent;
        }

        info->isActive = _isFakeChannelStarted[info->isInput == AsioBool::True];
        info->group = 0;
        info->sampleType = (long)AsioSampleType::Int32LSB;
        strcpy_s(info->name, 32, kNoInterfaceSelected);
        return AsioStatus::OK;
    }

    long inputChannels = 0, outputChannels = 0;
    auto status = _innerDriver->getChannels(&inputChannels, &outputChannels);
    AsioChannelInfo query;

    if (status != AsioStatus::OK) {
        return status;
    }

    // We require at least one physical channel of the same kind  as the virtual
    // to exist, so we can mimic the sample type of the underlying hardware.
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

    if (index < (int)channels.size()) {
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
    LOG(INFO) << "SarAsioWrapper::createBuffers(infos, " << channelCount
       << ", " << bufferSize << ", callbacks)";

    std::vector<AsioBufferInfo> physicalChannelBuffers;
    std::vector<int> physicalChannelIndices;
    std::vector<int> virtualChannelIndices;
    long physicalInputCount = 0, physicalOutputCount = 0;
    double sampleRate = 0;
    AsioStatus status;

    if (!_innerDriver) {
        LOG(INFO) << "SarAsioWrapper::createBuffers: no inner driver, trying "
            << "to create fake channels.";

        // Allow allocating a single fake channel if hardware device is not yet
        // selected. Supports one input and one output.
        if (channelCount > 2) {
            return AsioStatus::InvalidMode;
        }

        for (long i = 0; i < channelCount; ++i) {
            if (infos[i].index != 0) {
                return AsioStatus::InvalidMode;
            }
        }

        _fakeBuffers.clear();

        for (long i = 0; i < channelCount; ++i) {
            // Dummy channel is always Int32LSB sample type.
            infos[i].asioBuffers[0] = calloc(bufferSize, 4);
            infos[i].asioBuffers[1] = calloc(bufferSize, 4);
            _isFakeChannelStarted[infos[i].isInput == AsioBool::True] =
                AsioBool::True;
            _fakeBuffers.push_back(infos[i].asioBuffers[0]);
            _fakeBuffers.push_back(infos[i].asioBuffers[1]);
        }

        return AsioStatus::OK;
    }

    _callbacks.tick = &SarAsioWrapper::onTickStub;
    _callbacks.tickWithTime = nullptr;
    _callbacks.asioMessage = callbacks->asioMessage;
    _callbacks.sampleRateDidChange = callbacks->sampleRateDidChange;

    status = _innerDriver->getChannels(
        &physicalInputCount, &physicalOutputCount);

    if (status != AsioStatus::OK) {
        LOG(ERROR) << "Couldn't get channel count from inner driver: "
            << (int)status;
        return status;
    }

    status = _innerDriver->getSampleRate(&sampleRate);

    if (status != AsioStatus::OK) {
        LOG(ERROR) << "Couldn't get sample rate from inner driver: "
            << (int)status;
        return status;
    }

    // See comment in SarAsioWrapper::getBufferSize for details.
    if (bufferSize > (long)(sampleRate / 100.0)) {
        LOG(ERROR) << "Invalid buffer size: larger than system audio engine periodicity";
        return AsioStatus::InvalidMode;
    }

    for (long i = 0; i < channelCount; ++i) {
        auto count = infos[i].isInput == AsioBool::True ?
            physicalInputCount : physicalOutputCount;
        auto& channels = infos[i].isInput == AsioBool::True ?
            _virtualInputs : _virtualOutputs;

        if (infos[i].index >= count) {
            if (infos[i].index >= count + (int)channels.size()) {
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

    LOG(INFO) << "Creating inner driver buffers."
        << " Count: " << physicalChannelBuffers.size()
        << " BufferSize: " << bufferSize
        << " Callbacks: " << &_callbacks;

    for (auto& physical : physicalChannelBuffers) {
        LOG(INFO) << "  ChannelInfo:"
            << " buffer[0]: " << physical.asioBuffers[0]
            << " buffer[1]: " << physical.asioBuffers[1]
            << " index: " << physical.index
            << " isInput: " << (int)physical.isInput;
    }

    status = _innerDriver->createBuffers(
        physicalChannelBuffers.data(), (long)physicalChannelBuffers.size(),
        bufferSize, &_callbacks);

    if (status != AsioStatus::OK) {
        LOG(ERROR) << "Couldn't create inner driver buffers: "
            << (int)status;
        return status;
    }

    for (size_t i = 0; i < physicalChannelBuffers.size(); ++i) {
        infos[physicalChannelIndices[i]] = physicalChannelBuffers[i];
    }

    _bufferConfig.frameSampleCount = bufferSize;
    _bufferConfig.sampleSize = 4; // TODO: handle sample types properly.
    _bufferConfig.sampleRate = (int)sampleRate;
    _bufferConfig.asioBuffers.clear();
    _bufferConfig.asioBuffers.resize(_config.endpoints.size());

    for (size_t i = 0; i < _config.endpoints.size(); ++i) {
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
        LOG(ERROR) << "Client attempted to create multiple active instances of "
            << "SarAsioWrapper. This is currently unsupported.";
        disposeBuffers();
        return AsioStatus::HardwareMalfunction;
    }

    return AsioStatus::OK;
}

AsioStatus SarAsioWrapper::disposeBuffers()
{
    LOG(INFO) << "SarAsioWrapper::disposeBuffers";

    if (!_innerDriver) {
        for (auto buf : _fakeBuffers) {
            free(buf);
        }

        _fakeBuffers.clear();
        _isFakeChannelStarted[0] = AsioBool::False;
        _isFakeChannelStarted[1] = AsioBool::False;
        return AsioStatus::OK;
    }

    stop();

    for (auto& endpointBuffers : _bufferConfig.asioBuffers) {
        for (auto buffer : endpointBuffers) {
            free(buffer);
        }
    }

    _bufferConfig.asioBuffers.clear();
    _callbacks = {};
    _userTick = nullptr;
    _userTickWithTime = nullptr;
    InterlockedExchangePointer((PVOID *)&gActiveWrapper, nullptr);
    return _innerDriver->disposeBuffers();
}

AsioStatus SarAsioWrapper::controlPanel()
{
    LOG(INFO) << "SarAsioWrapper::controlPanel";
    auto sheet = std::make_shared<ConfigurationPropertyDialog>(_config);

    if (sheet->show(_hwnd) > 0) {
        _config = sheet->newConfig();
        _config.writeFile(ConfigurationPath("default.json"));

        if (_callbacks.asioMessage) {
            _callbacks.asioMessage(
                AsioMessage::ResetRequest, 0, nullptr, nullptr);
        }
    }

    return AsioStatus::OK;
}

AsioStatus SarAsioWrapper::future(long selector, void *opt)
{
    LOG(INFO) << "SarAsioWrapper::future";

    if (!_innerDriver) {
        return AsioStatus::NotPresent;
    }

    return _innerDriver->future(selector, opt);
}

AsioStatus SarAsioWrapper::outputReady()
{
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
