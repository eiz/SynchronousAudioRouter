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
#include "wrapper.h"

bool SarAsioWrapper::init(void *sysHandle)
{
    return false;
}

void SarAsioWrapper::getDriverName(char *name)
{
}

long SarAsioWrapper::getDriverVersion()
{
    return 0;
}

void SarAsioWrapper::getErrorMessage(char *str)
{
}

long SarAsioWrapper::start()
{
    return 0;
}

long SarAsioWrapper::stop()
{
    return 0;
}

long SarAsioWrapper::getChannels(long *inputCount, long *outputCount)
{
    return 0;
}

long SarAsioWrapper::getLatencies(long *inputLatency, long *outputLatency)
{
    return 0;
}

long SarAsioWrapper::getBufferSize(
    long *minSize, long *maxSize, long *preferredSize, long *granularity)
{
    return 0;
}

long SarAsioWrapper::canSampleRate(double sampleRate)
{
    return 0;
}

long SarAsioWrapper::getSampleRate(double *sampleRate)
{
    return 0;
}

long SarAsioWrapper::setSampleRate(double sampleRate)
{
    return 0;
}

long SarAsioWrapper::getClockSources(ClockSource *clocks, long *count)
{
    return 0;
}

long SarAsioWrapper::setClockSource(long index)
{
    return 0;
}

long SarAsioWrapper::getSamplePosition(int64_t *pos, int64_t *timestamp)
{
    return 0;
}

long SarAsioWrapper::getChannelInfo(ChannelInfo *info)
{
    return 0;
}

long SarAsioWrapper::createBuffers(
    BufferInfo *infos, long channelCount, long bufferSize,
    Callbacks *callbacks)
{
    return 0;
}

long SarAsioWrapper::disposeBuffers()
{
    return 0;
}

long SarAsioWrapper::controlPanel()
{
    return 0;
}

long SarAsioWrapper::future(long selector, void *opt)
{
    return 0;
}

long SarAsioWrapper::outputReady()
{
    return 0;
}
