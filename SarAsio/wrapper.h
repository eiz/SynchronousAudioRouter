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

#ifndef _SAR_ASIO_WRAPPER_H
#define _SAR_ASIO_WRAPPER_H

#include "config.h"
#include "sarclient.h"
#include "tinyasio.h"

namespace Sar {

// {0569D852-1F6A-44A7-B7B5-EFB78B66BE21}
DEFINE_GUID(CLSID_SarAsioWrapper,
    0x569d852, 0x1f6a, 0x44a7, 0xb7, 0xb5, 0xef, 0xb7, 0x8b, 0x66, 0xbe, 0x21);

struct ATL_NO_VTABLE SarAsioWrapper:
    public CComObjectRootEx<CComSingleThreadModel>,
    public CComCoClass<SarAsioWrapper, &CLSID_SarAsioWrapper>,
    public IASIO
{
    BEGIN_COM_MAP(SarAsioWrapper)
        COM_INTERFACE_ENTRY(IASIO)
    END_COM_MAP();

    DECLARE_REGISTRY_RESOURCEID(IDR_SARASIO)

    SarAsioWrapper();

    virtual AsioBool init(void *sysHandle) override;
    virtual void getDriverName(char name[32]) override;
    virtual long getDriverVersion() override;
    virtual void getErrorMessage(char str[124]) override;
    virtual AsioStatus start() override;
    virtual AsioStatus stop() override;
    virtual AsioStatus getChannels(
        long *inputCount, long *outputCount) override;
    virtual AsioStatus getLatencies(
        long *inputLatency, long *outputLatency) override;
    virtual AsioStatus getBufferSize(
        long *minSize, long *maxSize,
        long *preferredSize, long *granularity) override;
    virtual AsioStatus canSampleRate(double sampleRate) override;
    virtual AsioStatus getSampleRate(double *sampleRate) override;
    virtual AsioStatus setSampleRate(double sampleRate) override;
    virtual AsioStatus getClockSources(
        AsioClockSource *clocks, long *count) override;
    virtual AsioStatus setClockSource(long index) override;
    virtual AsioStatus getSamplePosition(
        int64_t *pos, int64_t *timestamp) override;
    virtual AsioStatus getChannelInfo(AsioChannelInfo *info) override;
    virtual AsioStatus createBuffers(
        AsioBufferInfo *infos, long channelCount, long bufferSize,
        AsioCallbacks *callbacks) override;
    virtual AsioStatus disposeBuffers() override;
    virtual AsioStatus controlPanel() override;
    virtual AsioStatus future(long selector, void *opt) override;
    virtual AsioStatus outputReady() override;

private:
    struct VirtualChannel
    {
        VirtualChannel()
            : endpointIndex(-1), channelIndex(-1), name("")
        {
            asioBuffers[0] = nullptr;
            asioBuffers[1] = nullptr;
        }

        int endpointIndex;
        int channelIndex;
        std::string name;
        void *asioBuffers[2];
    };

    bool initInnerDriver();
    void initVirtualChannels();
    void onTick(long bufferIndex, AsioBool directProcess);
    AsioTime *onTickWithTime(
        AsioTime *time, long bufferIndex, AsioBool directProcess);
    static void onTickStub(long bufferIndex, AsioBool directProcess);
    static AsioTime *onTickWithTimeStub(
        AsioTime *time, long bufferIndex, AsioBool directProcess);

    HWND _hwnd;
    DriverConfig _config;
    BufferConfig _bufferConfig;
    std::shared_ptr<SarClient> _sar;
    CComPtr<IASIO> _innerDriver;
    std::vector<VirtualChannel> _virtualInputs;
    std::vector<VirtualChannel> _virtualOutputs;
    AsioTickCallback *_userTick;
    AsioTickWithTimeCallback *_userTickWithTime;
    AsioCallbacks _callbacks = {};
};

OBJECT_ENTRY_AUTO(CLSID_SarAsioWrapper, SarAsioWrapper)

} // namespace Sar
#endif // _SAR_ASIO_WRAPPER_H
