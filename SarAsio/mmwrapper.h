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

namespace Sar {

// {9FB96668-9EDD-4574-AD77-76BD89659D5D}
DEFINE_GUID(CLSID_SarMMDeviceEnumerator,
    0x9fb96668, 0x9edd, 0x4574, 0xad, 0x77, 0x76, 0xbd, 0x89, 0x65, 0x9d, 0x5d);

struct ATL_NO_VTABLE SarMMDeviceEnumerator:
    public CComObjectRootEx<CComSingleThreadModel>,
    public CComCoClass<SarMMDeviceEnumerator, &CLSID_SarMMDeviceEnumerator>,
    public IMMDeviceEnumerator
{
    BEGIN_COM_MAP(SarMMDeviceEnumerator)
        COM_INTERFACE_ENTRY(IMMDeviceEnumerator)
    END_COM_MAP();

    DECLARE_REGISTRY_RESOURCEID(IDR_SARMMDEVICE)

    SarMMDeviceEnumerator();

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
};

OBJECT_ENTRY_AUTO(CLSID_SarMMDeviceEnumerator, SarMMDeviceEnumerator)

} // namespace Sar

#endif // _SAR_ASIO_MMWRAPPER_H