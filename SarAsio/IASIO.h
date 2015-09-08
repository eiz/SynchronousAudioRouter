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

#ifndef _SAR_ASIO_IASIO_H
#define _SAR_ASIO_IASIO_H

typedef struct ClockSource
{
    long index;
    long channel;
    long group;
    bool isCurrentSource;
    char name[32];
} ClockSource;

typedef struct ChannelInfo
{
    long index;
    bool isInput;
    bool isActive;
    long group;
    long sampleType;
    char name[32];
} ChannelInfo;

typedef struct BufferInfo
{
} BufferInfo;

typedef struct Callbacks
{
} Callbacks;

struct __declspec(uuid("{0569D852-1F6A-44A7-B7B5-EFB78B66BE21}")) IASIO:
    public IUnknown
{
    virtual bool init(void *sysHandle) = 0;
    virtual void getDriverName(char name[32]) = 0;
    virtual long getDriverVersion() = 0;
    virtual void getErrorMessage(char str[124]) = 0;
    virtual long start() = 0;
    virtual long stop() = 0;
    virtual long getChannels(
        long *inputCount, long *outputCount) = 0;
    virtual long getLatencies(
        long *inputLatency, long *outputLatency) = 0;
    virtual long getBufferSize(
        long *minSize, long *maxSize, long *preferredSize, long *granularity) = 0;
    virtual long canSampleRate(double sampleRate) = 0;
    virtual long getSampleRate(double *sampleRate) = 0;
    virtual long setSampleRate(double sampleRate) = 0;
    virtual long getClockSources(
        ClockSource *clocks, long *count) = 0;
    virtual long setClockSource(long index) = 0;
    virtual long getSamplePosition(int64_t *pos, int64_t *timestamp) = 0;
    virtual long getChannelInfo(ChannelInfo *info) = 0;
    virtual long createBuffers(
        BufferInfo *infos, long channelCount, long bufferSize,
        Callbacks *callbacks) = 0;
    virtual long disposeBuffers() = 0;
    virtual long controlPanel() = 0;
    virtual long future(long selector, void *opt) = 0;
    virtual long outputReady() = 0;
};

#endif // _SAR_ASIO_IASIO_H
