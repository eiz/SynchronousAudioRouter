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

// Microsoft thinks getenv is insecure, presumably because setenv/putenv are not
// thread safe? I think it's going to be ok.
#define _CRT_SECURE_NO_WARNINGS

#include "stdafx.h"
#include "dllmain.h"
#include "utility.h"

SarAsioModule _AtlModule;
HMODULE gDllModule;

BOOL WINAPI DllMain(HMODULE hModule, DWORD reason, LPVOID reserved)
{
    if (reason == DLL_PROCESS_ATTACH) {
        char buf[1024] = {};

        // Google Log use native open() using std::string which uses system codepage
        FLAGS_log_dir = Sar::TCHARToLocal(Sar::LoggingPath().c_str());
        FLAGS_logbuflevel = -1;

        char *sarLogVar = getenv("SAR_ASIO_LOG");

        if (sarLogVar && atoi(sarLogVar)) {
            GetModuleFileNameA(nullptr, buf, 1024);
            google::InitGoogleLogging(
                *buf ? _strdup(PathFindFileNameA(buf)) : "SarAsio");
            LOG(INFO) << "Initializing SarAsio.";
        } else {
            LOG(INFO) << "Logging disabled. (This shouldn't log.)";
        }
    }

    gDllModule = hModule;
    return _AtlModule.DllMain(reason, reserved);
}

STDAPI DllCanUnloadNow()
{
    return _AtlModule.DllCanUnloadNow();
}

STDAPI DllGetClassObject(REFCLSID rclsid, REFIID riid, LPVOID *ppv)
{
    return _AtlModule.DllGetClassObject(rclsid, riid, ppv);
}

STDAPI DllRegisterServer()
{
    return _AtlModule.DllRegisterServer(FALSE);
}

STDAPI DllUnregisterServer()
{
    return _AtlModule.DllUnregisterServer(FALSE);
}
