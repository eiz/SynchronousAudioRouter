// SynchronousAudioRouter
// Copyright (C) 2026 Mackenzie Straight
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

#include "install.h"

#include <setupapi.h>
#include <newdev.h>
#include <shlobj.h>
#include <shlwapi.h>

namespace SarTest {

// These must match SynchronousAudioRouter.inf and
// SarInstallerActions/devnode.cpp.
static const wchar_t kHardwareId[] = L"SW\\{0BCFFA5C-E754-48CF-A783-A64C0DC0BB2C}";
static const wchar_t kClassName[] = L"MEDIA";
static const GUID kMediaClassGuid =
    { 0x4d36e96c, 0xe325, 0x11ce, { 0xbf, 0xc1, 0x08, 0x00, 0x2b, 0xe1, 0x03, 0x18 } };

std::wstring appDataDirectory()
{
    wchar_t path[MAX_PATH] = {};

    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, path))) {
        return std::wstring();
    }

    PathAppendW(path, L"SynchronousAudioRouter");
    CreateDirectoryW(path, nullptr);
    return std::wstring(path) + L"\\";
}

std::wstring configurationPath()
{
    return appDataDirectory() + L"default.json";
}

std::wstring loggingPath()
{
    std::wstring path = appDataDirectory() + L"logs\\";

    CreateDirectoryW(path.c_str(), nullptr);
    return path;
}

bool writeDriverConfig(
    const EndpointLayout& layout, const std::wstring& driverClsid,
    int waveRtMinimumFrames, const std::wstring& path)
{
    std::vector<std::string> endpoints;

    for (int pair = 0; pair < layout.pairs; ++pair) {
        JsonObject playback, recording;

        playback.setString("id", layout.playbackId(pair))
            .setString("description", narrow(layout.playbackName(pair)))
            .setString("type", "playback")
            .setInt("channelCount", layout.channels);
        recording.setString("id", layout.recordingId(pair))
            .setString("description", narrow(layout.recordingName(pair)))
            .setString("type", "recording")
            .setInt("channelCount", layout.channels);
        endpoints.push_back(playback.str());
        endpoints.push_back(recording.str());
    }

    JsonObject root;

    root.setString("driverClsid", narrow(driverClsid))
        .setBool("enableApplicationRouting", false)
        .setInt("waveRtMinimumFrames", waveRtMinimumFrames)
        .setRaw("endpoints", jsonArray(endpoints))
        .setRaw("applications", "[]");

    return writeTextFile(path, root.str() + "\n");
}

static bool hasSarHardwareId(HDEVINFO set, SP_DEVINFO_DATA *data)
{
    DWORD type = 0, size = 0;

    SetLastError(0);
    SetupDiGetDeviceRegistryPropertyW(
        set, data, SPDRP_HARDWAREID, &type, nullptr, 0, &size);

    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || size == 0) {
        return false;
    }

    std::vector<wchar_t> buffer(size / sizeof(wchar_t) + 2, L'\0');

    if (!SetupDiGetDeviceRegistryPropertyW(
        set, data, SPDRP_HARDWAREID, &type,
        (BYTE *)buffer.data(), size, nullptr)) {
        return false;
    }

    for (const wchar_t *p = buffer.data(); *p; p += wcslen(p) + 1) {
        if (_wcsicmp(p, kHardwareId) == 0) {
            return true;
        }
    }

    return false;
}

// Enumerates the MEDIA class and returns how many SAR device nodes exist,
// removing them when `remove` is set.
static int visitSarDeviceNodes(bool remove)
{
    HDEVINFO set = SetupDiGetClassDevsW(&kMediaClassGuid, nullptr, nullptr, 0);

    if (set == INVALID_HANDLE_VALUE) {
        logf("SetupDiGetClassDevs failed: %s", lastErrorText(GetLastError()).c_str());
        return -1;
    }

    int found = 0;
    SP_DEVINFO_DATA data = {};

    data.cbSize = sizeof(data);

    for (DWORD i = 0; SetupDiEnumDeviceInfo(set, i, &data); ++i) {
        if (!hasSarHardwareId(set, &data)) {
            continue;
        }

        found++;

        if (remove) {
            BOOL needReboot = FALSE;

            if (!DiUninstallDevice(nullptr, set, &data, 0, &needReboot)) {
                logf("DiUninstallDevice failed: %s", lastErrorText(GetLastError()).c_str());
            } else {
                logf("Removed SAR device node%s", needReboot ? " (reboot required)" : "");
            }
        }
    }

    SetupDiDestroyDeviceInfoList(set);
    return found;
}

bool deviceNodeExists()
{
    return visitSarDeviceNodes(false) > 0;
}

bool removeDeviceNode()
{
    return visitSarDeviceNodes(true) >= 0;
}

bool createDeviceNode()
{
    HDEVINFO set = SetupDiCreateDeviceInfoList(&kMediaClassGuid, nullptr);

    if (set == INVALID_HANDLE_VALUE) {
        logf("SetupDiCreateDeviceInfoList failed: %s", lastErrorText(GetLastError()).c_str());
        return false;
    }

    SP_DEVINFO_DATA data = {};
    bool ok = false;

    data.cbSize = sizeof(data);

    // REG_MULTI_SZ: the ID plus a second terminator.
    std::vector<wchar_t> hardwareIds(kHardwareId, kHardwareId + wcslen(kHardwareId) + 1);
    hardwareIds.push_back(L'\0');

    if (!SetupDiCreateDeviceInfoW(
        set, kClassName, &kMediaClassGuid, nullptr, nullptr,
        DICD_GENERATE_ID, &data)) {
        logf("SetupDiCreateDeviceInfo failed: %s", lastErrorText(GetLastError()).c_str());
    } else if (!SetupDiSetDeviceRegistryPropertyW(
        set, &data, SPDRP_HARDWAREID, (const BYTE *)hardwareIds.data(),
        (DWORD)(hardwareIds.size() * sizeof(wchar_t)))) {
        logf("SetupDiSetDeviceRegistryProperty failed: %s", lastErrorText(GetLastError()).c_str());
    } else if (!SetupDiCallClassInstaller(DIF_REGISTERDEVICE, set, &data)) {
        logf("DIF_REGISTERDEVICE failed: %s", lastErrorText(GetLastError()).c_str());
    } else {
        logf("Created SAR device node");
        ok = true;
    }

    SetupDiDestroyDeviceInfoList(set);
    return ok;
}

bool installDriverPackage(const std::wstring& infPath, bool *rebootRequired)
{
    wchar_t fullPath[MAX_PATH] = {};
    BOOL reboot = FALSE;

    if (!GetFullPathNameW(infPath.c_str(), MAX_PATH, fullPath, nullptr)) {
        logf("Bad INF path: %s", narrow(infPath).c_str());
        return false;
    }

    if (GetFileAttributesW(fullPath) == INVALID_FILE_ATTRIBUTES) {
        logf("INF not found: %s", narrow(fullPath).c_str());
        return false;
    }

    logf("Installing driver package %s", narrow(fullPath).c_str());

    if (!UpdateDriverForPlugAndPlayDevicesW(
        nullptr, kHardwareId, fullPath,
        INSTALLFLAG_FORCE | INSTALLFLAG_NONINTERACTIVE, &reboot)) {
        DWORD error = GetLastError();

        if (error == ERROR_IN_WOW64) {
            logf("Driver installation must run from a 64-bit SarTest on 64-bit Windows");
        } else {
            logf("UpdateDriverForPlugAndPlayDevices failed: %s", lastErrorText(error).c_str());
        }

        return false;
    }

    if (rebootRequired) {
        *rebootRequired = reboot != FALSE;
    }

    logf("Driver package installed%s", reboot ? " (reboot required)" : "");
    return true;
}

bool registerComServer(const std::wstring& dllPath, bool unregister)
{
    typedef HRESULT (STDAPICALLTYPE *RegisterFn)();

    HMODULE module = LoadLibraryW(dllPath.c_str());

    if (!module) {
        logf("LoadLibrary(%s) failed: %s", narrow(dllPath).c_str(),
            lastErrorText(GetLastError()).c_str());
        return false;
    }

    const char *name = unregister ? "DllUnregisterServer" : "DllRegisterServer";
    RegisterFn fn = (RegisterFn)GetProcAddress(module, name);
    bool ok = false;

    if (!fn) {
        logf("%s does not export %s", narrow(dllPath).c_str(), name);
    } else {
        HRESULT hr = fn();

        if (FAILED(hr)) {
            logf("%s in %s failed: %s", name, narrow(dllPath).c_str(),
                hresultText(hr).c_str());
        } else {
            logf("%s: %s", name, narrow(dllPath).c_str());
            ok = true;
        }
    }

    FreeLibrary(module);
    return ok;
}

} // namespace SarTest
