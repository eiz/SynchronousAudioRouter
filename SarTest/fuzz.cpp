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


#include "fuzz.h"
#include "control.h"
#include "install.h"

#include <winioctl.h>
#include <winternl.h>
#include <cfgmgr32.h>
#include <mmreg.h>
#include <ks.h>
#include <ksmedia.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <thread>

namespace SarTest {

namespace {

const uint32_t kPageSize = 4096;

// xorshift64*: small, fast, and the same sequence for the same seed.
class Rng
{
public:
    explicit Rng(uint64_t seed) : _state(seed * 0x9E3779B97F4A7C15ull + 0xD1B54A32D192ED03ull)
    {
        if (!_state) {
            _state = 1;
        }
    }

    uint64_t next64()
    {
        _state ^= _state >> 12;
        _state ^= _state << 25;
        _state ^= _state >> 27;
        return _state * 0x2545F4914F6CDD1Dull;
    }

    uint32_t next() { return (uint32_t)(next64() >> 32); }
    uint32_t below(uint32_t n) { return n ? next() % n : 0; }
    bool chance(uint32_t percent) { return below(100) < percent; }

    // Values at and around the limits the driver checks, and a few wild ones.
    uint32_t interesting()
    {
        static const uint32_t values[] = {
            0, 1, 2, 3, 4, 5, 7, 8, 15, 16, 20, 24, 31, 32, 33, 40, 63, 64, 65, 104, 255, 256,
            267, 268, 269, 2339, 2340, 2341, 4095, 4096, 4097, 7999, 8000, 44100, 48000,
            65535, 65536, 65537, 192000, 192001, 1048576, 16777216, 134217727, 134217728,
            134217729, 0x7FFFFFFF, 0x80000000, 0x80000001, 0xFFFFFFFC, 0xFFFFFFFE, 0xFFFFFFFF,
        };

        return chance(85) ? values[below(ARRAYSIZE(values))] : next();
    }

private:
    uint64_t _state;
};

// A thread's memory for request buffers: readable and writable pages, then a
// read-only page, then an inaccessible one.
class Arena
{
public:
    static const uint32_t kWritable = 16 * kPageSize;

    Arena()
    {
        _base = (BYTE *)VirtualAlloc(
            nullptr, kWritable + 2 * kPageSize, MEM_RESERVE, PAGE_NOACCESS);

        if (_base) {
            VirtualAlloc(_base, kWritable, MEM_COMMIT, PAGE_READWRITE);
            VirtualAlloc(_base + kWritable, kPageSize, MEM_COMMIT, PAGE_READWRITE);
            memset(_base + kWritable, 0x41, kPageSize);

            DWORD old = 0;

            VirtualProtect(_base + kWritable, kPageSize, PAGE_READONLY, &old);
        }
    }

    ~Arena()
    {
        if (_base) {
            VirtualFree(_base, 0, MEM_RELEASE);
        }
    }

    BYTE *writable() const { return _base; }
    BYTE *readOnly() const { return _base + kWritable; }
    BYTE *inaccessible() const { return _base + kWritable + kPageSize; }

private:
    BYTE *_base = nullptr;
};

struct Placement
{
    void *pointer;
    uint32_t length;
};

// Where a buffer of `length` bytes holding `data` is put, and what length is
// claimed for it. Mostly somewhere sound; sometimes misaligned, running into
// read-only or inaccessible memory, null, or (64-bit) in kernel space.
Placement place(Rng& rng, const Arena& arena, const BYTE *data, uint32_t length, bool forOutput)
{
    Placement placement;
    uint32_t stored = length < 8 * kPageSize ? length : 8 * kPageSize;
    BYTE *sound = arena.writable() + (forOutput ? 8 * kPageSize : 0);
    uint32_t how = rng.below(100);

    if (data && stored) {
        memcpy(sound, data, stored);
    }

    placement.pointer = sound;
    placement.length = length;

    if (how < 62) {
        // Sound.
    } else if (how < 70) {
        placement.pointer = sound + 1 + rng.below(3);

        if (data && stored) {
            memmove(placement.pointer, sound, stored);
        }
    } else if (how < 78) {
        // Ends exactly at the read-only page.
        BYTE *end = arena.readOnly();
        uint32_t fit = stored < 8 * kPageSize ? stored : 8 * kPageSize;

        placement.pointer = end - fit;

        if (data && fit) {
            memcpy(placement.pointer, data, fit);
        }
    } else if (how < 84) {
        // Starts in writable memory and runs through the read-only page into
        // the inaccessible one.
        placement.pointer = arena.readOnly() - 8;
        placement.length = length < 16 ? 16 + kPageSize : length + kPageSize;
    } else if (how < 88) {
        placement.pointer = arena.readOnly();
    } else if (how < 92) {
        placement.pointer = arena.inaccessible();
    } else if (how < 95) {
        placement.pointer = nullptr;
    } else if (how < 97) {
        placement.length = rng.interesting();
    } else {
#ifdef _WIN64
        placement.pointer = (void *)(0xFFFF800000000000ull + ((uint64_t)rng.next() << 12));
#else
        placement.pointer = (void *)(uintptr_t)(0x7FFF0000u + (rng.next() & 0xFFFC));
#endif
    }

    return placement;
}

struct Shared
{
    std::atomic<bool> stop{ false };
    std::wstring controlPath;
    std::wstring instanceId;
    HANDLE token = nullptr;         // impersonated by the workers when set
    uint64_t seed = 0;

    std::mutex mutex;
    std::vector<std::wstring> filterPaths;      // enabled SAR KS interfaces
    std::map<uint32_t, long long> statuses;

    std::atomic<long long> controlRequests{ 0 };
    std::atomic<long long> controlOpens{ 0 };
    std::atomic<long long> endpointsCreated{ 0 };
    std::atomic<long long> layoutsSet{ 0 };
    std::atomic<long long> ksRequests{ 0 };
    std::atomic<long long> filterOpens{ 0 };
    std::atomic<long long> filterOpenFailures{ 0 };
    std::atomic<long long> pinCreates{ 0 };
    std::atomic<long long> pinsCreated{ 0 };
    std::atomic<long long> hostRestarts{ 0 };
    std::atomic<long long> timeouts{ 0 };

    void count(uint32_t status)
    {
        std::lock_guard<std::mutex> lock(mutex);

        statuses[status]++;
    }
};

struct Worker
{
    const char *kind = "";
    int index = 0;
    std::atomic<double> lastProgressMs{ 0.0 };
    std::atomic<const char *> stage{ "starting" };
    std::thread thread;

    void at(const char *where)
    {
        stage = where;
        lastProgressMs = nowMs();
    }
};

void impersonate(const Shared& shared)
{
    if (shared.token && !ImpersonateLoggedOnUser(shared.token)) {
        logf("ImpersonateLoggedOnUser failed: %s", lastErrorText(GetLastError()).c_str());
    }
}

// A token like the process's own, but with the Administrators group reduced
// to deny-only, every privilege it can lose removed, and medium integrity:
// what a program the user runs without elevation has.
HANDLE restrictedToken()
{
    HANDLE process = nullptr;
    HANDLE restricted = nullptr;
    HANDLE impersonation = nullptr;
    BYTE adminSid[SECURITY_MAX_SID_SIZE];
    BYTE mediumSid[SECURITY_MAX_SID_SIZE];
    DWORD size = sizeof(adminSid);

    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ALL_ACCESS, &process)) {
        return nullptr;
    }

    CreateWellKnownSid(WinBuiltinAdministratorsSid, nullptr, adminSid, &size);

    SID_AND_ATTRIBUTES deny = { adminSid, 0 };

    if (CreateRestrictedToken(process, DISABLE_MAX_PRIVILEGE, 1, &deny, 0, nullptr, 0, nullptr,
            &restricted)) {

        TOKEN_MANDATORY_LABEL label = {};

        size = sizeof(mediumSid);
        CreateWellKnownSid(WinMediumLabelSid, nullptr, mediumSid, &size);
        label.Label.Sid = mediumSid;
        label.Label.Attributes = SE_GROUP_INTEGRITY;
        SetTokenInformation(restricted, TokenIntegrityLevel, &label,
            sizeof(label) + GetLengthSid(mediumSid));
        DuplicateTokenEx(restricted, TOKEN_ALL_ACCESS, nullptr, SecurityImpersonation,
            TokenImpersonation, &impersonation);
        CloseHandle(restricted);
    }

    CloseHandle(process);
    return impersonation;
}

std::vector<std::wstring> enabledFilterPaths(const std::wstring& instanceId)
{
    std::vector<std::wstring> result;
    ULONG length = 0;
    GUID category = KSCATEGORY_AUDIO;

    if (instanceId.empty() ||
        CM_Get_Device_Interface_List_SizeW(&length, &category, (DEVINSTID_W)instanceId.c_str(),
            CM_GET_DEVICE_INTERFACE_LIST_PRESENT) != CR_SUCCESS || length == 0) {
        return result;
    }

    std::vector<wchar_t> buffer(length + 1, L'\0');

    if (CM_Get_Device_Interface_ListW(&category, (DEVINSTID_W)instanceId.c_str(), buffer.data(),
            length, CM_GET_DEVICE_INTERFACE_LIST_PRESENT) != CR_SUCCESS) {
        return result;
    }

    for (const wchar_t *p = buffer.data(); *p; p += wcslen(p) + 1) {
        result.push_back(p);
    }

    return result;
}

// --- Control requests ------------------------------------------------------

const wchar_t *const kHostileNames[] = {
    L"", L"\\", L"/", L"..\\..", L"a\\b", L"a/b", L"{0EB287D4-6C04-4926-AE19-3C066A4C3F3A}",
    L"x_2_48000_4", L"x_2_48000_4_topology", L"_topology", L"%s%n%x", L"\x0001\x0002",
    L"\xD800", L"\xDC00\xD800", L"\xFFFF\xFFFE", L"nul", L"con", L" ", L"a b", L"a\tb",
};

void fillName(Rng& rng, wchar_t *field, const wchar_t *prefix)
{
    uint32_t how = rng.below(100);

    memset(field, 0, 64 * sizeof(wchar_t));

    if (how < 70) {
        // A small set, so that names collide across handles and threads.
        swprintf_s(field, 64, L"%s-%c", prefix, L'a' + (wchar_t)rng.below(12));
    } else if (how < 85) {
        wcsncpy_s(field, 64, kHostileNames[rng.below(ARRAYSIZE(kHostileNames))], _TRUNCATE);
    } else if (how < 93) {
        // All 64 units used, no terminator.
        for (int i = 0; i < 64; ++i) {
            field[i] = (wchar_t)(L'A' + rng.below(26));
        }
    } else {
        for (int i = 0; i < 64; ++i) {
            field[i] = (wchar_t)rng.next();
        }
    }
}

void controlWorker(Shared& shared, Worker& self)
{
    Rng rng(shared.seed * 1000 + self.index);
    Arena arena;
    std::vector<HANDLE> handles;
    std::vector<std::unique_ptr<ControlRequest>> pending;

    impersonate(shared);

    auto reap = [&](bool all) {
        for (size_t i = 0; i < pending.size();) {
            if (pending[i]->wait(0)) {
                shared.count(pending[i]->result().status);
                pending.erase(pending.begin() + i);
            } else {
                if (all || rng.chance(30)) {
                    pending[i]->cancel();
                }

                ++i;
            }
        }

        if (all) {
            self.at("waiting for cancelled control requests");
            pending.clear();    // each destructor waits for its request
        }
    };

    while (!shared.stop) {
        uint32_t action = rng.below(100);

        self.at("control: choosing");

        if (handles.empty() || (action < 6 && handles.size() < 3)) {
            DWORD access = rng.chance(90) ? GENERIC_READ | GENERIC_WRITE :
                rng.chance(50) ? GENERIC_READ : GENERIC_WRITE;

            self.at("control: open");

            HANDLE handle = openControlDevice(shared.controlPath, access);

            if (handle != INVALID_HANDLE_VALUE) {
                handles.push_back(handle);
                shared.controlOpens++;
            } else {
                Sleep(10);
            }

            continue;
        }

        size_t which = rng.below((uint32_t)handles.size());
        HANDLE handle = handles[which];

        if (action < 12) {
            // Close with whatever is still pending on it.
            self.at("control: close");
            CloseHandle(handle);
            handles.erase(handles.begin() + which);
            continue;
        }

        if (action < 20) {
            reap(false);

            if (pending.size() > 24) {
                reap(true);
            }

            continue;
        }

        BYTE input[512] = {};
        uint32_t inputLength = 0;
        uint32_t outputLength = 0;
        uint32_t code = 0;
        uint32_t request = rng.below(100);
        bool sane = rng.chance(55);

        if (request < 30) {
            LayoutRequest layout = {};
            static const uint32_t sizes[] = { 65536, 1048576, 3158016, 16777216 };

            code = kSetBufferLayout;
            layout.bufferSize = sane ? sizes[rng.below(ARRAYSIZE(sizes))] : rng.interesting();
            layout.sampleRate = sane ? 48000 : rng.interesting();
            layout.sampleSize = sane ? 1 + rng.below(4) : rng.interesting();
            layout.periodSizeBytes = sane ? 480 * layout.sampleSize : rng.interesting();
            layout.minimumFrameCount = rng.interesting();
            memcpy(input, &layout, sizeof(layout));
            inputLength = sizeof(layout);
            outputLength = sizeof(LayoutReply);
        } else if (request < 65) {
            EndpointRequest endpoint = {};

            code = kCreateEndpoint;
            endpoint.type = sane ? 1 + rng.below(2) : rng.interesting();
            endpoint.index = sane ? rng.below(6) : rng.interesting();
            endpoint.channelCount = sane ? 1 + rng.below(4) : rng.interesting();
            fillName(rng, endpoint.id, L"SarFuzz");
            fillName(rng, endpoint.name, L"SarFuzz name");
            memcpy(input, &endpoint, sizeof(endpoint));
            inputLength = sizeof(endpoint);
        } else if (request < 85) {
            code = kWaitNotifications;
            outputLength = rng.chance(70) ? 16 * (1 + rng.below(32)) : rng.interesting() % 1024;
        } else if (request < 88) {
            code = kStartRegistryFilter;
        } else {
            // Any function, method and access; mostly this device type.
            uint32_t type = rng.chance(80) ? 0x22 : rng.chance(50) ? 0x2F : rng.below(0x10000);

            code = (type << 16) | (rng.below(4) << 14) | (rng.below(rng.chance(80) ? 8 : 4096) << 2) |
                rng.below(4);
            inputLength = rng.interesting() % 512;
            outputLength = rng.interesting() % 512;

            for (uint32_t i = 0; i < sizeof(input); ++i) {
                input[i] = (BYTE)rng.next();
            }
        }

        if (!sane) {
            // Lengths around the real ones, and a few bytes changed.
            if (rng.chance(40)) {
                inputLength = rng.chance(60) ? inputLength + rng.below(5) - 2 : rng.interesting() % 600;
            }

            if (rng.chance(40)) {
                outputLength = rng.chance(60) ? outputLength + rng.below(5) - 2 : rng.interesting() % 600;
            }

            for (int i = (int)rng.below(4); i > 0; --i) {
                input[rng.below(sizeof(input))] = (BYTE)rng.next();
            }
        }

        if (inputLength > 0x80000000u) {
            inputLength = 0;
        }

        Placement in = { input, inputLength };
        Placement out = { arena.writable() + 8 * kPageSize, outputLength };

        if (!sane || rng.chance(10)) {
            in = place(rng, arena, input, inputLength, false);
            out = place(rng, arena, nullptr, outputLength, true);
        }

        self.at("control: request");

        auto issued = std::make_unique<ControlRequest>();

        issued->start(handle, code, in.pointer, in.length, out.pointer, out.length);
        shared.controlRequests++;

        // Endpoint creation and waits finish later; give the others a moment
        // and keep whatever is still going.
        if (issued->wait(code == kCreateEndpoint && rng.chance(50) ? 2000 : 20)) {
            ControlResult result = issued->result();

            shared.count(result.status);

            if (result.status == kStatusSuccess) {
                if (code == kCreateEndpoint) {
                    shared.endpointsCreated++;
                } else if (code == kSetBufferLayout) {
                    shared.layoutsSet++;
                } else if (code == kWaitNotifications && result.informationValid) {
                    // Handles the driver put in this process.
                    auto records = (NotificationRecord *)out.pointer;

                    for (uint64_t i = 0; i + 16 <= result.information && i < 16 * 32; i += 16) {
                        HANDLE event = (HANDLE)(uintptr_t)records[i / 16].handle;

                        if (out.pointer == arena.writable() + 8 * kPageSize && event) {
                            SetEvent(event);
                            CloseHandle(event);
                        }
                    }
                }
            }
        } else {
            pending.push_back(std::move(issued));
        }
    }

    self.at("control: closing handles");

    for (HANDLE handle : handles) {
        CloseHandle(handle);
    }

    reap(true);
    self.at("done");
}

// --- A host that keeps real endpoints alive, and takes them away -----------

void hostWorker(Shared& shared, Worker& self)
{
    Rng rng(shared.seed * 1000 + 500 + self.index);

    while (!shared.stop) {
        self.at("host: open");

        HANDLE device = openControlDevice(shared.controlPath);

        if (device == INVALID_HANDLE_VALUE) {
            Sleep(100);
            continue;
        }

        LayoutRequest layout = {};
        LayoutReply reply = {};

        layout.bufferSize = 16 << 20;
        layout.sampleRate = 48000;
        layout.sampleSize = 4;
        layout.periodSizeBytes = 480 * 4;
        layout.minimumFrameCount = 2;
        self.at("host: set layout");

        ControlResult result = controlRequest(
            device, kSetBufferLayout, &layout, sizeof(layout), &reply, sizeof(reply));

        if (result.status == kStatusSuccess) {
            for (uint32_t i = 0; i < 4 && !shared.stop; ++i) {
                EndpointRequest endpoint = {};

                endpoint.type = 1 + (i & 1);
                endpoint.index = i;
                endpoint.channelCount = 2;
                swprintf_s(endpoint.id, L"SarFuzzHost-%u", i);
                swprintf_s(endpoint.name, L"SarFuzzHost %u", i);
                self.at("host: create endpoint");
                shared.count(controlRequest(
                    device, kCreateEndpoint, &endpoint, sizeof(endpoint), nullptr, 0, 60000).status);
            }
        }

        // Signal whatever streams the other threads manage to start, for a
        // while; then take everything away underneath them.
        NotificationRecord records[32];
        std::vector<HANDLE> events;
        double until = nowMs() + 1500 + rng.below(4000);

        while (!shared.stop && nowMs() < until) {
            self.at("host: ticking");

            ControlResult waited = controlRequest(
                device, kWaitNotifications, nullptr, 0, records, sizeof(records), 50);

            if (waited.status == kStatusSuccess && waited.informationValid) {
                for (uint64_t i = 0; i + 16 <= waited.information; i += 16) {
                    events.push_back((HANDLE)(uintptr_t)records[i / 16].handle);
                }
            }

            for (HANDLE event : events) {
                SetEvent(event);
            }
        }

        self.at("host: close");
        CloseHandle(device);

        for (HANDLE event : events) {
            CloseHandle(event);
        }

        shared.hostRestarts++;
    }

    self.at("done");
}

// --- Kernel streaming requests ----------------------------------------------

typedef LONG (NTAPI *NtCreateFileFn)(
    HANDLE *file, ACCESS_MASK access, OBJECT_ATTRIBUTES *attributes, IO_STATUS_BLOCK *status,
    LARGE_INTEGER *allocationSize, ULONG fileAttributes, ULONG shareAccess,
    ULONG createDisposition, ULONG createOptions, void *ea, ULONG eaLength);

NtCreateFileFn ntCreateFile()
{
    static NtCreateFileFn fn = (NtCreateFileFn)GetProcAddress(
        GetModuleHandleW(L"ntdll.dll"), "NtCreateFile");

    return fn;
}

// The request that opens a pin: the pin's create name, then a KSPIN_CONNECT
// and a data format as raw bytes.
std::vector<BYTE> pinConnectBlob(Rng& rng, uint32_t channels, bool sane)
{
    struct Blob
    {
        KSPIN_CONNECT connect;
        KSDATAFORMAT format;
        WAVEFORMATEXTENSIBLE wave;
    };
    Blob blob = {};

    blob.connect.Interface.Set = KSINTERFACESETID_Standard;
    blob.connect.Interface.Id = KSINTERFACE_STANDARD_LOOPED_STREAMING;
    blob.connect.Medium.Set = KSMEDIUMSETID_Standard;
    blob.connect.Medium.Id = KSMEDIUM_TYPE_ANYINSTANCE;
    blob.connect.PinId = 0;
    blob.connect.Priority.PriorityClass = KSPRIORITY_NORMAL;
    blob.connect.Priority.PrioritySubClass = 1;
    blob.format.FormatSize = sizeof(KSDATAFORMAT) + sizeof(WAVEFORMATEXTENSIBLE);
    blob.format.MajorFormat = KSDATAFORMAT_TYPE_AUDIO;
    blob.format.SubFormat = KSDATAFORMAT_SUBTYPE_PCM;
    blob.format.Specifier = KSDATAFORMAT_SPECIFIER_WAVEFORMATEX;
    blob.wave.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    blob.wave.Format.nChannels = (WORD)channels;
    blob.wave.Format.nSamplesPerSec = 48000;
    blob.wave.Format.wBitsPerSample = 32;
    blob.wave.Format.nBlockAlign = (WORD)(4 * channels);
    blob.wave.Format.nAvgBytesPerSec = 48000 * 4 * channels;
    blob.wave.Format.cbSize = 22;
    blob.wave.Samples.wValidBitsPerSample = 32;
    blob.wave.SubFormat = KSDATAFORMAT_SUBTYPE_PCM;
    blob.format.SampleSize = blob.wave.Format.nBlockAlign;

    std::vector<BYTE> bytes((BYTE *)&blob, (BYTE *)&blob + sizeof(blob));

    if (!sane) {
        uint32_t how = rng.below(100);

        if (how < 25) {
            // One field at a time: these are the ones a driver validates.
            switch (rng.below(10)) {
            case 0: blob.connect.PinId = rng.interesting(); break;
            case 1: blob.connect.Interface.Id = rng.interesting(); break;
            case 2: blob.format.FormatSize = rng.interesting(); break;
            case 3: blob.wave.Format.nChannels = (WORD)rng.interesting(); break;
            case 4: blob.wave.Format.nSamplesPerSec = rng.interesting(); break;
            case 5: blob.wave.Format.wBitsPerSample = (WORD)rng.interesting(); break;
            case 6: blob.wave.Format.cbSize = (WORD)rng.interesting(); break;
            case 7: blob.wave.Format.nBlockAlign = (WORD)rng.interesting(); break;
            case 8: blob.connect.PinToHandle = (HANDLE)(uintptr_t)rng.next64(); break;
            default: blob.wave.Format.wFormatTag = (WORD)rng.interesting(); break;
            }

            bytes.assign((BYTE *)&blob, (BYTE *)&blob + sizeof(blob));
        } else if (how < 50) {
            for (int i = 1 + (int)rng.below(6); i > 0; --i) {
                bytes[rng.below((uint32_t)bytes.size())] = (BYTE)rng.next();
            }
        } else if (how < 75) {
            bytes.resize(rng.below((uint32_t)bytes.size() + 1) & ~1u);
        } else {
            size_t extra = 2 * (1 + rng.below(2048));

            bytes.resize(bytes.size() + extra);

            for (size_t i = sizeof(blob); i < bytes.size(); ++i) {
                bytes[i] = (BYTE)rng.next();
            }
        }
    }

    return bytes;
}

HANDLE createPin(HANDLE filter, const std::vector<BYTE>& blob, uint32_t *status)
{
    std::vector<wchar_t> name;
    const wchar_t *prefix = KSSTRING_Pin L"\\";

    name.assign(prefix, prefix + wcslen(prefix));
    name.resize(name.size() + (blob.size() + 1) / 2);
    memcpy(name.data() + wcslen(prefix), blob.data(), blob.size());

    UNICODE_STRING string = {};
    OBJECT_ATTRIBUTES attributes = {};
    IO_STATUS_BLOCK iosb = {};
    HANDLE pin = nullptr;

    string.Buffer = name.data();
    string.Length = (USHORT)(name.size() * sizeof(wchar_t) > 0xFFFE ? 0xFFFE : name.size() * sizeof(wchar_t));
    string.MaximumLength = string.Length;
    InitializeObjectAttributes(&attributes, &string, OBJ_CASE_INSENSITIVE, filter, nullptr);
    *status = (uint32_t)ntCreateFile()(
        &pin, GENERIC_READ | GENERIC_WRITE, &attributes, &iosb, nullptr, 0, 0,
        1 /* FILE_OPEN */, 0, nullptr, 0);
    return (LONG)*status >= 0 ? pin : nullptr;
}

uint32_t channelsOf(const std::wstring& path)
{
    // "...\<id>_<channels>_<rate>_<sample size>[_topology]"
    std::wstring reference = path.substr(path.find_last_of(L'\\') + 1);
    std::vector<std::wstring> parts;
    size_t start = 0;

    for (size_t i = 0; i <= reference.size(); ++i) {
        if (i == reference.size() || reference[i] == L'_') {
            parts.push_back(reference.substr(start, i - start));
            start = i + 1;
        }
    }

    size_t numbers = parts.size() >= 4 && parts.back() == L"topology" ? parts.size() - 4 :
        parts.size() >= 3 ? parts.size() - 3 : 0;
    int channels = parts.size() >= 3 ? _wtoi(parts[numbers].c_str()) : 0;

    return channels >= 1 && channels <= 32 ? (uint32_t)channels : 2;
}

void ksWorker(Shared& shared, Worker& self)
{
    Rng rng(shared.seed * 1000 + 900 + self.index);
    Arena arena;
    std::vector<HANDLE> filters;
    std::vector<HANDLE> pins;
    HANDLE ownEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    HANDLE notEvent = GetCurrentThread();
    const GUID sets[] = {
        KSPROPSETID_Pin, KSPROPSETID_Topology, KSPROPSETID_Connection, KSPROPSETID_General,
        KSPROPSETID_Audio, KSPROPSETID_RtAudio, KSPROPSETID_Jack, KSEVENTSETID_PinCapsChange,
        KSPROPSETID_Stream, KSPROPSETID_StreamAllocator,
    };
    const uint32_t codes[] = {
        IOCTL_KS_PROPERTY, IOCTL_KS_PROPERTY, IOCTL_KS_PROPERTY, IOCTL_KS_PROPERTY,
        IOCTL_KS_ENABLE_EVENT, IOCTL_KS_DISABLE_EVENT, IOCTL_KS_METHOD,
        IOCTL_KS_WRITE_STREAM, IOCTL_KS_READ_STREAM, IOCTL_KS_RESET_STATE,
    };
    const uint32_t flags[] = {
        KSPROPERTY_TYPE_GET, KSPROPERTY_TYPE_SET, KSPROPERTY_TYPE_GET | KSPROPERTY_TYPE_TOPOLOGY,
        KSPROPERTY_TYPE_SET | KSPROPERTY_TYPE_TOPOLOGY, KSPROPERTY_TYPE_BASICSUPPORT,
        KSPROPERTY_TYPE_SETSUPPORT, KSPROPERTY_TYPE_RELATIONS, KSPROPERTY_TYPE_DEFAULTVALUES,
        KSPROPERTY_TYPE_SERIALIZESET, KSPROPERTY_TYPE_UNSERIALIZESET, KSPROPERTY_TYPE_SERIALIZERAW,
        KSPROPERTY_TYPE_GET | KSPROPERTY_TYPE_SET, 0,
    };

    impersonate(shared);

    auto closeOne = [&](std::vector<HANDLE>& list) {
        if (!list.empty()) {
            size_t which = rng.below((uint32_t)list.size());

            CloseHandle(list[which]);
            list.erase(list.begin() + which);
        }
    };

    while (!shared.stop) {
        uint32_t action = rng.below(100);

        self.at("ks: choosing");

        if (filters.empty() || (action < 10 && filters.size() < 4)) {
            std::wstring path;

            {
                std::lock_guard<std::mutex> lock(shared.mutex);

                if (!shared.filterPaths.empty()) {
                    path = shared.filterPaths[rng.below((uint32_t)shared.filterPaths.size())];
                }
            }

            if (path.empty()) {
                Sleep(20);
                continue;
            }

            self.at("ks: open filter");

            HANDLE filter = CreateFileW(path.c_str(),
                rng.chance(85) ? GENERIC_READ | GENERIC_WRITE : 0,
                FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED, nullptr);

            if (filter == INVALID_HANDLE_VALUE) {
                shared.filterOpenFailures++;
                Sleep(5);
                continue;
            }

            shared.filterOpens++;
            filters.push_back(filter);

            if (rng.chance(60)) {
                uint32_t status = 0;
                bool sane = rng.chance(50);

                self.at("ks: create pin");

                HANDLE pin = createPin(filter, pinConnectBlob(rng, channelsOf(path), sane), &status);

                shared.pinCreates++;
                shared.count(status);

                if (pin) {
                    shared.pinsCreated++;
                    pins.push_back(pin);
                }
            }

            continue;
        }

        if (action < 16) {
            self.at("ks: close filter");
            closeOne(filters);
            continue;
        }

        if (action < 24 || pins.size() > 4) {
            self.at("ks: close pin");
            closeOne(pins);
            continue;
        }

        bool onPin = !pins.empty() && rng.chance(60);
        HANDLE target = onPin ? pins[rng.below((uint32_t)pins.size())] :
            filters[rng.below((uint32_t)filters.size())];
        BYTE input[128] = {};
        uint32_t inputLength = 0;
        uint32_t outputLength = 0;
        uint32_t code = codes[rng.below(ARRAYSIZE(codes))];
        bool sane = rng.chance(50);
        auto property = (KSPROPERTY *)input;

        property->Set = rng.chance(95) ? sets[rng.below(ARRAYSIZE(sets))] : GUID{ rng.next() };
        property->Id = rng.chance(90) ? rng.below(24) : rng.interesting();
        property->Flags = rng.chance(90) ? flags[rng.below(ARRAYSIZE(flags))] : rng.next();
        inputLength = sizeof(KSPROPERTY);

        if (onPin && rng.chance(70)) {
            // The pin's own properties, with the fields a driver has to
            // distrust set to what it has to refuse.
            property->Set = rng.chance(75) ? KSPROPSETID_RtAudio : KSPROPSETID_Connection;
            property->Id = rng.below(rng.chance(90) ? 12 : 40);
            property->Flags = rng.chance(75) ? KSPROPERTY_TYPE_GET : KSPROPERTY_TYPE_SET;

            const uint64_t handleValues[] = {
                (uint64_t)(uintptr_t)ownEvent, (uint64_t)(uintptr_t)notEvent, 0, ~0ull, ~0ull - 1,
                0xFFFFFFFF80000004ull, 0x80000004ull, 4, 0x1234, rng.next64(),
                (uint64_t)(uintptr_t)target,
            };
            uint64_t tail[3] = {
                rng.chance(60) ? handleValues[rng.below(ARRAYSIZE(handleValues))] : 0,
                rng.chance(50) ? rng.interesting() : 0,
                rng.interesting(),
            };

            memcpy(input + sizeof(KSPROPERTY), tail, sizeof(tail));
            inputLength = sizeof(KSPROPERTY) + 8 * rng.below(4);

            if (property->Set == KSPROPSETID_Connection && property->Id == KSPROPERTY_CONNECTION_STATE &&
                rng.chance(70)) {

                // Move the stream through its states, valid orders and not.
                ULONG state = rng.chance(90) ? rng.below(4) : rng.interesting();

                property->Flags = KSPROPERTY_TYPE_SET;
                memcpy(arena.writable() + 8 * kPageSize, &state, sizeof(state));
                inputLength = sizeof(KSPROPERTY);
                outputLength = sizeof(state);
                sane = true;
            }
        } else if (rng.chance(60)) {
            // KSP_PIN / KSNODEPROPERTY: a pin or node number follows.
            uint32_t tail[2] = { rng.chance(80) ? rng.below(4) : rng.interesting(), rng.chance(90) ? 0 : rng.next() };

            memcpy(input + sizeof(KSPROPERTY), tail, sizeof(tail));
            inputLength = sizeof(KSPROPERTY) + sizeof(tail);
        }

        if (!outputLength) {
            static const uint32_t sizes[] = { 0, 1, 2, 4, 8, 12, 16, 24, 40, 104, 512, 4096 };

            outputLength = sizes[rng.below(ARRAYSIZE(sizes))];
        }

        if (!sane) {
            if (rng.chance(40)) {
                inputLength = rng.below(sizeof(input) + 1);
            }

            for (int i = (int)rng.below(3); i > 0; --i) {
                input[rng.below(sizeof(input))] = (BYTE)rng.next();
            }
        }

        Placement in = { input, inputLength };
        Placement out = { arena.writable() + 8 * kPageSize, outputLength };

        if (!sane) {
            in = place(rng, arena, input, inputLength, false);
            out = place(rng, arena, nullptr, outputLength, true);
        }

        self.at("ks: request");

        ControlRequest request;

        request.start(target, code, in.pointer, in.length, out.pointer, out.length);
        shared.ksRequests++;

        if (!request.wait(1000)) {
            shared.timeouts++;
            self.at("ks: cancelling a request that did not finish");
            request.cancel();
            request.wait(INFINITE);
        }

        shared.count(request.result().status);
    }

    self.at("ks: closing handles");

    // Filters before their pins, half the time.
    if (rng.chance(50)) {
        for (HANDLE filter : filters) { CloseHandle(filter); }
        for (HANDLE pin : pins) { CloseHandle(pin); }
    } else {
        for (HANDLE pin : pins) { CloseHandle(pin); }
        for (HANDLE filter : filters) { CloseHandle(filter); }
    }

    CloseHandle(ownEvent);
    self.at("done");
}

} // namespace

int cmdFuzz(const Args& args)
{
    Shared shared;
    double seconds = args.getDouble(L"seconds", 60.0);
    int controlThreads = args.getInt(L"control-threads", 3);
    int ksThreads = args.getInt(L"ks-threads", 3);
    int hangSeconds = args.getInt(L"hang-timeout", 60);

    shared.seed = args.has(L"seed") ? (uint64_t)_wtoi64(args.get(L"seed").c_str()) :
        (uint64_t)GetTickCount64() ^ ((uint64_t)GetCurrentProcessId() << 32);
    shared.controlPath = controlDevicePath();
    shared.instanceId = sarDeviceInstanceId();

    if (shared.controlPath.empty() || shared.instanceId.empty()) {
        logf("The SAR device is not present.");
        return 1;
    }

    if (args.has(L"restricted")) {
        shared.token = restrictedToken();

        if (!shared.token) {
            logf("Couldn't make a restricted token: %s", lastErrorText(GetLastError()).c_str());
            return 1;
        }
    }

    logf("Fuzzing for %.0f s, seed %llu, %d control and %d kernel streaming threads%s",
        seconds, (unsigned long long)shared.seed, controlThreads, ksThreads,
        shared.token ? ", as a medium-integrity user without administrator rights" : "");

    std::vector<std::unique_ptr<Worker>> workers;
    auto add = [&](const char *kind, int index, void (*run)(Shared&, Worker&)) {
        auto worker = std::make_unique<Worker>();

        worker->kind = kind;
        worker->index = index;
        worker->lastProgressMs = nowMs();

        Worker *raw = worker.get();

        worker->thread = std::thread([&shared, raw, run] { run(shared, *raw); });
        workers.push_back(std::move(worker));
    };

    add("host", 0, hostWorker);

    for (int i = 0; i < controlThreads; ++i) {
        add("control", i, controlWorker);
    }

    for (int i = 0; i < ksThreads; ++i) {
        add("ks", i, ksWorker);
    }

    double start = nowMs();
    double lastReport = start;
    bool hung = false;
    std::string hungWhere;

    auto watch = [&] {
        for (const auto& worker : workers) {
            const char *stage = worker->stage;

            if (strcmp(stage, "done") != 0 && nowMs() - worker->lastProgressMs > hangSeconds * 1000.0) {
                hung = true;
                hungWhere = std::string(worker->kind) + " thread " + std::to_string(worker->index) +
                    " stuck in \"" + stage + "\"";
                logf("HANG: %s for more than %d s", hungWhere.c_str(), hangSeconds);
                return;
            }
        }
    };

    while (!hung && nowMs() - start < seconds * 1000.0) {
        {
            auto paths = enabledFilterPaths(shared.instanceId);
            std::lock_guard<std::mutex> lock(shared.mutex);

            shared.filterPaths.swap(paths);
        }

        Sleep(250);
        watch();

        if (nowMs() - lastReport > 10000.0) {
            lastReport = nowMs();
            logf("  %lld control requests, %lld kernel streaming requests, %lld endpoints, %lld pins, %lld host restarts",
                (long long)shared.controlRequests, (long long)shared.ksRequests,
                (long long)shared.endpointsCreated, (long long)shared.pinsCreated,
                (long long)shared.hostRestarts);
        }
    }

    shared.stop = true;

    // The workers cancel what they have pending and close their handles; a
    // request the driver never finishes shows up here.
    double stopStart = nowMs();

    while (!hung) {
        bool allDone = true;

        for (const auto& worker : workers) {
            allDone = allDone && strcmp(worker->stage, "done") == 0;
        }

        if (allDone) {
            break;
        }

        Sleep(100);
        watch();

        if (nowMs() - stopStart > (hangSeconds + 30) * 1000.0) {
            hung = true;
            hungWhere = "the threads did not finish after being told to stop";
        }
    }

    std::vector<std::string> statuses;

    {
        std::lock_guard<std::mutex> lock(shared.mutex);

        for (const auto& pair : shared.statuses) {
            JsonObject item;

            item.setString("status", statusText(pair.first)).setInt("count", pair.second);
            statuses.push_back(item.str());
        }
    }

    JsonObject json;

    json.setString("command", "fuzz")
        .setBool("passed", !hung)
        .setInt("seed", (long long)shared.seed)
        .setDouble("seconds", (nowMs() - start) / 1000.0)
        .setBool("restricted", shared.token != nullptr)
        .setInt("controlRequests", shared.controlRequests)
        .setInt("controlOpens", shared.controlOpens)
        .setInt("layoutsSet", shared.layoutsSet)
        .setInt("endpointsCreated", shared.endpointsCreated)
        .setInt("hostRestarts", shared.hostRestarts)
        .setInt("ksRequests", shared.ksRequests)
        .setInt("filterOpens", shared.filterOpens)
        .setInt("filterOpenFailures", shared.filterOpenFailures)
        .setInt("pinCreateAttempts", shared.pinCreates)
        .setInt("pinsCreated", shared.pinsCreated)
        .setInt("requestsCancelledAfterTimeout", shared.timeouts)
        .setRaw("statuses", jsonArray(statuses));

    if (hung) {
        json.setString("hang", hungWhere);
    }

    if (args.has(L"results")) {
        writeTextFile(args.get(L"results"), json.str());
    }

    logf("%s: %lld control requests, %lld kernel streaming requests, %lld endpoints created, %lld of %lld pins created",
        hung ? "HUNG" : "SURVIVED", (long long)shared.controlRequests, (long long)shared.ksRequests,
        (long long)shared.endpointsCreated, (long long)shared.pinsCreated, (long long)shared.pinCreates);

    if (hung) {
        // A stuck thread cannot be joined; leave without it.
        fflush(stdout);
        TerminateProcess(GetCurrentProcess(), 2);
    }

    for (const auto& worker : workers) {
        worker->thread.join();
    }

    if (shared.token) {
        CloseHandle(shared.token);
    }

    return 0;
}

} // namespace SarTest
