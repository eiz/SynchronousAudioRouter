#include <iostream>
#include <windows.h>

#include <sar.h>

BOOL createEndpoint(
    HANDLE device, DWORD endpointType, DWORD index, DWORD channelCount,
    LPWSTR name)
{
    SarCreateEndpointRequest request = {};

    request.type = endpointType;
    request.index = index;
    request.channelCount = channelCount;
    wcscpy_s(request.name, name);

    if (!DeviceIoControl(device, SAR_REQUEST_CREATE_ENDPOINT,
            (LPVOID)&request, sizeof(request), nullptr, 0, nullptr, nullptr)) {
        return FALSE;
    }

    return TRUE;
}

int main(int argc, char *argv[])
{
	HANDLE device;

    device = CreateFileA("\\\\.\\SynchronousAudioRouter",
        GENERIC_ALL, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);

    if (device == INVALID_HANDLE_VALUE) {
        std::cerr << "Couldn't open SAR device. Error "
            << std::hex << GetLastError() << std::dec << std::endl;
        return 1;
    }

    std::cout << "Opened SAR device." << std::endl;

    if (!createEndpoint(
        device, SAR_ENDPOINT_TYPE_PLAYBACK, 0, 2, L"Music Out (Stereo)")) {
        std::cerr << "Couldn't create endpoint. Error "
            << std::hex << GetLastError() << std::dec << std::endl;
        return 1;
    }

    return 0;
}
