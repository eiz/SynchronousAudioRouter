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


#ifndef _SAR_TEST_CONTROL_H
#define _SAR_TEST_CONTROL_H

#include "common.h"

namespace SarTest {

// The NTSTATUS values the control interface uses
// (driver/docs/behavior.md, A.4), and two the I/O manager answers with.
const uint32_t kStatusSuccess = 0x00000000;
const uint32_t kStatusPending = 0x00000103;
const uint32_t kStatusDatatypeMisalignment = 0x80000002;
const uint32_t kStatusBufferOverflow = 0x80000005;
const uint32_t kStatusAccessViolation = 0xC0000005;
const uint32_t kStatusInvalidParameter = 0xC000000D;
const uint32_t kStatusInvalidDeviceRequest = 0xC0000010;
const uint32_t kStatusAccessDenied = 0xC0000022;
const uint32_t kStatusBufferTooSmall = 0xC0000023;
const uint32_t kStatusCancelled = 0xC0000120;
const uint32_t kStatusFileClosed = 0xC0000128;
const uint32_t kStatusResourceInUse = 0xC0000708;
const uint32_t kStatusInvalidStateTransition = 0xC000A003;

// "STATUS_NAME (0x........)" for the statuses above, the number alone for
// any other.
std::string statusText(uint32_t status);

// The control requests (A.2).
const uint32_t kSetBufferLayout = 0x0022C007;
const uint32_t kCreateEndpoint = 0x0022C00B;
const uint32_t kWaitNotifications = 0x0022C00C;
const uint32_t kStartRegistryFilter = 0x0022C013;

// Their structures (A.5, A.7), laid out as on the wire.
#pragma pack(push, 4)
struct LayoutRequest
{
    uint32_t bufferSize;
    uint32_t periodSizeBytes;
    uint32_t sampleRate;
    uint32_t sampleSize;
    uint32_t minimumFrameCount;
};

struct LayoutReply
{
    uint64_t virtualAddress;
    uint32_t actualSize;
    uint32_t registerBase;
};

struct EndpointRequest
{
    uint32_t type;          // 1 recording, 2 playback
    uint32_t index;
    uint32_t channelCount;
    wchar_t id[64];
    wchar_t name[64];
};

struct NotificationRecord
{
    uint64_t handle;
    uint64_t associatedData;
};

struct RegisterRecord
{
    uint32_t generation;
    uint32_t positionRegister;
    uint32_t reserved;
    uint32_t bufferOffset;
    uint32_t bufferSize;
    uint32_t notificationCount;
    uint32_t activeChannelCount;
};
#pragma pack(pop)

static_assert(sizeof(LayoutRequest) == 20, "A.5");
static_assert(sizeof(LayoutReply) == 16, "A.5");
static_assert(sizeof(EndpointRequest) == 268, "A.5");
static_assert(sizeof(NotificationRecord) == 16, "A.5");
static_assert(sizeof(RegisterRecord) == 28, "A.7");

struct ControlResult
{
    // The request's final status: the one it completed with if it was
    // pended, the one the system call returned otherwise.
    uint32_t status = 0;
    // The system call returned STATUS_PENDING first.
    bool pended = false;
    // Still pending when the wait gave up; status is STATUS_PENDING.
    bool timedOut = false;
    // Windows reports a byte count only for requests that were pended or did
    // not fail outright (success and warning statuses).
    bool informationValid = false;
    uint64_t information = 0;
};

// The control device's interface path; empty when the device isn't there.
std::wstring controlDevicePath();

// Opens the control device the way hosts do: overlapped, shared. Returns
// INVALID_HANDLE_VALUE on failure.
HANDLE openControlDevice(
    const std::wstring& path, DWORD access = GENERIC_READ | GENERIC_WRITE);

// One device control request, issued with NtDeviceIoControlFile so that the
// driver's own NTSTATUS comes back and no Win32 layer touches the buffers.
// The object must outlive the request: destroying it cancels a request that
// is still pending and waits for it.
class ControlRequest
{
public:
    ControlRequest();
    ~ControlRequest();
    ControlRequest(const ControlRequest&) = delete;
    ControlRequest& operator=(const ControlRequest&) = delete;

    void start(
        HANDLE device, uint32_t code,
        void *input, uint32_t inputLength, void *output, uint32_t outputLength);
    // True once the request has completed.
    bool wait(DWORD milliseconds);
    void cancel();
    ControlResult result() const;

private:
    struct IoStatusBlock
    {
        union {
            LONG status;
            void *pointer;
        };
        ULONG_PTR information;
    };

    HANDLE _device = nullptr;
    HANDLE _event = nullptr;
    IoStatusBlock _iosb = {};
    uint32_t _returned = 0;
    bool _started = false;
    bool _pended = false;
    bool _completed = false;
};

// start() and wait() in one call.
ControlResult controlRequest(
    HANDLE device, uint32_t code,
    void *input, uint32_t inputLength, void *output, uint32_t outputLength,
    DWORD timeoutMilliseconds = 30000);

} // namespace SarTest
#endif // _SAR_TEST_CONTROL_H
