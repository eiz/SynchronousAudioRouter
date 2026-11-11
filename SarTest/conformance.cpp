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


#include "conformance.h"
#include "control.h"
#include "wasapi.h"

#include <winioctl.h>
#include <audioclient.h>
#include <mmreg.h>
#include <ks.h>
#include <ksmedia.h>
#include <atomic>
#include <mutex>
#include <thread>

namespace SarTest {

namespace {

const uint32_t kCell = 65536;
const uint32_t kPage = 4096;

enum class Kind { Spec, Deviation, Observation, Check };

struct CaseRecord
{
    std::string name;
    Kind kind = Kind::Spec;
    bool ok = true;
    bool isRequest = false;
    uint32_t expected = 0;
    long long expectedInformation = -1;
    ControlResult actual;
    std::string detail;
};

class Report
{
public:
    explicit Report(bool legacy) : _legacy(legacy) {}

    // A row every driver has to answer the same way. `information` is
    // compared only when Windows reported one (see ControlResult).
    bool spec(
        const std::string& name, const ControlResult& actual,
        uint32_t status, long long information = -1)
    {
        return request(name, Kind::Spec, actual, status, information);
    }

    // A row where the driver in this repository deliberately departs from
    // what releases up to 0.13 did (section 13).
    bool deviation(
        const std::string& name, const ControlResult& actual,
        uint32_t legacyStatus, uint32_t status)
    {
        return request(name, Kind::Deviation, actual, _legacy ? legacyStatus : status, -1);
    }

    // Recorded, never failed: behaviour the specification leaves open.
    void observe(const std::string& name, const ControlResult& actual)
    {
        CaseRecord record;

        record.name = name;
        record.kind = Kind::Observation;
        record.isRequest = true;
        record.actual = actual;
        logf("  observed  %-58s %s", name.c_str(), statusText(actual.status).c_str());
        _cases.push_back(record);
    }

    bool check(const std::string& name, bool ok, const std::string& detail = "")
    {
        CaseRecord record;

        record.name = name;
        record.kind = Kind::Check;
        record.ok = ok;
        record.detail = detail;
        logf("  %-9s %-58s %s", ok ? "ok" : "FAILED", name.c_str(), detail.c_str());
        _cases.push_back(record);
        return ok;
    }

    bool passed() const
    {
        for (const auto& record : _cases) {
            if (!record.ok) {
                return false;
            }
        }

        return true;
    }

    int failures() const
    {
        int count = 0;

        for (const auto& record : _cases) {
            count += record.ok ? 0 : 1;
        }

        return count;
    }

    std::string toJson() const
    {
        std::vector<std::string> items;

        for (const auto& record : _cases) {
            JsonObject item;
            const char *kind =
                record.kind == Kind::Spec ? "spec" :
                record.kind == Kind::Deviation ? "deviation" :
                record.kind == Kind::Observation ? "observation" : "check";

            item.setString("name", record.name).setString("kind", kind).setBool("ok", record.ok);

            if (record.isRequest) {
                item.setString("status", statusText(record.actual.status))
                    .setBool("pended", record.actual.pended);

                if (record.actual.informationValid) {
                    item.setInt("information", (long long)record.actual.information);
                }

                if (record.kind != Kind::Observation) {
                    item.setString("expected", statusText(record.expected));

                    if (record.expectedInformation >= 0) {
                        item.setInt("expectedInformation", record.expectedInformation);
                    }
                }
            }

            if (!record.detail.empty()) {
                item.setString("detail", record.detail);
            }

            items.push_back(item.str());
        }

        return jsonArray(items);
    }

private:
    bool request(
        const std::string& name, Kind kind, const ControlResult& actual,
        uint32_t status, long long information)
    {
        CaseRecord record;

        record.name = name;
        record.kind = kind;
        record.isRequest = true;
        record.expected = status;
        record.expectedInformation = information;
        record.actual = actual;
        record.ok = !actual.timedOut && actual.status == status;

        if (record.ok && information >= 0 && actual.informationValid &&
            actual.information != (uint64_t)information) {

            record.ok = false;
            record.detail = "information " + std::to_string(actual.information) +
                ", expected " + std::to_string(information);
        }

        if (!record.ok && record.detail.empty()) {
            record.detail = "expected " + statusText(status) +
                (actual.timedOut ? ", still pending" : "");
        }

        logf("  %-9s %-58s %s%s%s", record.ok ? "ok" : "FAILED", name.c_str(),
            statusText(actual.status).c_str(), record.detail.empty() ? "" : "; ",
            record.detail.c_str());
        _cases.push_back(record);
        return record.ok;
    }

    bool _legacy;
    std::vector<CaseRecord> _cases;
};

// Two pages, the second one inaccessible, for buffers that run into memory
// the driver must not be able to read or write.
class GuardedPages
{
public:
    GuardedPages()
    {
        _base = (BYTE *)VirtualAlloc(nullptr, 2 * kPage, MEM_RESERVE, PAGE_NOACCESS);

        if (_base) {
            VirtualAlloc(_base, kPage, MEM_COMMIT, PAGE_READWRITE);
        }
    }

    ~GuardedPages()
    {
        if (_base) {
            VirtualFree(_base, 0, MEM_RELEASE);
        }
    }

    bool valid() const { return _base != nullptr; }
    // The last `bytes` bytes before the inaccessible page.
    BYTE *endMinus(size_t bytes) const { return _base + kPage - bytes; }
    // The first byte of the inaccessible page.
    BYTE *inaccessible() const { return _base + kPage; }

private:
    BYTE *_base = nullptr;
};

LayoutRequest validLayout(uint32_t bufferSize)
{
    LayoutRequest request = {};

    request.bufferSize = bufferSize;
    request.sampleRate = 48000;
    request.sampleSize = 4;
    // One channel's audio in one 10 ms period.
    request.periodSizeBytes = 480 * request.sampleSize;
    request.minimumFrameCount = 2;
    return request;
}

EndpointRequest validEndpoint(
    uint32_t type, uint32_t index, uint32_t channels,
    const std::wstring& id, const std::wstring& name)
{
    EndpointRequest request = {};

    request.type = type;
    request.index = index;
    request.channelCount = channels;
    wcsncpy_s(request.id, id.c_str(), _TRUNCATE);
    wcsncpy_s(request.name, name.c_str(), _TRUNCATE);
    return request;
}

uint32_t roundUp(uint32_t value, uint32_t to)
{
    return (value + to - 1) / to * to;
}

#ifdef _WIN64
// An address no user-mode pointer may name. A driver that used it without
// checking would read or write kernel memory.
void *kernelAddress() { return (void *)0xFFFF800000001000ull; }
#endif

RegisterRecord readRecord(const BYTE *section, uint32_t registerBase, uint32_t index)
{
    RegisterRecord record;

    memcpy(&record, section + registerBase + index * sizeof(RegisterRecord), sizeof(record));
    return record;
}

std::string describe(const RegisterRecord& record)
{
    char buffer[200];

    snprintf(buffer, sizeof(buffer),
        "generation %u position %u bufferOffset %u bufferSize %u notificationCount %u activeChannelCount %u",
        record.generation, record.positionRegister, record.bufferOffset, record.bufferSize,
        record.notificationCount, record.activeChannelCount);
    return buffer;
}

// The access an open handle was granted, or 0 if it can't be told.
uint32_t grantedAccess(HANDLE handle)
{
    struct BasicInformation
    {
        ULONG attributes;
        ULONG grantedAccess;
        ULONG handleCount;
        ULONG pointerCount;
        ULONG reserved[10];
    };
    typedef LONG (NTAPI *NtQueryObjectFn)(HANDLE, int, void *, ULONG, ULONG *);
    static NtQueryObjectFn query = (NtQueryObjectFn)GetProcAddress(
        GetModuleHandleW(L"ntdll.dll"), "NtQueryObject");
    BasicInformation info = {};

    if (!query || query(handle, 0, &info, sizeof(info), nullptr) < 0) {
        return 0;
    }

    return info.grantedAccess;
}

// Whether the view of a section that was at `address` is gone. The address
// alone does not tell: once the view is unmapped, anything else the process
// maps can land there. So the caller leaves a mark in the section first, and
// the view counts as gone when the address is free, unreadable or no longer
// holds the mark.
const uint64_t kUnmapMark = 0x5341525F4D41524Bull;

void markSection(BYTE *address)
{
    memcpy(address, &kUnmapMark, sizeof(kUnmapMark));
}

bool sectionUnmapped(const BYTE *address, std::string *detail)
{
    MEMORY_BASIC_INFORMATION info = {};
    uint64_t found = 0;
    SIZE_T read = 0;

    if (!VirtualQuery(address, &info, sizeof(info)) || info.State == MEM_FREE) {
        return true;
    }

    bool readable = ReadProcessMemory(GetCurrentProcess(), address, &found, sizeof(found), &read) &&
        read == sizeof(found);
    char buffer[160];

    snprintf(buffer, sizeof(buffer), "state 0x%lx type 0x%lx size %llu, %s", info.State, info.Type,
        (unsigned long long)info.RegionSize,
        !readable ? "unreadable" : found == kUnmapMark ? "still holds the mark" : "holds something else");
    *detail = buffer;
    return !readable || found != kUnmapMark;
}

// --- The groups of cases ---------------------------------------------------

void accessCases(Report& report, const std::wstring& path)
{
    logf("Access");

    HANDLE readOnly = openControlDevice(path, GENERIC_READ);

    if (!report.check("the control device opens for reading alone",
            readOnly != INVALID_HANDLE_VALUE)) {
        return;
    }

    LayoutRequest request = validLayout(1 << 20);
    LayoutReply reply = {};

    // The request codes ask for read and write access; Windows refuses the
    // request before the driver sees it.
    report.spec("set buffer layout on a handle without write access",
        controlRequest(readOnly, kSetBufferLayout, &request, sizeof(request), &reply, sizeof(reply)),
        kStatusAccessDenied);
    CloseHandle(readOnly);

    // The reference string is compared without regard to case.
    std::wstring upper = path;
    size_t slash = upper.find_last_of(L'\\');

    for (size_t i = slash == std::wstring::npos ? upper.size() : slash; i < upper.size(); ++i) {
        upper[i] = (wchar_t)towupper(upper[i]);
    }

    HANDLE shouted = openControlDevice(upper);

    if (report.check("the control device opens with its reference string in capitals",
            shouted != INVALID_HANDLE_VALUE)) {

        EndpointRequest endpoint = validEndpoint(2, 0, 2, L"SarConformance-case", L"case");

        report.spec("and that handle is a control handle",
            controlRequest(shouted, kCreateEndpoint, &endpoint, sizeof(endpoint), nullptr, 0),
            kStatusInvalidStateTransition);
        CloseHandle(shouted);
    }
}

void beforeLayoutCases(Report& report, const std::wstring& path)
{
    logf("A handle with no layout");

    HANDLE device = openControlDevice(path);
    GuardedPages pages;

    if (!report.check("the control device opens", device != INVALID_HANDLE_VALUE && pages.valid())) {
        return;
    }

    // Unrecognized requests. 0x0022C017 is function 5, which releases before
    // 0.14.0 used and which must not get a new meaning.
    const uint32_t unknown[] = { 0x0022C017, 0x0022C018, 0x0022E000 };

    for (uint32_t code : unknown) {
        char name[64];

        snprintf(name, sizeof(name), "unrecognized request 0x%08X", code);
        report.deviation(name, controlRequest(device, code, nullptr, 0, nullptr, 0),
            kStatusSuccess, kStatusInvalidDeviceRequest);
    }

    // A kernel streaming request on a control handle.
    report.observe("IOCTL_KS_PROPERTY on a control handle",
        controlRequest(device, 0x002F0003, nullptr, 0, nullptr, 0));

    EndpointRequest endpoint = validEndpoint(2, 0, 2, L"SarConformance-early", L"early");

    report.spec("create endpoint before any layout",
        controlRequest(device, kCreateEndpoint, &endpoint, sizeof(endpoint), nullptr, 0),
        kStatusInvalidStateTransition);

    endpoint.type = 0;
    report.spec("create endpoint, bad type, before any layout (range is checked first)",
        controlRequest(device, kCreateEndpoint, &endpoint, sizeof(endpoint), nullptr, 0),
        kStatusInvalidParameter);

    LayoutRequest good = validLayout(1 << 20);
    LayoutReply reply = {};
    auto layout = [&](void *input, uint32_t inputLength) {
        return controlRequest(device, kSetBufferLayout, input, inputLength, &reply, sizeof(reply));
    };

    report.spec("set buffer layout, input one byte short",
        layout(&good, sizeof(good) - 1), kStatusBufferOverflow, 0);
    report.spec("set buffer layout, input of no bytes",
        layout(&good, 0), kStatusBufferOverflow, 0);

    BYTE *aligned = pages.endMinus(64);

    memcpy(aligned + 1, &good, sizeof(good));
    report.spec("set buffer layout, input pointer off by one",
        layout(aligned + 1, sizeof(good)), kStatusDatatypeMisalignment, 0);
    memcpy(aligned + 2, &good, sizeof(good));
    report.spec("set buffer layout, input pointer off by two",
        layout(aligned + 2, sizeof(good)), kStatusDatatypeMisalignment, 0);
    report.spec("set buffer layout, input in inaccessible memory",
        layout(pages.inaccessible(), sizeof(good)), kStatusAccessViolation);
    // The first 8 bytes are readable, the rest is not.
    memcpy(pages.endMinus(8), &good, 8);
    report.spec("set buffer layout, input running into inaccessible memory",
        layout(pages.endMinus(8), sizeof(good)), kStatusAccessViolation);
    report.spec("set buffer layout, null input pointer",
        layout(nullptr, sizeof(good)), kStatusAccessViolation);
#ifdef _WIN64
    report.spec("set buffer layout, input pointer in kernel space",
        layout(kernelAddress(), sizeof(good)), kStatusAccessViolation);
#endif

    struct BadLayout
    {
        const char *name;
        LayoutRequest request;
    };
    std::vector<BadLayout> bad;
    auto addBad = [&](const char *name, void (*change)(LayoutRequest&)) {
        BadLayout entry = { name, validLayout(1 << 20) };

        change(entry.request);
        bad.push_back(entry);
    };

    addBad("bufferSize 0", [](LayoutRequest& r) { r.bufferSize = 0; });
    addBad("bufferSize one over 128 MiB", [](LayoutRequest& r) { r.bufferSize = 134217729; });
    addBad("bufferSize 0xFFFFFFFF", [](LayoutRequest& r) { r.bufferSize = 0xFFFFFFFF; });
    addBad("sampleSize 0", [](LayoutRequest& r) { r.sampleSize = 0; });
    addBad("sampleSize 5", [](LayoutRequest& r) { r.sampleSize = 5; });
    addBad("sampleRate 7999", [](LayoutRequest& r) { r.sampleRate = 7999; });
    addBad("sampleRate 192001", [](LayoutRequest& r) { r.sampleRate = 192001; });
    addBad("periodSizeBytes 0", [](LayoutRequest& r) { r.periodSizeBytes = 0; });
    addBad("periodSizeBytes above bufferSize",
        [](LayoutRequest& r) { r.periodSizeBytes = r.bufferSize + 1; });

    for (auto& entry : bad) {
        report.spec(std::string("set buffer layout, ") + entry.name,
            layout(&entry.request, sizeof(entry.request)), kStatusInvalidParameter);
    }

    BYTE tiny[16] = {};

    // A failure outright: Windows does not pass the byte count on.
    report.spec("wait for notification events, room for no record",
        controlRequest(device, kWaitNotifications, nullptr, 0, tiny, 15),
        kStatusBufferTooSmall);

    // Nothing above may have set a layout.
    endpoint = validEndpoint(2, 0, 2, L"SarConformance-early", L"early");
    report.spec("no refused request set a layout",
        controlRequest(device, kCreateEndpoint, &endpoint, sizeof(endpoint), nullptr, 0),
        kStatusInvalidStateTransition);
    CloseHandle(device);
}

// "Set buffer layout" requests whose reply cannot be delivered. Releases up
// to 0.13 applied the layout anyway and left the handle unable to repeat
// the request; the driver in this repository undoes it.
void replyFailureCases(Report& report, const std::wstring& path)
{
    logf("Set buffer layout with a reply that cannot be written");

    GuardedPages pages;
    BYTE *readOnly = (BYTE *)VirtualAlloc(nullptr, kPage, MEM_COMMIT | MEM_RESERVE, PAGE_READONLY);
    LayoutReply reply = {};
    BYTE scratch[64] = {};

    struct Attempt
    {
        const char *name;
        void *output;
        uint32_t outputLength;
        uint32_t status;
        long long information;
    };
    std::vector<Attempt> attempts = {
        { "output of no bytes", &reply, 0, kStatusBufferOverflow, 16 },
        { "output of 8 bytes", &reply, 8, kStatusBufferTooSmall, -1 },
        { "output pointer off by one", scratch + 1, 16, kStatusDatatypeMisalignment, 16 },
        { "output in read-only memory", readOnly, 16, kStatusAccessViolation, -1 },
        { "output in inaccessible memory", pages.inaccessible(), 16, kStatusAccessViolation, -1 },
        { "output running into inaccessible memory", pages.endMinus(8), 16, kStatusAccessViolation, -1 },
#ifdef _WIN64
        { "output pointer in kernel space", kernelAddress(), 16, kStatusAccessViolation, -1 },
#endif
    };

    for (const auto& attempt : attempts) {
        HANDLE device = openControlDevice(path);

        if (device == INVALID_HANDLE_VALUE) {
            report.check(std::string("open for: ") + attempt.name, false);
            continue;
        }

        LayoutRequest request = validLayout(1 << 20);

        report.spec(std::string("set buffer layout, ") + attempt.name,
            controlRequest(device, kSetBufferLayout, &request, sizeof(request),
                attempt.output, attempt.outputLength),
            attempt.status, attempt.information);
        reply = {};
        report.deviation(std::string("  and the request repeated properly after it"),
            controlRequest(device, kSetBufferLayout, &request, sizeof(request), &reply, sizeof(reply)),
            kStatusInvalidStateTransition, kStatusSuccess);
        CloseHandle(device);
    }

    if (readOnly) {
        VirtualFree(readOnly, 0, MEM_RELEASE);
    }
}

void geometryCases(Report& report, const std::wstring& path, bool legacy)
{
    logf("Section geometry");

    // bufferSize -> audio area; one byte, a size that is no multiple of a
    // page, one that is a multiple of a page but not of a cell, and the
    // largest allowed.
    //
    // Not one byte for releases up to 0.13: with an audio area smaller than
    // one cell they ask the pool for zero bytes, which is harmless on its
    // own but which Driver Verifier stops the machine for. One cell instead.
    const uint32_t sizes[] = { legacy ? kCell : 1, 1000000, (3 << 20) + 3 * kPage, 134217728 };

    for (uint32_t size : sizes) {
        HANDLE device = openControlDevice(path);
        LayoutRequest request = validLayout(size);
        LayoutReply reply = {};
        char name[96];

        request.periodSizeBytes = 1;
        snprintf(name, sizeof(name), "set buffer layout, bufferSize %u", size);

        if (!report.spec(name,
                controlRequest(device, kSetBufferLayout, &request, sizeof(request), &reply, sizeof(reply)),
                kStatusSuccess, 16)) {
            CloseHandle(device);
            continue;
        }

        uint32_t area = roundUp(size, kPage);
        char detail[160];

        snprintf(detail, sizeof(detail), "registerBase %u actualSize %u address 0x%llx",
            reply.registerBase, reply.actualSize, (unsigned long long)reply.virtualAddress);
        report.check("  registerBase is bufferSize rounded up to a page, actualSize one cell more",
            reply.registerBase == area && reply.actualSize == area + kCell, detail);

        BYTE *section = (BYTE *)(uintptr_t)reply.virtualAddress;
        MEMORY_BASIC_INFORMATION info = {};
        bool mapped = section && (sizeof(void *) == 8 || (reply.virtualAddress >> 32) == 0) &&
            VirtualQuery(section, &info, sizeof(info)) && info.State == MEM_COMMIT &&
            info.Type == MEM_MAPPED && (info.Protect & PAGE_READWRITE);

        if (report.check("  the section is mapped readable and writable at the address returned", mapped)) {
            bool zero = true;

            for (uint32_t i = 0; i < kPage && i < area; ++i) {
                zero = zero && section[i] == 0;
            }

            for (uint32_t i = 0; i < kCell; ++i) {
                zero = zero && section[reply.registerBase + i] == 0;
            }

            report.check("  its first page and its register table start zeroed", zero);
            section[0] = 0x5A;
            section[reply.actualSize - 1] = 0xA5;
            report.check("  its first and last bytes can be written",
                section[0] == 0x5A && section[reply.actualSize - 1] == 0xA5);
        }

        if (mapped) {
            markSection(section);
        }

        CloseHandle(device);

        if (mapped) {
            // Section 11: the close unmaps the section from the host.
            std::string unmapDetail;
            bool unmapped = sectionUnmapped(section, &unmapDetail);

            report.check("  the section is unmapped when the handle closes", unmapped, unmapDetail);
        }
    }
}

// The host half of a running stream: on every period, move each registered
// stream's position on and signal its event, as SarAsio does on each ASIO
// callback.
class Ticker
{
public:
    Ticker(BYTE *section, uint32_t registerBase, uint32_t periodBytes)
        : _section(section), _registerBase(registerBase), _periodBytes(periodBytes) {}

    ~Ticker() { stop(); }

    void add(HANDLE event, uint32_t index)
    {
        std::lock_guard<std::mutex> lock(_mutex);

        _streams.push_back({ event, index });
    }

    void start()
    {
        _thread = std::thread([this] {
            while (!_stop) {
                {
                    std::lock_guard<std::mutex> lock(_mutex);

                    for (const auto& stream : _streams) {
                        auto record = (volatile RegisterRecord *)(
                            _section + _registerBase + stream.index * sizeof(RegisterRecord));
                        uint32_t size = record->bufferSize;

                        if (size) {
                            record->positionRegister =
                                (record->positionRegister + _periodBytes * record->activeChannelCount) % size;
                        }

                        if (stream.event) {
                            SetEvent(stream.event);
                        }
                    }
                }

                Sleep(10);
            }
        });
    }

    void stop()
    {
        _stop = true;

        if (_thread.joinable()) {
            _thread.join();
        }
    }

private:
    struct Stream
    {
        HANDLE event;
        uint32_t index;
    };

    BYTE *_section;
    uint32_t _registerBase;
    uint32_t _periodBytes;
    std::mutex _mutex;
    std::vector<Stream> _streams;
    std::thread _thread;
    std::atomic<bool> _stop{ false };
};

void endpointAndStreamCases(Report& report, const std::wstring& path, const Args& args, bool legacy)
{
    logf("Endpoints, a stream and its notification events");

    HANDLE device = openControlDevice(path);
    GuardedPages pages;

    if (!report.check("the control device opens", device != INVALID_HANDLE_VALUE && pages.valid())) {
        return;
    }

    // An audio area that is a whole number of pages but not of cells: the
    // register table then does not start on a 64 KiB boundary, which is
    // where a view of a section has to start (A.6).
    //
    // Releases up to 0.13 cannot give a client the register table of such a
    // section (no stream opens on it), so for them the audio area is a whole
    // number of cells, as SarAsio's always is.
    LayoutRequest layoutRequest = validLayout(legacy ? (3 << 20) : (3 << 20) + 3 * kPage);
    LayoutReply layout = {};

    if (!report.spec(legacy ? "set buffer layout, 3 MiB" : "set buffer layout, 3 MiB and three pages",
            controlRequest(device, kSetBufferLayout, &layoutRequest, sizeof(layoutRequest),
                &layout, sizeof(layout)),
            kStatusSuccess, 16)) {
        CloseHandle(device);
        return;
    }

    BYTE *section = (BYTE *)(uintptr_t)layout.virtualAddress;

    report.spec("a second set buffer layout on the same handle",
        controlRequest(device, kSetBufferLayout, &layoutRequest, sizeof(layoutRequest),
            &layout, sizeof(layout)),
        kStatusInvalidStateTransition);

    EndpointLayout names;

    names.pairs = 1;
    names.channels = 2;
    names.prefix = L"SarConformance";

    EndpointRequest playback = validEndpoint(
        2, 0, 2, widen(names.playbackId(0)), names.playbackName(0));
    auto create = [&](void *input, uint32_t inputLength) {
        return controlRequest(device, kCreateEndpoint, input, inputLength, nullptr, 0, 60000);
    };

    report.spec("create endpoint, input one byte short",
        create(&playback, sizeof(playback) - 1), kStatusBufferOverflow, 0);

    BYTE *aligned = pages.endMinus(512);

    memcpy(aligned + 1, &playback, sizeof(playback));
    report.spec("create endpoint, input pointer off by one",
        create(aligned + 1, sizeof(playback)), kStatusDatatypeMisalignment, 0);
    report.spec("create endpoint, input in inaccessible memory",
        create(pages.inaccessible(), sizeof(playback)), kStatusAccessViolation);
    memcpy(pages.endMinus(64), &playback, 64);
    report.spec("create endpoint, input running into inaccessible memory",
        create(pages.endMinus(64), sizeof(playback)), kStatusAccessViolation);
#ifdef _WIN64
    report.spec("create endpoint, input pointer in kernel space",
        create(kernelAddress(), sizeof(playback)), kStatusAccessViolation);
#endif

    struct BadEndpoint
    {
        const char *name;
        uint32_t type, index, channels;
    };
    const BadEndpoint bad[] = {
        { "type 0", 0, 0, 2 }, { "type 3", 3, 0, 2 }, { "index 2340", 2, 2340, 2 },
        { "index 0xFFFFFFFF", 2, 0xFFFFFFFF, 2 }, { "channelCount 0", 2, 0, 0 },
        { "channelCount 33", 2, 0, 33 },
    };

    for (const auto& entry : bad) {
        EndpointRequest request = validEndpoint(
            entry.type, entry.index, entry.channels, L"SarConformance-bad", L"bad");

        report.spec(std::string("create endpoint, ") + entry.name,
            create(&request, sizeof(request)), kStatusInvalidParameter);
    }

    // The real endpoints. A request that passes the checks reports pending
    // first and finishes on a worker thread.
    ControlResult created = create(&playback, sizeof(playback));

    report.spec("create a playback endpoint (index 0, two channels)", created, kStatusSuccess);
    report.check("  it reported pending first", created.pended);

    EndpointRequest recording = validEndpoint(
        1, 1, 2, widen(names.recordingId(0)), names.recordingName(0));

    created = create(&recording, sizeof(recording));
    report.spec("create a recording endpoint (index 1, two channels)", created, kStatusSuccess);
    report.check("  it reported pending first", created.pended);

    // 64 units with no terminator: the driver ends each string at unit 64.
    EndpointRequest full = validEndpoint(2, 2, 1, L"", L"");

    for (int i = 0; i < 64; ++i) {
        full.id[i] = L"SarConformance-full-"[i % 20];
        full.name[i] = L'n';
    }

    report.spec("create an endpoint whose id and name fill all 64 units",
        create(&full, sizeof(full)), kStatusSuccess);

    EndpointRequest reused = validEndpoint(2, 0, 2, L"SarConformance-reused", L"SarConformance reused");

    report.deviation("create an endpoint with an index already in use",
        create(&reused, sizeof(reused)), kStatusSuccess, kStatusInvalidParameter);

    FoundEndpoints found;

    if (!report.check("Windows shows the two endpoints", findEndpoints(names, 30, &found))) {
        CloseHandle(device);
        return;
    }

    // Nothing is queued: a wait pends.
    NotificationRecord records[32] = {};
    ControlRequest firstWait;

    firstWait.start(device, kWaitNotifications, nullptr, 0, records, sizeof(records));
    report.check("wait for notification events pends while nothing is queued",
        !firstWait.wait(300) && firstWait.result().pended);

    // A shared-mode stream on the playback endpoint.
    IAudioClient *client = nullptr;
    IAudioRenderClient *render = nullptr;
    WAVEFORMATEX *mix = nullptr;
    UINT32 frames = 0;
    BYTE *data = nullptr;
    HRESULT hr = found.render[0]->Activate(
        __uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void **)&client);
    const char *stage = "Activate";

    if (SUCCEEDED(hr)) { stage = "GetMixFormat"; hr = client->GetMixFormat(&mix); }
    if (SUCCEEDED(hr)) {
        stage = "Initialize";
        hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, 10000000, 0, mix, nullptr);
    }
    if (SUCCEEDED(hr)) { stage = "GetBufferSize"; hr = client->GetBufferSize(&frames); }
    if (SUCCEEDED(hr)) {
        stage = "GetService";
        hr = client->GetService(__uuidof(IAudioRenderClient), (void **)&render);
    }
    if (SUCCEEDED(hr)) { stage = "GetBuffer"; hr = render->GetBuffer(frames, &data); }
    if (SUCCEEDED(hr)) {
        stage = "ReleaseBuffer";
        hr = render->ReleaseBuffer(frames, AUDCLNT_BUFFERFLAGS_SILENT);
    }
    if (SUCCEEDED(hr)) { stage = "Start"; hr = client->Start(); }

    bool streaming = report.check("a shared-mode stream opens and starts on the playback endpoint",
        SUCCEEDED(hr), SUCCEEDED(hr) ? "" : std::string(stage) + ": " + hresultText(hr));
    Ticker ticker(section, layout.registerBase, layoutRequest.periodSizeBytes);

    if (streaming) {
        // The audio engine registers an event for the stream; that completes
        // the pending wait with no data, the cue to ask again.
        bool woke = firstWait.wait(10000);

        report.spec("the pending wait completes when a client registers an event",
            firstWait.result(), kStatusSuccess, 0);

        if (woke) {
            ControlResult second = controlRequest(
                device, kWaitNotifications, nullptr, 0, records, sizeof(records), 1000);

            report.spec("the next wait returns the queued event at once", second, kStatusSuccess);

            uint64_t count = second.informationValid ? second.information / 16 : 0;

            report.check("  it reports whole records",
                second.informationValid && second.information >= 16 && second.information % 16 == 0,
                "information " + std::to_string(second.information));

            for (uint64_t i = 0; i < count; ++i) {
                HANDLE event = (HANDLE)(uintptr_t)records[i].handle;
                uint32_t index = (uint32_t)(records[i].associatedData >> 32);
                uint32_t access = grantedAccess(event);
                DWORD flags = 0;
                char detail[160];

                snprintf(detail, sizeof(detail), "handle 0x%llx index %u generation %u access 0x%08X",
                    (unsigned long long)records[i].handle, index,
                    (uint32_t)records[i].associatedData, access);
                report.check("  the record names the playback endpoint's index",
                    index == 0, detail);
                report.check("  its handle may signal and wait on the event and nothing else",
                    access == (EVENT_MODIFY_STATE | SYNCHRONIZE), detail);
                report.check("  its handle is not inheritable",
                    GetHandleInformation(event, &flags) && !(flags & HANDLE_FLAG_INHERIT));
                report.check("  its handle signals an event", SetEvent(event) != 0);
                ticker.add(event, index);
            }
        }

        ticker.start();
        Sleep(1000);

        RegisterRecord record = readRecord(section, layout.registerBase, 0);

        report.check("the stream's register record shows it running", (record.generation & 1) == 1,
            describe(record));
        report.check("  its buffer lies in whole cells inside the audio area",
            record.bufferSize > 0 && record.bufferOffset % kCell == 0 &&
            (uint64_t)record.bufferOffset + record.bufferSize <= layout.registerBase,
            describe(record));
        report.check("  its active channel count is the endpoint's",
            record.activeChannelCount == 2, describe(record));

        UINT32 padding = 0;

        hr = client->GetCurrentPadding(&padding);
        report.check("the stream is still valid after a second of ticks", SUCCEEDED(hr),
            SUCCEEDED(hr) ? "" : hresultText(hr));

        RegisterRecord before = record;

        client->Stop();
        render->Release();
        render = nullptr;
        client->Release();
        client = nullptr;

        bool stopped = false;

        for (int i = 0; i < 50 && !stopped; ++i) {
            Sleep(100);
            record = readRecord(section, layout.registerBase, 0);
            stopped = (record.generation & 1) == 0 && record.generation != before.generation;
        }

        report.check("closing the stream clears the running bit and moves the generation on",
            stopped, describe(record));

        // An exclusive-mode, event-driven stream. Here the client process
        // itself opens the pin, has the buffer and the position register
        // mapped into it and hands the driver its event, all of which the
        // audio engine's process does for a shared-mode stream. In a 32-bit
        // build of this program these are the driver's 32-bit paths.
        IAudioClient *exclusive = nullptr;
        IAudioRenderClient *exclusiveRender = nullptr;
        HANDLE clientEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        WAVEFORMATEXTENSIBLE native = {};
        REFERENCE_TIME period = 0, minimumPeriod = 0;

        native.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
        native.Format.nChannels = 2;
        native.Format.nSamplesPerSec = layoutRequest.sampleRate;
        native.Format.wBitsPerSample = (WORD)(layoutRequest.sampleSize * 8);
        native.Format.nBlockAlign = (WORD)(2 * layoutRequest.sampleSize);
        native.Format.nAvgBytesPerSec = native.Format.nBlockAlign * native.Format.nSamplesPerSec;
        native.Format.cbSize = 22;
        native.Samples.wValidBitsPerSample = native.Format.wBitsPerSample;
        native.dwChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
        native.SubFormat = KSDATAFORMAT_SUBTYPE_PCM;

        stage = "Activate";
        hr = found.render[0]->Activate(
            __uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void **)&exclusive);

        if (SUCCEEDED(hr)) {
            stage = "IsFormatSupported";
            hr = exclusive->IsFormatSupported(AUDCLNT_SHAREMODE_EXCLUSIVE, &native.Format, nullptr);
        }

        if (report.check("the endpoint takes its own format (32-bit PCM, 48 kHz, stereo) in exclusive mode",
                hr == S_OK, hr == S_OK ? "" : std::string(stage) + ": " + hresultText(hr))) {

            stage = "GetDevicePeriod";
            hr = exclusive->GetDevicePeriod(&period, &minimumPeriod);

            if (SUCCEEDED(hr)) {
                stage = "Initialize";
                hr = exclusive->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE,
                    AUDCLNT_STREAMFLAGS_EVENTCALLBACK, period, period, &native.Format, nullptr);
            }

            if (hr == AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED) {
                // The usual second attempt: ask for exactly the size the
                // device aligned the buffer to.
                UINT32 alignedFrames = 0;

                exclusive->GetBufferSize(&alignedFrames);
                exclusive->Release();
                exclusive = nullptr;
                period = (REFERENCE_TIME)(
                    10000000.0 * alignedFrames / native.Format.nSamplesPerSec + 0.5);
                hr = found.render[0]->Activate(
                    __uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void **)&exclusive);

                if (SUCCEEDED(hr)) {
                    hr = exclusive->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE,
                        AUDCLNT_STREAMFLAGS_EVENTCALLBACK, period, period, &native.Format, nullptr);
                }
            }

            if (SUCCEEDED(hr)) { stage = "SetEventHandle"; hr = exclusive->SetEventHandle(clientEvent); }
            if (SUCCEEDED(hr)) { stage = "GetBufferSize"; hr = exclusive->GetBufferSize(&frames); }
            if (SUCCEEDED(hr)) {
                stage = "GetService";
                hr = exclusive->GetService(__uuidof(IAudioRenderClient), (void **)&exclusiveRender);
            }
            if (SUCCEEDED(hr)) { stage = "GetBuffer"; hr = exclusiveRender->GetBuffer(frames, &data); }
            if (SUCCEEDED(hr)) {
                stage = "ReleaseBuffer";
                hr = exclusiveRender->ReleaseBuffer(frames, AUDCLNT_BUFFERFLAGS_SILENT);
            }
            if (SUCCEEDED(hr)) { stage = "Start"; hr = exclusive->Start(); }

            if (report.check("an exclusive-mode, event-driven stream opens and starts", SUCCEEDED(hr),
                    SUCCEEDED(hr) ? "" : std::string(stage) + ": " + hresultText(hr))) {

                // The client's event reaches the host through the driver; a
                // wait that pends is woken with no data and asked again.
                int received = 0;

                for (int attempt = 0; attempt < 5 && !received; ++attempt) {
                    ControlResult waited = controlRequest(
                        device, kWaitNotifications, nullptr, 0, records, sizeof(records), 2000);

                    if (waited.status != kStatusSuccess || !waited.informationValid) {
                        continue;
                    }

                    for (uint64_t i = 0; i + 16 <= waited.information; i += 16) {
                        if ((uint32_t)(records[i / 16].associatedData >> 32) == 0) {
                            ticker.add((HANDLE)(uintptr_t)records[i / 16].handle, 0);
                            received++;
                        }
                    }
                }

                report.check("the host is handed the event the client registered", received > 0,
                    std::to_string(received) + " notification record(s)");
                report.check("the client's event is signalled when the host signals its handle",
                    WaitForSingleObject(clientEvent, 3000) == WAIT_OBJECT_0);

                record = readRecord(section, layout.registerBase, 0);
                report.check("the exclusive stream's register record shows it running",
                    (record.generation & 1) == 1 && record.bufferSize > 0 &&
                    record.activeChannelCount == 2 &&
                    (uint64_t)record.bufferOffset + record.bufferSize <= layout.registerBase,
                    describe(record));

                // The client fills its buffer each time it is signalled.
                bool refilled = true;

                for (int i = 0; i < 20 && refilled; ++i) {
                    refilled = WaitForSingleObject(clientEvent, 2000) == WAIT_OBJECT_0 &&
                        SUCCEEDED(exclusiveRender->GetBuffer(frames, &data)) &&
                        SUCCEEDED(exclusiveRender->ReleaseBuffer(frames, AUDCLNT_BUFFERFLAGS_SILENT));
                }

                report.check("the client is signalled and refills its buffer twenty times", refilled);
                before = record;
                exclusive->Stop();
            }
        }

        if (exclusiveRender) { exclusiveRender->Release(); }
        if (exclusive) { exclusive->Release(); }

        if ((before.generation & 1) == 1) {
            stopped = false;

            for (int i = 0; i < 50 && !stopped; ++i) {
                Sleep(100);
                record = readRecord(section, layout.registerBase, 0);
                stopped = (record.generation & 1) == 0 && record.generation != before.generation;
            }

            report.check("closing the exclusive stream clears the running bit", stopped, describe(record));
        }

        CloseHandle(clientEvent);
    }

    ticker.stop();

    if (render) { render->Release(); }
    if (client) { client->Release(); }
    if (mix) { CoTaskMemFree(mix); }

    found.release();

    // Other request types on a control handle.
    {
        OVERLAPPED overlapped = {};
        BYTE byte = 0;
        DWORD read = 0;

        overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);

        BOOL ok = ReadFile(device, &byte, 1, &read, &overlapped);
        DWORD error = ok ? 0 : GetLastError();

        if (!ok && error == ERROR_IO_PENDING) {
            CancelIoEx(device, &overlapped);
            ok = GetOverlappedResult(device, &overlapped, &read, TRUE);
            error = ok ? 0 : GetLastError();
        }

        report.check("a read on a control handle is refused", !ok,
            "Win32 error " + std::to_string(error));
        CloseHandle(overlapped.hEvent);
    }

    if (args.has(L"registry-filter")) {
        // It can be started once in the driver's lifetime, by anyone.
        ControlResult first = controlRequest(device, kStartRegistryFilter, nullptr, 0, nullptr, 0);

        report.check("start registry filter succeeds or was already started",
            first.status == kStatusSuccess || first.status == kStatusResourceInUse,
            statusText(first.status));
        report.spec("start registry filter a second time",
            controlRequest(device, kStartRegistryFilter, nullptr, 0, nullptr, 0),
            kStatusResourceInUse);
    }

    // Closing the handle cancels a pending wait, withdraws the endpoints and
    // unmaps the section.
    ControlRequest lastWait;

    lastWait.start(device, kWaitNotifications, nullptr, 0, records, sizeof(records));

    bool pending = !lastWait.wait(300);

    markSection(section);
    CloseHandle(device);

    if (pending) {
        lastWait.wait(10000);
        report.spec("closing the handle cancels a pending wait", lastWait.result(), kStatusCancelled);
    } else {
        report.observe("a wait issued before the close (events were still queued)", lastWait.result());
    }

    report.check("the endpoints are gone once the handle is closed", waitForEndpointsGone(names, 30));

    std::string unmapDetail;
    bool unmapped = sectionUnmapped(section, &unmapDetail);

    report.check("the section is unmapped once the handle is closed", unmapped, unmapDetail);
}

} // namespace

int cmdConformance(const Args& args)
{
    bool legacy = args.has(L"legacy-quirks");
    Report report(legacy);
    std::wstring path = controlDevicePath();

    logf("Control interface conformance (%d-bit process%s)", (int)sizeof(void *) * 8,
        legacy ? ", expecting the quirks of releases up to 0.13" : "");

    if (report.check("the control device is present", !path.empty())) {
        accessCases(report, path);
        beforeLayoutCases(report, path);
        replyFailureCases(report, path);
        geometryCases(report, path, legacy);
        endpointAndStreamCases(report, path, args, legacy);
    }

    bool passed = report.passed();

    logf("%s: %d failed", passed ? "PASSED" : "FAILED", report.failures());

    JsonObject json;

    json.setString("command", "conformance")
        .setBool("passed", passed)
        .setBool("legacyQuirks", legacy)
        .setInt("processBits", (long long)sizeof(void *) * 8)
        .setInt("failures", report.failures())
        .setRaw("cases", report.toJson());

    if (args.has(L"results")) {
        writeTextFile(args.get(L"results"), json.str());
    }

    return passed ? 0 : 2;
}

} // namespace SarTest
