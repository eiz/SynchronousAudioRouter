// SynchronousAudioRouter
// Copyright (C) 2015 Mackenzie Straight
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 2 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with SynchronousAudioRouter.  If not, see <http://www.gnu.org/licenses/>.

#ifndef _SAR_ASIO_TINYASIO_H
#define _SAR_ASIO_TINYASIO_H

namespace Sar {

enum class AsioBool : long { False, True };

enum class AsioStatus : long
{
    OK = 0,
    Success = 0x3f4847a0,
    NotPresent = -1000,
    HardwareMalfunction,
    InvalidParameter,
    InvalidMode,
    SPNotAdvancing,
    NoClock,
    NoMemory
};

enum class AsioMessage : long
{
    SelectorSupported = 1,
    EngineVersion,
    ResetRequest,
    BufferSizeChange,
    ResyncRequest,
    LatenciesChanged,
    SupportsTimeInfo,
    SupportsTimeCode,
    MMCCommand,
    SupportsInputMonitor,
    SupportsInputGain,
    SupportsInputMeter,
    SupportsOutputGain,
    SupportsOutputMeter,
    Overload
};

enum AsioSampleType : long
{
    Int16MSB = 0,
    Int24MSB = 1,
    Int32MSB = 2,
    Float32MSB = 3,
    Float64MSB = 4,

    Int32MSB16 = 8,
    Int32MSB18 = 9,
    Int32MSB20 = 10,
    Int32MSB24 = 11,

    Int16LSB = 16,
    Int24LSB = 17,
    Int32LSB = 18,
    Float32LSB = 19,
    Float64LSB = 20,

    Int32LSB16 = 24,
    Int32LSB18 = 25,
    Int32LSB20 = 26,
    Int32LSB24 = 27,

    UnknownSampleType = 0xFFFF
};

struct AsioClockSource
{
    long index;
    long channel;
    long group;
    AsioBool isCurrentSource;
    char name[32];
};

struct AsioChannelInfo
{
    long index;
    AsioBool isInput;
    AsioBool isActive;
    long group;
    long sampleType;
    char name[32];
};

struct AsioBufferInfo
{
    AsioBool isInput;
    long index;
    void *asioBuffers[2];
};

struct AsioTimeCode
{
    double speed;
    int64_t sampleIndex;
    unsigned flags;
    char future[64];
};

struct AsioTimeInfo
{
    double speed;
    int64_t systemTime;
    int64_t samplePosition;
    double sampleRate;
    unsigned flags;
    char reserved[12];
};

struct AsioTime
{
    long reserved[4];
    AsioTimeInfo timeInfo;
    AsioTimeCode timeCode;
};

typedef void AsioTickCallback(long bufferIndex, AsioBool directProcess);
typedef AsioTime *AsioTickWithTimeCallback(
    AsioTime *time, long bufferIndex, AsioBool directProcess);

struct AsioCallbacks
{
    AsioTickCallback *tick;
    void (*sampleRateDidChange)(double sampleRate);
    long (*asioMessage)(
        AsioMessage selector, long value, void *message, double *opt);
    AsioTickWithTimeCallback *tickWithTime;
};

#define CLSID_STR_SynchronousAudioRouter \
    "{0569D852-1F6A-44A7-B7B5-EFB78B66BE21}"

#ifndef IID_STR_IASIO
#define IID_STR_IASIO CLSID_STR_SynchronousAudioRouter
#endif

struct __declspec(uuid(IID_STR_IASIO)) IASIO:
    public IUnknown
{
    virtual AsioBool init(void *sysHandle) = 0;
    virtual void getDriverName(char name[32]) = 0;
    virtual long getDriverVersion() = 0;
    virtual void getErrorMessage(char str[124]) = 0;
    virtual AsioStatus start() = 0;
    virtual AsioStatus stop() = 0;
    virtual AsioStatus getChannels(
        long *inputCount, long *outputCount) = 0;
    virtual AsioStatus getLatencies(
        long *inputLatency, long *outputLatency) = 0;
    virtual AsioStatus getBufferSize(
        long *minSize, long *maxSize,
        long *preferredSize, long *granularity) = 0;
    virtual AsioStatus canSampleRate(double sampleRate) = 0;
    virtual AsioStatus getSampleRate(double *sampleRate) = 0;
    virtual AsioStatus setSampleRate(double sampleRate) = 0;
    virtual AsioStatus getClockSources(
        AsioClockSource *clocks, long *count) = 0;
    virtual AsioStatus setClockSource(long index) = 0;
    virtual AsioStatus getSamplePosition(int64_t *pos, int64_t *timestamp) = 0;
    virtual AsioStatus getChannelInfo(AsioChannelInfo *info) = 0;
    virtual AsioStatus createBuffers(
        AsioBufferInfo *infos, long channelCount, long bufferSize,
        AsioCallbacks *callbacks) = 0;
    virtual AsioStatus disposeBuffers() = 0;
    virtual AsioStatus controlPanel() = 0;
    virtual AsioStatus future(long selector, void *opt) = 0;
    virtual AsioStatus outputReady() = 0;
};

struct AsioDriver
{
    std::string name;
    std::string clsid;
    HRESULT open(IASIO **ppAsio);
};

std::vector<AsioDriver> InstalledAsioDrivers();

} // namespace Sar
#endif // _SAR_ASIO_TINYASIO_H
