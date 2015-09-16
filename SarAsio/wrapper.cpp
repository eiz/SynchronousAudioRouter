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

SarAsioWrapper::SarAsioWrapper()
{
    OutputDebugString(L"SarAsioWrapper::SarAsioWrapper");
    _config = DriverConfig::fromFile(ConfigurationPath("default.json"));
}

AsioBool SarAsioWrapper::init(void *sysHandle)
{
    OutputDebugString(L"SarAsioWrapper::init");

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
    return AsioStatus::OK;
}

AsioStatus SarAsioWrapper::stop()
{
    OutputDebugString(L"SarAsioWrapper::stop");
    return AsioStatus::OK;
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
    OutputDebugString(L"SarAsioWrapper::getSamplePosition");

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
        info->sampleType = 1; // TODO: proper sample types
        info->isActive = AsioBool::False; // TODO
        strcpy_s(info->name, channels[index].name.c_str());
        return AsioStatus::OK;
    }

    return AsioStatus::NotPresent;
}

AsioStatus SarAsioWrapper::createBuffers(
    AsioBufferInfo *infos, long channelCount, long bufferSize,
    AsioCallbacks *callbacks)
{
    OutputDebugString(L"SarAsioWrapper::createBuffers");

    if (!_innerDriver) {
        return AsioStatus::NotPresent;
    }

    return _innerDriver->createBuffers(
        infos, channelCount, bufferSize, callbacks);
}

AsioStatus SarAsioWrapper::disposeBuffers()
{
    OutputDebugString(L"SarAsioWrapper::disposeBuffers");

    if (!_innerDriver) {
        return AsioStatus::NotPresent;
    }

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
    for (auto& endpoint : _config.endpoints) {
        for (int i = 0; i < endpoint.channelCount; ++i) {
            VirtualChannel chan;
            std::ostringstream os;

            os << endpoint.description << " " << (i + 1);
            chan.endpoint = &endpoint;
            chan.index = i;
            chan.name = os.str();

            if (endpoint.type == EndpointType::Recording) {
                _virtualOutputs.emplace_back(chan);
            } else {
                _virtualInputs.emplace_back(chan);
            }
        }
    }
}
