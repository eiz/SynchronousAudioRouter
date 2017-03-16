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

#include <iostream>
#include <windows.h>
#include <SetupAPI.h>

#include <initguid.h>
#include <sar.h>

int main(int argc, char *argv[])
{
	HANDLE device;
    DWORD bytes;

    device = CreateFile(L"\\??\\SarNdis",
        GENERIC_ALL, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);

    if (device == INVALID_HANDLE_VALUE) {
        std::cerr << "Couldn't open SarNdis device: "
            << GetLastError() << std::endl;
        return 1;
    }

    std::cout << "Opened SarNdis device." << std::endl;
    DeviceIoControl(device, SARNDIS_ENUMERATE, 0, 0, 0, 0, &bytes, 0);
    CloseHandle(device);
    return 0;
}
