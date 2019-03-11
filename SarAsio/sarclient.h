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

#ifndef _SAR_ASIO_SARCLIENT_H
#define _SAR_ASIO_SARCLIENT_H

#include "config.h"
#include "sar.h"

namespace Sar {

struct BufferConfig
{
    int periodFrameSize;
    int sampleRate;
    int sampleSize;

    std::array<std::vector<std::vector<void *>>, 2> asioBuffers;
};

struct SarClient: public std::enable_shared_from_this<SarClient>
{
    SarClient(
        const DriverConfig& driverConfig,
        const BufferConfig& bufferConfig);
    void tick(long bufferIndex);
    bool start();
    void stop();
    void updateSampleRateOnTick()
    {
        _updateSampleRateOnTick = true;
    }

private:
    struct NotificationHandle
    {
        NotificationHandle(): generation(0), handle(nullptr) {}
        ~NotificationHandle()
        {
            if (handle) {
                CloseHandle(handle);
            }
        }

        ULONG generation;
        HANDLE handle;
    };

    struct HandleQueueCompletion: OVERLAPPED
    {
        SarHandleQueueResponse responses[32];
    };

    struct ATL_NO_VTABLE NotificationClient:
        public CComObjectRootEx<CComMultiThreadModel>,
        public IMMNotificationClient
    {
        BEGIN_COM_MAP(NotificationClient)
            COM_INTERFACE_ENTRY(IMMNotificationClient)
        END_COM_MAP()

        DECLARE_NO_REGISTRY()

        void setClient(std::weak_ptr<SarClient> client)
        {
            _client = client;
        }

        virtual HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(
            _In_  LPCWSTR pwstrDeviceId,
            _In_  DWORD dwNewState) override;
        virtual HRESULT STDMETHODCALLTYPE OnDeviceAdded(
            _In_  LPCWSTR pwstrDeviceId) override;
        virtual HRESULT STDMETHODCALLTYPE OnDeviceRemoved(
            _In_  LPCWSTR pwstrDeviceId) override;
        virtual HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(
            _In_  EDataFlow flow,
            _In_  ERole role,
            _In_  LPCWSTR pwstrDefaultDeviceId) override;
        virtual HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(
            _In_  LPCWSTR pwstrDeviceId,
            _In_  const PROPERTYKEY key) override;

    private:
        std::weak_ptr<SarClient> _client;
    };

    bool openControlDevice();
    bool openMmNotificationClient();
    bool setBufferLayout();
    bool createEndpoints();
    bool enableRegistryFilter();
    void updateNotificationHandles();
    void processNotificationHandleUpdates(int updateCount);

    void demux(
        void *muxBufferFirst, size_t firstSize,
        void *muxBufferSecond, size_t secondSize,
        void **targetBuffers, int ntargets, int nsources,
        size_t targetSize, int sampleSize);
    void mux(
        void *muxBufferFirst, size_t firstSize,
        void *muxBufferSecond, size_t secondSize,
        void **targetBuffers, int ntargets, int nsources,
        size_t targetSize, int sampleSize);

    DriverConfig _driverConfig;
    BufferConfig _bufferConfig;
    std::vector<NotificationHandle> _notificationHandles;
    HANDLE _device;
    HANDLE _completionPort;
    void *_sharedBuffer;
    DWORD _sharedBufferSize;
    volatile SarEndpointRegisters *_registers;
    HandleQueueCompletion _handleQueueCompletion;
    bool _handleQueueStarted;
    CComPtr<IMMDeviceEnumerator> _mmEnumerator;
    CComObject<NotificationClient> *_mmNotificationClient = nullptr;
    bool _mmNotificationClientRegistered = false;
    std::atomic<bool> _updateSampleRateOnTick = false;
};

} // namespace Sar

#endif // _SAR_ASIO_SARCLIENT_H
