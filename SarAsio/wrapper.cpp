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
#include "wrapper.h"

using namespace Sar;

SarAsioWrapper::SarAsioWrapper()
{
    OutputDebugString(L"SarAsioWrapper::SarAsioWrapper");
}

bool SarAsioWrapper::init(void *sysHandle)
{
    OutputDebugString(L"SarAsioWrapper::init");

    _hwnd = (HWND)sysHandle;
    return true;
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

long SarAsioWrapper::start()
{
    OutputDebugString(L"SarAsioWrapper::start");
    return 0;
}

long SarAsioWrapper::stop()
{
    OutputDebugString(L"SarAsioWrapper::stop");
    return 0;
}

long SarAsioWrapper::getChannels(long *inputCount, long *outputCount)
{
    OutputDebugString(L"SarAsioWrapper::getChannels");
    return 0;
}

long SarAsioWrapper::getLatencies(long *inputLatency, long *outputLatency)
{
    OutputDebugString(L"SarAsioWrapper::getLatencies");
    return 0;
}

long SarAsioWrapper::getBufferSize(
    long *minSize, long *maxSize, long *preferredSize, long *granularity)
{
    OutputDebugString(L"SarAsioWrapper::getBufferSize");
    return 0;
}

long SarAsioWrapper::canSampleRate(double sampleRate)
{
    OutputDebugString(L"SarAsioWrapper::canSampleRate");
    return 0;
}

long SarAsioWrapper::getSampleRate(double *sampleRate)
{
    OutputDebugString(L"SarAsioWrapper::getSampleRate");
    return 0;
}

long SarAsioWrapper::setSampleRate(double sampleRate)
{
    OutputDebugString(L"SarAsioWrapper::setSampleRate");
    return 0;
}

long SarAsioWrapper::getClockSources(ClockSource *clocks, long *count)
{
    OutputDebugString(L"SarAsioWrapper::getClockSources");
    return 0;
}

long SarAsioWrapper::setClockSource(long index)
{
    OutputDebugString(L"SarAsioWrapper::setClockSource");
    return 0;
}

long SarAsioWrapper::getSamplePosition(int64_t *pos, int64_t *timestamp)
{
    OutputDebugString(L"SarAsioWrapper::getSamplePosition");
    return 0;
}

long SarAsioWrapper::getChannelInfo(ChannelInfo *info)
{
    OutputDebugString(L"SarAsioWrapper::getChannelInfo");
    return 0;
}

long SarAsioWrapper::createBuffers(
    BufferInfo *infos, long channelCount, long bufferSize,
    Callbacks *callbacks)
{
    OutputDebugString(L"SarAsioWrapper::createBuffers");
    return 0;
}

long SarAsioWrapper::disposeBuffers()
{
    OutputDebugString(L"SarAsioWrapper::disposeBuffers");
    return 0;
}

long SarAsioWrapper::controlPanel()
{
    OutputDebugString(L"SarAsioWrapper::controlPanel");
    auto sheet = std::make_shared<ConfigurationPropertyDialog>();

    sheet->show(_hwnd);
    return 0;
}

long SarAsioWrapper::future(long selector, void *opt)
{
    OutputDebugString(L"SarAsioWrapper::future");
    return 0;
}

long SarAsioWrapper::outputReady()
{
    OutputDebugString(L"SarAsioWrapper::outputReady");
    return 0;
}
