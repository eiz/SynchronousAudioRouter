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

#include "IASIO.h"

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

    virtual bool init(void *sysHandle) override;
    virtual void getDriverName(char name[32]) override;
    virtual long getDriverVersion() override;
    virtual void getErrorMessage(char str[124]) override;
    virtual long start() override;
    virtual long stop() override;
    virtual long getChannels(
        long *inputCount, long *outputCount) override;
    virtual long getLatencies(
        long *inputLatency, long *outputLatency) override;
    virtual long getBufferSize(
        long *minSize, long *maxSize,
        long *preferredSize, long *granularity) override;
    virtual long canSampleRate(double sampleRate) override;
    virtual long getSampleRate(double *sampleRate) override;
    virtual long setSampleRate(double sampleRate) override;
    virtual long getClockSources(
        ClockSource *clocks, long *count) override;
    virtual long setClockSource(long index) override;
    virtual long getSamplePosition(int64_t *pos, int64_t *timestamp) override;
    virtual long getChannelInfo(ChannelInfo *info) override;
    virtual long createBuffers(
        BufferInfo *infos, long channelCount, long bufferSize,
        Callbacks *callbacks) override;
    virtual long disposeBuffers() override;
    virtual long controlPanel() override;
    virtual long future(long selector, void *opt) override;
    virtual long outputReady() override;

private:
    HWND _hwnd;
};

OBJECT_ENTRY_AUTO(CLSID_SarAsioWrapper, SarAsioWrapper)

} // namespace Sar
#endif // _SAR_ASIO_WRAPPER_H
