#include <iostream>
#include <windows.h>

int main(int argc, char *argv[])
{
	HANDLE device;
    char buf[1024];

    device = CreateFileA("\\\\.\\SynchronousAudioRouter",
        GENERIC_ALL, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);

    if (device == INVALID_HANDLE_VALUE) {
        std::cerr << "Couldn't open SAR device. Error "
            << std::hex << GetLastError() << std::dec << std::endl;
        return 1;
    }

    std::cout << "Opened SAR device." << std::endl;

    DeviceIoControl(device, 1, buf, sizeof(buf), nullptr, 0, nullptr, nullptr);
    return 0;
}
