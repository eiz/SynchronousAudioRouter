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

#ifndef _SAR_ASIO_MMWRAPPER_H
#define _SAR_ASIO_MMWRAPPER_H

#include "config.h"
#include "ActivateAudioInterfaceWorker_h.h"

namespace Sar {

extern const PROPERTYKEY PKEY_SynchronousAudioRouter_EndpointId;

struct ATL_NO_VTABLE SarMMDeviceEnumerator:
    public CComObjectRootEx<CComMultiThreadModel>,
    public CComCoClass<SarMMDeviceEnumerator, &__uuidof(MMDeviceEnumerator)>,
    public IMMDeviceEnumerator
{
    BEGIN_COM_MAP(SarMMDeviceEnumerator)
        COM_INTERFACE_ENTRY(IMMDeviceEnumerator)
        COM_INTERFACE_ENTRY_AGGREGATE_BLIND(_innerEnumerator)
    END_COM_MAP()

    DECLARE_REGISTRY_RESOURCEID(IDR_SARMMDEVICE)

    SarMMDeviceEnumerator();
    virtual ~SarMMDeviceEnumerator();

    virtual HRESULT STDMETHODCALLTYPE EnumAudioEndpoints(
        _In_ EDataFlow dataFlow,
        _In_ DWORD dwStateMask,
        _Out_ IMMDeviceCollection **ppDevices) override;
    virtual HRESULT STDMETHODCALLTYPE GetDefaultAudioEndpoint(
        _In_ EDataFlow dataFlow,
        _In_ ERole role,
        _Out_ IMMDevice **ppEndpoint) override;
    virtual HRESULT STDMETHODCALLTYPE GetDevice(
        _In_ LPCWSTR pwstrId,
        _Out_ IMMDevice **ppDevice) override;
    virtual HRESULT STDMETHODCALLTYPE RegisterEndpointNotificationCallback(
        _In_ IMMNotificationClient *pClient) override;
    virtual HRESULT STDMETHODCALLTYPE UnregisterEndpointNotificationCallback(
        _In_ IMMNotificationClient *pClient) override;

private:
    DriverConfig _config;
    CComPtr<IMMDeviceEnumerator> _innerEnumerator;
};

struct DECLSPEC_UUID("7A3A9E4A-6212-42C8-BE71-5772D95CCD8E") ATL_NO_VTABLE
    ISarMMDeviceCollectionInit: public IUnknown
{
    virtual HRESULT Initialize(
        const std::vector<CComPtr<IMMDevice>> &items) = 0;
};

struct DECLSPEC_UUID("A3A81B80-F89F-4806-A2AF-0E8CC2E8F8E5") ATL_NO_VTABLE
    SarMMDeviceCollection:
        public CComObjectRootEx<CComMultiThreadModel>,
        public CComCoClass<SarMMDeviceCollection>,
        public IMMDeviceCollection,
        public ISarMMDeviceCollectionInit
{
    BEGIN_COM_MAP(SarMMDeviceCollection)
        COM_INTERFACE_ENTRY(IMMDeviceCollection)
        COM_INTERFACE_ENTRY(ISarMMDeviceCollectionInit)
    END_COM_MAP()

    DECLARE_NO_REGISTRY()

    SarMMDeviceCollection() {}
    virtual ~SarMMDeviceCollection() {}

    virtual HRESULT STDMETHODCALLTYPE GetCount(
        _Out_ UINT *pcDevices) override;
    virtual HRESULT STDMETHODCALLTYPE Item(
        _In_ UINT nDevice, _Out_ IMMDevice **ppDevice) override;
    virtual HRESULT Initialize(
        const std::vector<CComPtr<IMMDevice>> &items) override;

private:
    std::vector<CComPtr<IMMDevice>> _items;
};

struct DECLSPEC_UUID("E2F7A62A-862B-40AE-BBC2-5C0CA9A5B7E1") ATL_NO_VTABLE
    SarActivateAudioInterfaceWorker:
        public CComObjectRootEx<CComMultiThreadModel>,
        public CComCoClass<SarActivateAudioInterfaceWorker>,
        public IActivateAudioInterfaceWorker
{
    BEGIN_COM_MAP(SarActivateAudioInterfaceWorker)
        COM_INTERFACE_ENTRY(IActivateAudioInterfaceWorker)
    END_COM_MAP()

    DECLARE_NO_REGISTRY()

    virtual HRESULT STDMETHODCALLTYPE Initialize(
        LPCWSTR deviceInterfacePath,
        REFIID riid,
        PROPVARIANT *activationParams,
        IActivateAudioInterfaceCompletionHandler *completionHandler,
        UINT threadId) override;
};

OBJECT_ENTRY_AUTO(__uuidof(MMDeviceEnumerator), SarMMDeviceEnumerator)
OBJECT_ENTRY_AUTO(
    __uuidof(SarActivateAudioInterfaceWorker), SarActivateAudioInterfaceWorker)
OBJECT_ENTRY_NON_CREATEABLE_EX_AUTO(
    __uuidof(SarMMDeviceCollection), SarMMDeviceCollection)

} // namespace Sar

#endif // _SAR_ASIO_MMWRAPPER_H