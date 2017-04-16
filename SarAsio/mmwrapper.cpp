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
#include "utility.h"
#include <DbgHelp.h>
#pragma comment(lib, "dbghelp.lib")

namespace Sar {

typedef HRESULT STDAPICALLTYPE DllGetClassObjectFn(
    REFCLSID rclsid, REFIID riid, LPVOID *ppv);

#define MMDEVAPI_PATH "%SystemRoot%\\System32\\MMDevApi.dll"

const PROPERTYKEY PKEY_SynchronousAudioRouter_EndpointId =
{
    { 0xf4b15b6f, 0x8c3f, 0x48b6,
      { 0xa1, 0x15, 0x42, 0xfd, 0xe1, 0x9e, 0xf0, 0x5b } },
    0
};

SarMMDeviceEnumerator::SarMMDeviceEnumerator()
{
    LOG(INFO) << "Initializing SarMMDeviceEnumerator.";

    char buf[256] = {};
    DllGetClassObjectFn *fn_DllGetClassObject;
    CComPtr<IClassFactory> cf;
    HMODULE lib, dummy;
    HRESULT hr;

    _config = DriverConfig::fromFile(ConfigurationPath("default.json"));

    // Our registry filter has replaced the registration for the
    // MMDeviceEnumerator with our wrapper object, but we need to instantiate
    // the original coclass. Unfortunately, we can't just bypass the registry
    // filter to get the original registration back, as CoCreateInstance uses
    // an internal class cache and won't re-load the registration. So we hack
    // job it and instantiate the object directly using the DLL's
    // DllGetClassObject export. This should be mostly safe since
    // MMDeviceEnumerator has a threading model of 'both', so proxies should
    // never be needed.
    if (!ExpandEnvironmentStringsA(MMDEVAPI_PATH, buf, sizeof(buf))) {
        LOG(ERROR) << "Failed to get MMDEVAPI_PATH";
        return;
    }

    lib = LoadLibraryA(buf);

    if (!lib) {
        LOG(ERROR) << "Failed to load MMDevAPI";
        return;
    }

    fn_DllGetClassObject =
        (DllGetClassObjectFn *)GetProcAddress(lib, "DllGetClassObject");

    if (!fn_DllGetClassObject) {
        LOG(ERROR) << "Failed to get DllClassObject from MMDevAPI";
        FreeLibrary(lib);
        return;
    }

    // Because of the nasty way we're loading the underlying COM object outside
    // of CoCreateInstance, we need to make sure that both our dll and the
    // MMDevAPI dll never unload for the lifetime of the process, so we pin them
    // here.
    GetModuleHandleEx(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
        (LPCTSTR)fn_DllGetClassObject, &dummy);

    GetModuleHandleEx(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
        (LPCTSTR)DllGetClassObject, &dummy);

    // It probably doesn't matter since we've pinned the library anyway, but
    // balance the LoadLibraryA call above.
    FreeLibrary(lib);

    if (!SUCCEEDED(fn_DllGetClassObject(
        __uuidof(MMDeviceEnumerator), IID_IClassFactory, (LPVOID *)&cf))) {

        LOG(ERROR) << "Failed to instantiate MMDeviceEnumerator";
        return;
    }

    hr = cf->CreateInstance(
        0, __uuidof(IMMDeviceEnumerator), (LPVOID *)&_innerEnumerator);

    if (!SUCCEEDED(hr)) {
        LOG(ERROR) << "Failed to instantiate MMDeviceEnumerator";
    }

    LOG(INFO) << "Initialized SarMMDeviceEnumerator.";
}

SarMMDeviceEnumerator::~SarMMDeviceEnumerator()
{
    _innerEnumerator = nullptr;
}

HRESULT STDMETHODCALLTYPE SarMMDeviceEnumerator::EnumAudioEndpoints(
    _In_ EDataFlow dataFlow,
    _In_ DWORD dwStateMask,
    _Out_ IMMDeviceCollection **ppDevices)
{
    if (!_innerEnumerator) {
        return E_FAIL;
    }

    LOG(INFO) << "SarMMDeviceEnumerator::EnumAudioEndpoints";

    CComPtr<IMMDeviceCollection> innerCollection;
    CComPtr<IMMDeviceCollection> outerCollection;
    std::vector<CComPtr<IMMDevice>> devices;
    auto hr = _innerEnumerator->EnumAudioEndpoints(
        dataFlow, dwStateMask, &innerCollection);

    if (!SUCCEEDED(hr)) {
        return hr;
    }

    UINT numDevices;
    hr = innerCollection->GetCount(&numDevices);

    if (!SUCCEEDED(hr)) {
        return hr;
    }

    for (UINT i = 0; i < numDevices; ++i) {
        CComPtr<IMMDevice> device;

        hr = innerCollection->Item(i, &device);

        if (!SUCCEEDED(hr)) {
            continue;
        }

        devices.emplace_back(device);
    }

    hr = SarMMDeviceCollection::CreateInstance(&outerCollection);

    if (!SUCCEEDED(hr)) {
        return hr;
    }

    CComQIPtr<ISarMMDeviceCollectionInit> ocInit(outerCollection);
    ocInit->Initialize(devices);
    return outerCollection.CopyTo(ppDevices);
}

HRESULT STDMETHODCALLTYPE SarMMDeviceEnumerator::GetDefaultAudioEndpoint(
    _In_ EDataFlow dataFlow,
    _In_ ERole role,
    _Out_ IMMDevice **ppEndpoint)
{
    LOG(INFO) << "SarMMDeviceEnumerator::GetDefaultAudioEndpoint";

    if (!_innerEnumerator) {
        return E_FAIL;
    }

    if (!_config.enableApplicationRouting) {
        return _innerEnumerator->GetDefaultAudioEndpoint(
            dataFlow, role, ppEndpoint);
    }

    CComPtr<IMMDeviceCollection> devices;
    CComPtr<IMMDevice> foundDevice;
    UINT deviceCount;
    WCHAR processNameWide[512] = {};
    ApplicationConfig *appConfig = nullptr;

    GetModuleFileName(nullptr, processNameWide, sizeof(processNameWide));

    auto processName = TCHARToUTF8(processNameWide);

    for (auto& candidateApp : _config.applications) {
        if (std::regex_search(processName, candidateApp.pattern)) {
            appConfig = &candidateApp;
            break;
        }
    }

    if (!appConfig) {
        return _innerEnumerator->GetDefaultAudioEndpoint(
                dataFlow, role, ppEndpoint);
    }

    if (!SUCCEEDED(_innerEnumerator->EnumAudioEndpoints(
        dataFlow, DEVICE_STATE_ACTIVE, &devices))) {

        return E_FAIL;
    }

    if (!SUCCEEDED(devices->GetCount(&deviceCount))) {
        return E_FAIL;
    }

    for (UINT i = 0; i < deviceCount && !foundDevice; ++i) {
        CComPtr<IMMDevice> device;
        CComPtr<IPropertyStore> ps;
        PROPVARIANT pvalue = {};

        if (!SUCCEEDED(devices->Item(i, &device))) {
            continue;
        }

        if (!SUCCEEDED(device->OpenPropertyStore(STGM_READ, &ps))) {
            continue;
        }

        if (!SUCCEEDED(ps->GetValue(
            PKEY_SynchronousAudioRouter_EndpointId, &pvalue))) {

            continue;
        }

        if (pvalue.vt == VT_LPWSTR) {
            auto candidateId = TCHARToUTF8(pvalue.pwszVal);

            for (auto& defaultEndpoint : appConfig->defaults) {
                if (role == defaultEndpoint.role &&
                    dataFlow == defaultEndpoint.type &&
                    candidateId == defaultEndpoint.id) {

                    foundDevice = device;
                    break;
                }
            }
        }

        PropVariantClear(&pvalue);
    }

    if (foundDevice) {
        *ppEndpoint = foundDevice.Detach();
        return S_OK;
    }

    return _innerEnumerator->GetDefaultAudioEndpoint(
        dataFlow, role, ppEndpoint);
}

HRESULT STDMETHODCALLTYPE SarMMDeviceEnumerator::GetDevice(
    _In_ LPCWSTR pwstrId,
    _Out_ IMMDevice **ppDevice)
{
    if (!_innerEnumerator) {
        return E_FAIL;
    }

    auto deviceId = TCHARToUTF8(pwstrId);
    LOG(INFO) << "SarMMDeviceEnumerator::GetDevice("
        << deviceId << ")";

    return _innerEnumerator->GetDevice(pwstrId, ppDevice);
}

HRESULT STDMETHODCALLTYPE
SarMMDeviceEnumerator::RegisterEndpointNotificationCallback(
    _In_ IMMNotificationClient *pClient)
{
    if (!_innerEnumerator) {
        return E_FAIL;
    }

    LOG(INFO) << "SarMMDeviceEnumerator::RegisterEndpointNotificationCallback";
    return _innerEnumerator->RegisterEndpointNotificationCallback(pClient);
}

HRESULT STDMETHODCALLTYPE
SarMMDeviceEnumerator::UnregisterEndpointNotificationCallback(
    _In_ IMMNotificationClient *pClient)
{
    if (!_innerEnumerator) {
        return E_FAIL;
    }

    LOG(INFO)
        << "SarMMDeviceEnumerator::UnregisterEndpointNotificationCallback";
    return _innerEnumerator->UnregisterEndpointNotificationCallback(
        pClient);
}

HRESULT SarMMDeviceCollection::Initialize(
    const std::vector<CComPtr<IMMDevice>> &items)
{
    _items = items;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE SarMMDeviceCollection::GetCount(
    _Out_ UINT *pcDevices)
{
    if (!pcDevices) {
        return E_POINTER;
    }

    *pcDevices = (UINT)_items.size();
    return S_OK;
}

HRESULT STDMETHODCALLTYPE SarMMDeviceCollection::Item(
    _In_ UINT nDevice, _Out_ IMMDevice **ppDevice)
{
    if (nDevice >= _items.size()) {
        return E_INVALIDARG;
    }

    LOG(INFO) << "Getting enumerated device at index " << nDevice;
    return _items[nDevice].CopyTo(ppDevice);
}

HRESULT STDMETHODCALLTYPE SarActivateAudioInterfaceWorker::Initialize(
    LPCWSTR deviceInterfacePath,
    REFIID riid,
    PROPVARIANT *activationParams,
    IActivateAudioInterfaceCompletionHandler *completionHandler,
    UINT threadId)
{
    LOG(INFO) << "SarActivateAudioInterfaceWorker::Initialize";
    return S_OK;
}

} // namespace Sar