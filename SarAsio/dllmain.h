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

#ifndef _SAR_ASIO_DLLMAIN_H
#define _SAR_ASIO_DLLMAIN_H

// {70BCE35E-E409-4552-AF9F-0B074FAB1952}
DEFINE_GUID(LIBID_SarAsio,
    0x70bce35e, 0xe409, 0x4552, 0xaf, 0x9f, 0xb, 0x7, 0x4f, 0xab, 0x19, 0x52);

struct SarAsioModule: public CAtlDllModuleT<SarAsioModule>
{
    DECLARE_LIBID(LIBID_SarAsio)
};

extern SarAsioModule _AtlModule;

BOOL WINAPI DllMain(
    HMODULE hModule, DWORD reason, LPVOID reserved);
STDAPI DllCanUnloadNow();
STDAPI DllGetClassObject(REFCLSID rclsid, REFIID riid, LPVOID *ppv);
STDAPI DllRegisterServer();
STDAPI DllUnregisterServer();

#endif // _SAR_ASIO_DLLMAIN_H