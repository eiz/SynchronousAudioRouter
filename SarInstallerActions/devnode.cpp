// SynchronousAudioRouter
// Copyright (C) 2015, 2017 Mackenzie Straight
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
#include <tchar.h>
#include <initguid.h>
#include <devpkey.h>
#include <newdev.h>

// These values must match the INF
#define HARDWARE_ID L"SW\\{0BCFFA5C-E754-48CF-A783-A64C0DC0BB2C}\0"
#define CLASS_GUID L"{4d36e96c-e325-11ce-bfc1-08002be10318}"
#define CLASS_NAME L"MEDIA"

#define ALL_USERS_ACL L"D:P(A;;GA;;;AU)"

// Whether the device described by `data` has SAR's hardware ID.
static BOOL IsSarDevice(HDEVINFO deviceInfoSet, SP_DEVINFO_DATA *data)
{
    DWORD requiredSize = 0;
    DEVPROPTYPE propType;

    SetLastError(0);
    SetupDiGetDeviceProperty(
        deviceInfoSet, data, &DEVPKEY_Device_HardwareIds,
        &propType, nullptr, 0, &requiredSize, 0);

    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || requiredSize < sizeof(WCHAR)) {
        return FALSE;
    }

    WCHAR *buffer = (WCHAR *)malloc(requiredSize);

    if (!buffer) {
        return FALSE;
    }

    BOOL found = FALSE;

    // Make sure the buffer is null terminated to avoid an overrun. It is a
    // REG_MULTI_SZ: strings separated by one null, ended by two.
    if (SetupDiGetDeviceProperty(
            deviceInfoSet, data, &DEVPKEY_Device_HardwareIds,
            &propType, (BYTE *)buffer, requiredSize, &requiredSize, 0) &&
        buffer[requiredSize / sizeof(WCHAR) - 1] == L'\0') {

        for (WCHAR *p = buffer;
            p < buffer + requiredSize / sizeof(WCHAR) && *p;
            p += lstrlen(p) + 1) {

            if (!lstrcmpi(p, HARDWARE_ID)) {
                found = TRUE;
                break;
            }
        }
    }

    free(buffer);
    return found;
}

// Whether a SAR device node exists already.
//
// Every device is looked at, not just those of SAR's class: a device has no
// class until a driver is installed on it. That is the state it is in when
// RemoveDeviceNode runs as the rollback of a failed install, after the driver
// package has been taken away again, the state a device left behind by an
// earlier failed install is in, and the state an upgrade leaves the device
// in between taking the old driver package away and installing the new one.
static BOOL SarDeviceExists()
{
    HDEVINFO deviceInfoSet = SetupDiGetClassDevsEx(
        nullptr, nullptr, nullptr, DIGCF_ALLCLASSES,
        nullptr, nullptr, nullptr);

    if (deviceInfoSet == INVALID_HANDLE_VALUE) {
        return FALSE;
    }

    SP_DEVINFO_DATA data = {};
    BOOL found = FALSE;

    data.cbSize = sizeof(SP_DEVINFO_DATA);

    for (int i = 0; !found && SetupDiEnumDeviceInfo(deviceInfoSet, i, &data); ++i) {
        found = IsSarDevice(deviceInfoSet, &data);
    }

    SetupDiDestroyDeviceInfoList(deviceInfoSet);
    return found;
}

UINT __stdcall CreateDeviceNode(MSIHANDLE hInstall)
{
    HRESULT hr = S_OK;
    UINT er = ERROR_SUCCESS;
    GUID classGuid;
    HDEVINFO deviceInfoSet = INVALID_HANDLE_VALUE;
    SP_DEVINFO_DATA deviceInfoData = {};
    WCHAR *hardwareId = HARDWARE_ID;
    TCHAR requireAdminBuf[32] = {};
    // A count of characters, not bytes.
    DWORD requireAdminBufSz = ARRAYSIZE(requireAdminBuf);

    hr = WcaInitialize(hInstall, "CreateDeviceNode");

    if (FAILED(hr)) {
        return WcaFinalize(hr);
    }

    // An upgrade leaves the device node of the version it replaces in place
    // (RemoveDeviceNode is not run for it), so that Windows sees the same
    // device with a new driver and not a new device. A second node would be
    // a second SAR device.
    if (SarDeviceExists()) {
        WcaLog(LOGMSG_STANDARD, "A SAR device node exists already; using it.");
        return WcaFinalize(S_OK);
    }

    IIDFromString(CLASS_GUID, &classGuid);

    deviceInfoSet = SetupDiCreateDeviceInfoList(&classGuid, nullptr);

    if (deviceInfoSet == INVALID_HANDLE_VALUE) {
        WcaLog(LOGMSG_VERBOSE, "Device info list initialization failed.");
        return WcaFinalize(ERROR_INSTALL_FAILURE);
    }

    deviceInfoData.cbSize = sizeof(SP_DEVINFO_DATA);

    if (!SetupDiCreateDeviceInfo(deviceInfoSet, CLASS_NAME, &classGuid,
        nullptr, 0, DICD_GENERATE_ID, &deviceInfoData)) {

        WcaLog(LOGMSG_VERBOSE, "Device info data initialization failed.");
        SetupDiDestroyDeviceInfoList(deviceInfoSet);
        return WcaFinalize(ERROR_INSTALL_FAILURE);
    }

    if (!SetupDiSetDeviceRegistryProperty(deviceInfoSet, &deviceInfoData,
        SPDRP_HARDWAREID, (BYTE *)hardwareId,
        (lstrlen(hardwareId) + 2) * sizeof(WCHAR))) {

        WcaLog(LOGMSG_VERBOSE, "Set SPDRP_HARDWAREID failed.");
        SetupDiDestroyDeviceInfoList(deviceInfoSet);
        return WcaFinalize(ERROR_INSTALL_FAILURE);
    }

    if (!MsiGetProperty(
        hInstall, TEXT("CustomActionData"),
        requireAdminBuf, &requireAdminBufSz)) {

        WcaLog(LOGMSG_STANDARD, "REQUIREADMINUSER = %S", requireAdminBuf);

        if (!_tcscmp(requireAdminBuf, TEXT("Any")) &&
            !SetupDiSetDeviceRegistryProperty(deviceInfoSet, &deviceInfoData,
                SPDRP_SECURITY_SDS, (BYTE *)ALL_USERS_ACL,
                (lstrlen(ALL_USERS_ACL) + 1) * sizeof(TCHAR))) {

            WcaLog(LOGMSG_VERBOSE, "Set SPDRP_SECURITY_SDS failed.");
            SetupDiDestroyDeviceInfoList(deviceInfoSet);
            return WcaFinalize(ERROR_INSTALL_FAILURE);
        }
    }

    if (!SetupDiCallClassInstaller(
        DIF_REGISTERDEVICE, deviceInfoSet, &deviceInfoData)) {

        WcaLog(LOGMSG_VERBOSE, "DIF_REGISTERDEVICE failed.");
        SetupDiDestroyDeviceInfoList(deviceInfoSet);
        return WcaFinalize(ERROR_INSTALL_FAILURE);
    }

    SetupDiDestroyDeviceInfoList(deviceInfoSet);
    return WcaFinalize(S_OK);
}

UINT __stdcall RemoveDeviceNode(MSIHANDLE hInstall)
{
    HRESULT hr = S_OK;
    UINT er = ERROR_SUCCESS;
    HDEVINFO deviceInfoSet;
    SP_DEVINFO_DATA data;

    hr = WcaInitialize(hInstall, "RemoveDeviceNode");

    if (FAILED(hr)) {
        return WcaFinalize(ERROR_INSTALL_FAILURE);
    }

    // Every device, not just those of SAR's class: see SarDeviceExists.
    deviceInfoSet = SetupDiGetClassDevsEx(
        nullptr, nullptr, nullptr, DIGCF_ALLCLASSES,
        nullptr, nullptr, nullptr);

    if (deviceInfoSet == INVALID_HANDLE_VALUE) {
        return WcaFinalize(ERROR_INSTALL_FAILURE);
    }

    data.cbSize = sizeof(SP_DEVINFO_DATA);

    for (int i = 0; SetupDiEnumDeviceInfo(deviceInfoSet, i, &data); ++i) {
        BOOL found = IsSarDevice(deviceInfoSet, &data);

        if (found) {
            BOOL needReboot = FALSE;
            if (!DiUninstallDevice(nullptr, deviceInfoSet, &data, 0, &needReboot)) {
                WcaLog(LOGMSG_VERBOSE, "Remove device failed.");
            } else {
                if (needReboot) {
                    if (MsiSetMode(hInstall, MSIRUNMODE_REBOOTATEND, TRUE)) {
                        WcaLog(LOGMSG_VERBOSE, "Failed to schedule reboot.");
                    }
                }
                WcaLog(LOGMSG_VERBOSE, "Removed a device.");
            }
        }
    }

    SetupDiDestroyDeviceInfoList(deviceInfoSet);
    return WcaFinalize(S_OK);
}
