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


#include "control.h"

#include <initguid.h>
#include <setupapi.h>

namespace SarTest {

namespace {

// {C16E8D6C-C4CC-4C76-B11C-79B8414EA968}, the control interface class (A.1).
DEFINE_GUID(kControlInterfaceClass,
    0xc16e8d6c, 0xc4cc, 0x4c76, 0xb1, 0x1c, 0x79, 0xb8, 0x41, 0x4e, 0xa9, 0x68);

const ULONG_PTR kInformationUntouched = ~(ULONG_PTR)0 - 0x5A;

typedef LONG (NTAPI *NtDeviceIoControlFileFn)(
    HANDLE file, HANDLE event, void *apcRoutine, void *apcContext,
    void *ioStatusBlock, ULONG code,
    void *input, ULONG inputLength, void *output, ULONG outputLength);

NtDeviceIoControlFileFn ntDeviceIoControlFile()
{
    static NtDeviceIoControlFileFn fn = (NtDeviceIoControlFileFn)GetProcAddress(
        GetModuleHandleW(L"ntdll.dll"), "NtDeviceIoControlFile");

    return fn;
}

struct StatusName
{
    uint32_t status;
    const char *name;
};

const StatusName kStatusNames[] = {
    { kStatusSuccess, "STATUS_SUCCESS" },
    { kStatusPending, "STATUS_PENDING" },
    { kStatusDatatypeMisalignment, "STATUS_DATATYPE_MISALIGNMENT" },
    { kStatusBufferOverflow, "STATUS_BUFFER_OVERFLOW" },
    { kStatusAccessViolation, "STATUS_ACCESS_VIOLATION" },
    { kStatusInvalidParameter, "STATUS_INVALID_PARAMETER" },
    { kStatusInvalidDeviceRequest, "STATUS_INVALID_DEVICE_REQUEST" },
    { kStatusAccessDenied, "STATUS_ACCESS_DENIED" },
    { kStatusBufferTooSmall, "STATUS_BUFFER_TOO_SMALL" },
    { kStatusCancelled, "STATUS_CANCELLED" },
    { kStatusFileClosed, "STATUS_FILE_CLOSED" },
    { kStatusResourceInUse, "STATUS_RESOURCE_IN_USE" },
    { kStatusInvalidStateTransition, "STATUS_INVALID_STATE_TRANSITION" },
    { 0xC000009A, "STATUS_INSUFFICIENT_RESOURCES" },
    { 0xC0000225, "STATUS_NOT_FOUND" },
    { 0xC0000034, "STATUS_OBJECT_NAME_NOT_FOUND" },
    { 0xC0000035, "STATUS_OBJECT_NAME_COLLISION" },
    { 0xC00000BB, "STATUS_NOT_SUPPORTED" },
    { 0xC0000001, "STATUS_UNSUCCESSFUL" },
    { 0xC0000002, "STATUS_NOT_IMPLEMENTED" },
};

} // namespace

std::string statusText(uint32_t status)
{
    char buffer[80];

    for (const auto& entry : kStatusNames) {
        if (entry.status == status) {
            snprintf(buffer, sizeof(buffer), "%s (0x%08X)", entry.name, status);
            return buffer;
        }
    }

    snprintf(buffer, sizeof(buffer), "0x%08X", status);
    return buffer;
}

std::wstring controlDevicePath()
{
    std::wstring path;
    HDEVINFO set = SetupDiGetClassDevsW(
        &kControlInterfaceClass, nullptr, nullptr,
        DIGCF_DEVICEINTERFACE | DIGCF_PRESENT);

    if (set == INVALID_HANDLE_VALUE) {
        return path;
    }

    SP_DEVICE_INTERFACE_DATA data = {};

    data.cbSize = sizeof(data);

    if (SetupDiEnumDeviceInterfaces(set, nullptr, &kControlInterfaceClass, 0, &data)) {
        DWORD needed = 0;

        SetupDiGetDeviceInterfaceDetailW(set, &data, nullptr, 0, &needed, nullptr);

        if (needed) {
            std::vector<BYTE> storage(needed);
            auto detail = (SP_DEVICE_INTERFACE_DETAIL_DATA_W *)storage.data();

            detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);

            if (SetupDiGetDeviceInterfaceDetailW(set, &data, detail, needed, nullptr, nullptr)) {
                path = detail->DevicePath;
            }
        }
    }

    SetupDiDestroyDeviceInfoList(set);
    return path;
}

HANDLE openControlDevice(const std::wstring& path, DWORD access)
{
    return CreateFileW(
        path.c_str(), access, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED, nullptr);
}

ControlRequest::ControlRequest()
{
    _event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
}

ControlRequest::~ControlRequest()
{
    if (_started && !_completed) {
        cancel();
        // The request holds pointers to the status block and the event; it
        // has to be over before they go.
        wait(INFINITE);
    }

    if (_event) {
        CloseHandle(_event);
    }
}

void ControlRequest::start(
    HANDLE device, uint32_t code,
    void *input, uint32_t inputLength, void *output, uint32_t outputLength)
{
    _device = device;
    _iosb.pointer = nullptr;
    _iosb.information = kInformationUntouched;
    ResetEvent(_event);
    _started = true;
    _completed = false;
    _returned = (uint32_t)ntDeviceIoControlFile()(
        device, _event, nullptr, nullptr, &_iosb, code,
        input, inputLength, output, outputLength);
    _pended = _returned == kStatusPending;
    _completed = !_pended;
}

bool ControlRequest::wait(DWORD milliseconds)
{
    if (!_started || _completed) {
        return _started;
    }

    if (WaitForSingleObject(_event, milliseconds) == WAIT_OBJECT_0) {
        _completed = true;
    }

    return _completed;
}

void ControlRequest::cancel()
{
    if (_started && !_completed && _device) {
        CancelIoEx(_device, nullptr);
    }
}

ControlResult ControlRequest::result() const
{
    ControlResult result;

    result.pended = _pended;

    if (_pended && !_completed) {
        result.status = kStatusPending;
        result.timedOut = true;
        return result;
    }

    result.status = _pended ? (uint32_t)_iosb.status : _returned;
    result.informationValid = _iosb.information != kInformationUntouched;
    result.information = result.informationValid ? (uint64_t)_iosb.information : 0;
    return result;
}

ControlResult controlRequest(
    HANDLE device, uint32_t code,
    void *input, uint32_t inputLength, void *output, uint32_t outputLength,
    DWORD timeoutMilliseconds)
{
    ControlRequest request;

    request.start(device, code, input, inputLength, output, outputLength);
    request.wait(timeoutMilliseconds);

    ControlResult result = request.result();

    // Leaving the scope cancels a request that is still pending.
    return result;
}

} // namespace SarTest
