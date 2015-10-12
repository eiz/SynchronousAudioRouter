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
#include "mmwrapper.h"

namespace Sar {

SarMMDeviceEnumerator::SarMMDeviceEnumerator()
{
    OutputDebugStringA("SarMMDeviceEnumerator::SarMMDeviceEnumerator");
}

HRESULT STDMETHODCALLTYPE SarMMDeviceEnumerator::EnumAudioEndpoints(
    _In_ EDataFlow dataFlow,
    _In_ DWORD dwStateMask,
    _Out_ IMMDeviceCollection **ppDevices)
{
    OutputDebugStringA("SarMMDeviceEnumerator::EnumAudioEndpoints");
    return E_NOTIMPL;
}

HRESULT STDMETHODCALLTYPE SarMMDeviceEnumerator::GetDefaultAudioEndpoint(
    _In_ EDataFlow dataFlow,
    _In_ ERole role,
    _Out_ IMMDevice **ppEndpoint)
{
    OutputDebugStringA("SarMMDeviceEnumerator::GetDefaultAudioEndpoint");
    return E_NOTIMPL;
}

HRESULT STDMETHODCALLTYPE SarMMDeviceEnumerator::GetDevice(
    _In_ LPCWSTR pwstrId,
    _Out_ IMMDevice **ppDevice)
{
    OutputDebugStringA("SarMMDeviceEnumerator::GetDevice");
    return E_NOTIMPL;
}

HRESULT STDMETHODCALLTYPE SarMMDeviceEnumerator::RegisterEndpointNotificationCallback(
    _In_ IMMNotificationClient *pClient)
{
    OutputDebugStringA("SarMMDeviceEnumerator::RegisterEndpointNotificationCallback");
    return E_NOTIMPL;
}

HRESULT STDMETHODCALLTYPE SarMMDeviceEnumerator::UnregisterEndpointNotificationCallback(
    _In_ IMMNotificationClient *pClient)
{
    OutputDebugStringA("SarMMDeviceEnumerator::UnregisterEndpointNotificationCallback");
    return E_NOTIMPL;
}

} // namespace Sar