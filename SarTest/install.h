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

#ifndef _SAR_TEST_INSTALL_H
#define _SAR_TEST_INSTALL_H

#include "common.h"

namespace SarTest {

// %APPDATA%\SynchronousAudioRouter\, the directory SarAsio reads its
// configuration from and writes its logs to. Created if missing.
std::wstring appDataDirectory();
std::wstring configurationPath();
std::wstring loggingPath();

// Writes a SarAsio default.json describing the layout, with the given ASIO
// driver CLSID as the physical interface and application routing disabled.
bool writeDriverConfig(
    const EndpointLayout& layout, const std::wstring& driverClsid,
    int waveRtMinimumFrames, const std::wstring& path);

// Software device node for the SAR driver, matching the INF's hardware ID.
bool deviceNodeExists();
bool createDeviceNode();
bool removeDeviceNode();

// Installs (or updates) the driver package for the device node from an INF.
// Requires an elevated 64-bit process on 64-bit Windows.
bool installDriverPackage(const std::wstring& infPath, bool *rebootRequired);

// Calls DllRegisterServer / DllUnregisterServer in the given DLL.
bool registerComServer(const std::wstring& dllPath, bool unregister);

} // namespace SarTest
#endif // _SAR_TEST_INSTALL_H
