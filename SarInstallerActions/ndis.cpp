// SynchronousAudioRouter
// Copyright (C) 2017 Mackenzie Straight
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
#include <devguid.h>

#define WRITE_LOCK_TIMEOUT 5000
#define CLIENT_NAME L"SynchronousAudioRouter.msi"
#define SARNDIS_COMPONENT_ID L"SarNdis"

struct ScopedCoInitialize
{
    HRESULT hr;

    ScopedCoInitialize()
    {
        hr = CoInitialize(0);
    }

    ~ScopedCoInitialize()
    {
        if (!FAILED(hr)) {
            CoUninitialize();
        }
    }
};

struct ScopedNetCfgLock
{
    HRESULT hr;

    ScopedNetCfgLock(CComPtr<INetCfgLock>& cfg): _cfg(cfg) {
        hr = _cfg->AcquireWriteLock(WRITE_LOCK_TIMEOUT, CLIENT_NAME, NULL);
    }

    ~ScopedNetCfgLock() {
        if (!FAILED(hr)) {
            _cfg->ReleaseWriteLock();
        }
    }
private:
    CComPtr<INetCfgLock> _cfg;
};

static HRESULT DoNdisSetup(
    MSIHANDLE hInstall, HRESULT (*cb)(
        MSIHANDLE, CComPtr<INetCfg>&, CComPtr<INetCfgClassSetup>&))
{
    HRESULT hr = S_OK;

    hr = WcaInitialize(hInstall, "InstallNdisFilter");

    if (FAILED(hr)) {
        return WcaFinalize(hr);
    }

    ScopedCoInitialize sci;

    if (FAILED(sci.hr)) {
        WcaLogError(hr, "Failed to initialize COM.");
        return WcaFinalize(hr);
    }

    CComPtr<INetCfg> netCfg;

    hr = CoCreateInstance(
        CLSID_CNetCfg, 0, CLSCTX_INPROC_SERVER, IID_INetCfg, (LPVOID *)&netCfg);

    if (FAILED(hr)) {
        WcaLogError(hr, "Failed to access INetCfg interface.");
        return WcaFinalize(hr);
    }

    CComQIPtr<INetCfgLock> netCfgLock(netCfg);

    if (!netCfgLock) {
        WcaLog(LOGMSG_STANDARD, "Failed to get INetCfgLock interface.");
        return WcaFinalize(hr);
    }

    ScopedNetCfgLock sncl(netCfgLock);

    if (FAILED(sncl.hr)) {
        WcaLog(LOGMSG_STANDARD, "Failed to acquire INetCfg write lock.");
        return WcaFinalize(hr);
    }

    hr = netCfg->Initialize(NULL);

    if (FAILED(hr)) {
        WcaLog(LOGMSG_STANDARD, "Failed to initialize INetCfg.");
        return WcaFinalize(hr);
    }

    CComPtr<INetCfgClassSetup> setup;
    GUID devGuid = GUID_DEVCLASS_NETSERVICE;

    hr = netCfg->QueryNetCfgClass(
        &devGuid, IID_INetCfgClassSetup, (PVOID *)&setup);

    if (FAILED(hr)) {
        WcaLog(LOGMSG_STANDARD, "Failed to get INetCfgClassSetup interface.");
        return WcaFinalize(hr);
    }

    return WcaFinalize(cb(hInstall, netCfg, setup));
}

static HRESULT DoInstall(
    MSIHANDLE hInstall,
    CComPtr<INetCfg> &cfg,
    CComPtr<INetCfgClassSetup>& setup)
{
    UNREFERENCED_PARAMETER(cfg);
    HRESULT hr;
    OBO_TOKEN oboToken = {};
    CComPtr<INetCfgComponent> component;

    oboToken.Type = OBO_USER;
    hr = setup->Install(
        SARNDIS_COMPONENT_ID, &oboToken, 0, 0, NULL, NULL, &component);

    if (FAILED(hr)) {
        WcaLogError(hr, "Failed to install NDIS filter component.");
        return hr;
    }

    if (hr == NETCFG_S_REBOOT) {
        if (MsiSetMode(hInstall, MSIRUNMODE_REBOOTATEND, TRUE)) {
            WcaLog(LOGMSG_VERBOSE, "Failed to schedule reboot.");
        }
    }

    return S_OK;
}

static HRESULT DoUninstall(
    MSIHANDLE hInstall,
    CComPtr<INetCfg>& cfg,
    CComPtr<INetCfgClassSetup>& setup)
{
    HRESULT hr;
    OBO_TOKEN oboToken = {};
    CComPtr<INetCfgComponent> component;

    hr = cfg->FindComponent(SARNDIS_COMPONENT_ID, &component);

    if (FAILED(hr)) {
        WcaLogError(hr,
            "NDIS filter component is not installed, not uninstalling.");
        return S_OK;
    }

    oboToken.Type = OBO_USER;
    hr = setup->DeInstall(component, &oboToken, NULL);

    if (FAILED(hr)) {
        WcaLogError(hr, "Failed to uninstall NDIS filter component.");
        return hr;
    }

    if (hr == NETCFG_S_REBOOT) {
        if (MsiSetMode(hInstall, MSIRUNMODE_REBOOTATEND, TRUE)) {
            WcaLog(LOGMSG_VERBOSE, "Failed to schedule reboot.");
        }
    }

    return S_OK;
}

UINT __stdcall InstallNdisFilter(MSIHANDLE hInstall)
{
    return DoNdisSetup(hInstall, DoInstall);
}

UINT __stdcall UninstallNdisFilter(MSIHANDLE hInstall)
{
    return DoNdisSetup(hInstall, DoUninstall);
}