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
#include <devpkey.h>

// These values must match the INF
#define HARDWARE_ID L"SW\\{0BCFFA5C-E754-48CF-A783-A64C0DC0BB2C}\0"
#define CLASS_GUID L"{4d36e96c-e325-11ce-bfc1-08002be10318}"
#define CLASS_NAME L"MEDIA"

UINT __stdcall CreateDeviceNode(MSIHANDLE hInstall)
{
	HRESULT hr = S_OK;
	UINT er = ERROR_SUCCESS;
    GUID classGuid;
    HDEVINFO deviceInfoSet = INVALID_HANDLE_VALUE;
    SP_DEVINFO_DATA deviceInfoData = {};
    WCHAR *hardwareId = HARDWARE_ID;

	hr = WcaInitialize(hInstall, "CreateDeviceNode");

    if (FAILED(hr)) {
        return WcaFinalize(hr);
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
    GUID classGuid;
    HDEVINFO deviceInfoSet;
    SP_DEVINFO_DATA data;
    DWORD requiredSize;
    DEVPROPTYPE propType;

	hr = WcaInitialize(hInstall, "RemoveDeviceNode");

    if (FAILED(hr)) {
        return WcaFinalize(ERROR_INSTALL_FAILURE);
    }

    IIDFromString(CLASS_GUID, &classGuid);
    deviceInfoSet = SetupDiGetClassDevsEx(
        &classGuid, nullptr, nullptr, 0,
        nullptr, nullptr, nullptr);

    if (deviceInfoSet == INVALID_HANDLE_VALUE) {
        return WcaFinalize(ERROR_INSTALL_FAILURE);
    }

    data.cbSize = sizeof(SP_DEVINFO_DATA);

    for (int i = 0; SetupDiEnumDeviceInfo(deviceInfoSet, i, &data); ++i) {
        SetLastError(0);
        SetupDiGetDeviceProperty(
            deviceInfoSet, &data, &DEVPKEY_Device_HardwareIds,
            &propType, nullptr, 0, &requiredSize, 0);

        if (GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
            continue;
        }

        WCHAR *buffer = (WCHAR *)malloc(requiredSize);

        if (!SetupDiGetDeviceProperty(
            deviceInfoSet, &data, &DEVPKEY_Device_HardwareIds,
            &propType, (BYTE *)buffer, requiredSize, &requiredSize, 0)) {

            free(buffer);
            continue;
        }

        // Make sure buffer is null terminated to avoid overrun
        if (buffer[requiredSize / sizeof(WCHAR) - 1] != L'\0') {
            free(buffer);
            continue;
        }

        BOOL found = FALSE;

        for (WCHAR *p = buffer;
             p < p + requiredSize / sizeof(WCHAR) && *p;
             p += lstrlen(p) + 2) {

            if (!lstrcmpi(p, HARDWARE_ID)) {
                found = TRUE;
                break;
            }
        }

        free(buffer);

        if (found) {
            if (!SetupDiCallClassInstaller(DIF_REMOVE, deviceInfoSet, &data)) {
                WcaLog(LOGMSG_VERBOSE, "Remove device failed.");
            } else {
                SP_DEVINSTALL_PARAMS installParams;

                installParams.cbSize = sizeof(SP_DEVINSTALL_PARAMS);

                if (SetupDiGetDeviceInstallParams(
                    deviceInfoSet, &data, &installParams) &&
                    (installParams.Flags & (DI_NEEDRESTART|DI_NEEDREBOOT))) {

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
