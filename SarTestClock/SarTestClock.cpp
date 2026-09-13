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

// A software-clock ASIO driver for testing SAR on machines with no audio
// hardware. It exposes a few dummy channels and delivers bufferSwitch
// callbacks from a timer thread at the configured period, so SarAsio can
// wrap it exactly like a real interface. Tunables (read at object creation):
//
//   SAR_TEST_CLOCK_RATE     sample rate in Hz        (default 48000)
//   SAR_TEST_CLOCK_FRAMES   frames per callback      (default 480, 10 ms)
//   SAR_TEST_CLOCK_INPUTS   dummy input channels     (default 2)
//   SAR_TEST_CLOCK_OUTPUTS  dummy output channels    (default 2)

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <unknwn.h>
#include <olectl.h>
#include <mmsystem.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string>
#include <thread>
#include <vector>

#include "tinyasio.h"
#include "clockstats.h"

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

using namespace Sar;

namespace {

// {7A3C9E12-4B5D-4F6E-8A1B-2C3D4E5F6A7B}
const GUID kClsid =
    { 0x7a3c9e12, 0x4b5d, 0x4f6e, { 0x8a, 0x1b, 0x2c, 0x3d, 0x4e, 0x5f, 0x6a, 0x7b } };
const wchar_t kAsioKey[] = L"SOFTWARE\\ASIO\\SAR Test Clock";
const wchar_t kClsidKey[] = L"CLSID\\" SAR_TEST_CLOCK_CLSID_STR;
const wchar_t kInprocKey[] = L"CLSID\\" SAR_TEST_CLOCK_CLSID_STR L"\\InProcServer32";

HMODULE gModule = nullptr;
std::atomic<long> gObjectCount{ 0 };
std::atomic<long> gLockCount{ 0 };

long envLong(const wchar_t *name, long def)
{
    wchar_t buffer[64];
    DWORD len = GetEnvironmentVariableW(name, buffer, 64);

    if (len == 0 || len >= 64) {
        return def;
    }

    long value = _wtol(buffer);
    return value > 0 ? value : def;
}

int64_t fileTimeNow()
{
    FILETIME ft;

    GetSystemTimePreciseAsFileTime(&ft);
    return ((int64_t)ft.dwHighDateTime << 32) | (int64_t)ft.dwLowDateTime;
}

class SoftwareClock: public IASIO
{
public:
    SoftwareClock()
    {
        gObjectCount++;
        _sampleRate = (double)envLong(L"SAR_TEST_CLOCK_RATE", 48000);
        _bufferFrames = envLong(L"SAR_TEST_CLOCK_FRAMES", 480);
        _inputCount = envLong(L"SAR_TEST_CLOCK_INPUTS", 2);
        _outputCount = envLong(L"SAR_TEST_CLOCK_OUTPUTS", 2);
    }

    virtual ~SoftwareClock()
    {
        disposeBuffers();
        gObjectCount--;
    }

    // IUnknown. ASIO drivers answer their own CLSID as the interface ID.
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppv) override
    {
        if (!ppv) {
            return E_POINTER;
        }

        if (riid == IID_IUnknown || riid == kClsid) {
            *ppv = static_cast<IASIO *>(this);
            AddRef();
            return S_OK;
        }

        *ppv = nullptr;
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override
    {
        return (ULONG)++_refs;
    }

    ULONG STDMETHODCALLTYPE Release() override
    {
        long refs = --_refs;

        if (refs == 0) {
            delete this;
        }

        return (ULONG)refs;
    }

    // IASIO
    AsioBool init(void *) override
    {
        return AsioBool::True;
    }

    void getDriverName(char name[32]) override
    {
        strcpy_s(name, 32, "SAR Test Clock");
    }

    long getDriverVersion() override
    {
        return 1;
    }

    void getErrorMessage(char str[124]) override
    {
        strcpy_s(str, 124, _error.c_str());
    }

    AsioStatus start() override
    {
        if (_running) {
            return AsioStatus::OK;
        }

        if (!_callbacks.tick) {
            _error = "createBuffers has not been called";
            return AsioStatus::NotPresent;
        }

        _stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);

        if (!_stopEvent) {
            return AsioStatus::HardwareMalfunction;
        }

        _ticks = 0;
        _lateTicks = 0;
        _maxLatenessMs = 0.0;
        _running = true;
        _thread = std::thread([this] { tickThread(); });
        return AsioStatus::OK;
    }

    AsioStatus stop() override
    {
        if (!_running) {
            return AsioStatus::OK;
        }

        _running = false;
        SetEvent(_stopEvent);

        if (_thread.joinable()) {
            _thread.join();
        }

        CloseHandle(_stopEvent);
        _stopEvent = nullptr;
        return AsioStatus::OK;
    }

    AsioStatus getChannels(long *inputCount, long *outputCount) override
    {
        *inputCount = _inputCount;
        *outputCount = _outputCount;
        return AsioStatus::OK;
    }

    AsioStatus getLatencies(long *inputLatency, long *outputLatency) override
    {
        *inputLatency = _bufferFrames;
        *outputLatency = _bufferFrames;
        return AsioStatus::OK;
    }

    AsioStatus getBufferSize(
        long *minSize, long *maxSize, long *preferredSize, long *granularity) override
    {
        *minSize = *maxSize = *preferredSize = _bufferFrames;
        *granularity = 0;
        return AsioStatus::OK;
    }

    AsioStatus canSampleRate(double sampleRate) override
    {
        return sampleRate >= 8000.0 && sampleRate <= 192000.0 ?
            AsioStatus::OK : AsioStatus::NoClock;
    }

    AsioStatus getSampleRate(double *sampleRate) override
    {
        *sampleRate = _sampleRate;
        return AsioStatus::OK;
    }

    AsioStatus setSampleRate(double sampleRate) override
    {
        if (sampleRate == _sampleRate) {
            return AsioStatus::OK;
        }

        if (_running || canSampleRate(sampleRate) != AsioStatus::OK) {
            return AsioStatus::NoClock;
        }

        _sampleRate = sampleRate;

        if (_callbacks.sampleRateDidChange) {
            _callbacks.sampleRateDidChange(sampleRate);
        }

        return AsioStatus::OK;
    }

    AsioStatus getClockSources(AsioClockSource *clocks, long *count) override
    {
        if (!clocks || !count || *count < 1) {
            return AsioStatus::InvalidParameter;
        }

        clocks[0].index = 0;
        clocks[0].channel = 0;
        clocks[0].group = 0;
        clocks[0].isCurrentSource = AsioBool::True;
        strcpy_s(clocks[0].name, 32, "Internal");
        *count = 1;
        return AsioStatus::OK;
    }

    AsioStatus setClockSource(long index) override
    {
        return index == 0 ? AsioStatus::OK : AsioStatus::InvalidParameter;
    }

    AsioStatus getSamplePosition(int64_t *pos, int64_t *timestamp) override
    {
        *pos = _samplePosition.load();
        // ASIO timestamps are nanoseconds; FILETIME is 100 ns units.
        *timestamp = fileTimeNow() * 100;
        return AsioStatus::OK;
    }

    AsioStatus getChannelInfo(AsioChannelInfo *info) override
    {
        bool isInput = info->isInput == AsioBool::True;
        long limit = isInput ? _inputCount : _outputCount;

        if (info->index < 0 || info->index >= limit) {
            return AsioStatus::InvalidParameter;
        }

        info->isActive = _active ? AsioBool::True : AsioBool::False;
        info->group = 0;
        info->sampleType = Int32LSB;
        sprintf_s(info->name, 32, "Clock %s %ld", isInput ? "In" : "Out", info->index + 1);
        return AsioStatus::OK;
    }

    AsioStatus createBuffers(
        AsioBufferInfo *infos, long channelCount, long bufferSize,
        AsioCallbacks *callbacks) override
    {
        if (_active) {
            disposeBuffers();
        }

        if (!callbacks || !callbacks->tick) {
            return AsioStatus::InvalidParameter;
        }

        if (bufferSize < 16 || bufferSize > 65536) {
            return AsioStatus::InvalidMode;
        }

        for (long i = 0; i < channelCount; ++i) {
            long limit = infos[i].isInput == AsioBool::True ? _inputCount : _outputCount;

            if (infos[i].index < 0 || infos[i].index >= limit) {
                return AsioStatus::InvalidParameter;
            }
        }

        _bufferFrames = bufferSize;
        _callbacks = *callbacks;

        for (long i = 0; i < channelCount; ++i) {
            for (int half = 0; half < 2; ++half) {
                void *memory = calloc((size_t)bufferSize, sizeof(int32_t));

                if (!memory) {
                    disposeBuffers();
                    return AsioStatus::NoMemory;
                }

                infos[i].asioBuffers[half] = memory;
                _buffers.push_back(memory);
            }
        }

        _active = true;
        return AsioStatus::OK;
    }

    AsioStatus disposeBuffers() override
    {
        stop();

        for (void *buffer : _buffers) {
            free(buffer);
        }

        _buffers.clear();
        _callbacks = {};
        _active = false;
        return AsioStatus::OK;
    }

    AsioStatus controlPanel() override
    {
        return AsioStatus::OK;
    }

    AsioStatus future(long selector, void *opt) override
    {
        if (selector == SarTestClock::kStatsSelector && opt) {
            auto *stats = (SarTestClock::ClockStats *)opt;

            if (stats->size < sizeof(SarTestClock::ClockStats)) {
                return AsioStatus::InvalidParameter;
            }

            stats->running = _running ? 1 : 0;
            stats->ticks = _ticks.load();
            stats->lateTicks = _lateTicks.load();
            stats->maxLatenessMs = _maxLatenessMs.load();
            stats->periodMs = _bufferFrames * 1000.0 / _sampleRate;
            stats->sampleRate = _sampleRate;
            stats->bufferFrames = (uint32_t)_bufferFrames;
            stats->reserved = 0;
            return AsioStatus::OK;
        }

        return AsioStatus::NotPresent;
    }

    AsioStatus outputReady() override
    {
        return AsioStatus::NotPresent;
    }

private:
    void tickThread()
    {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
        timeBeginPeriod(1);

        HANDLE timer = CreateWaitableTimerExW(
            nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);

        if (!timer) {
            timer = CreateWaitableTimerW(nullptr, TRUE, nullptr);
        }

        const double periodMs = _bufferFrames * 1000.0 / _sampleRate;
        const int64_t period100ns = (int64_t)(_bufferFrames * 10000000.0 / _sampleRate + 0.5);
        const DWORD waitTimeoutMs = (DWORD)(periodMs * 4.0 + 50.0);
        int64_t origin = fileTimeNow();
        long bufferIndex = 0;
        HANDLE handles[2] = { _stopEvent, timer };

        for (uint64_t n = 1; _running; ++n) {
            LARGE_INTEGER due;

            due.QuadPart = origin + (int64_t)n * period100ns;
            SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE);

            DWORD wait = WaitForMultipleObjects(2, handles, FALSE, waitTimeoutMs);

            if (wait == WAIT_OBJECT_0 || !_running) {
                break;
            }

            double latenessMs = (double)(fileTimeNow() - due.QuadPart) / 10000.0;

            if (latenessMs > periodMs) {
                _lateTicks++;
            }

            if (latenessMs > _maxLatenessMs.load()) {
                _maxLatenessMs = latenessMs;
            }

            // Far behind (suspended VM, debugger): resynchronize the schedule
            // rather than delivering a burst of catch-up callbacks.
            if (latenessMs > periodMs * 8.0) {
                origin = fileTimeNow() - (int64_t)n * period100ns;
            }

            _samplePosition += _bufferFrames;
            _callbacks.tick(bufferIndex, AsioBool::True);
            bufferIndex ^= 1;
            _ticks++;
        }

        CloseHandle(timer);
        timeEndPeriod(1);
    }

    std::atomic<long> _refs{ 1 };
    std::string _error;
    double _sampleRate = 48000.0;
    long _bufferFrames = 480;
    long _inputCount = 2;
    long _outputCount = 2;
    bool _active = false;
    std::atomic<bool> _running{ false };
    AsioCallbacks _callbacks = {};
    std::vector<void *> _buffers;
    std::thread _thread;
    HANDLE _stopEvent = nullptr;
    std::atomic<int64_t> _samplePosition{ 0 };
    std::atomic<uint64_t> _ticks{ 0 };
    std::atomic<uint64_t> _lateTicks{ 0 };
    std::atomic<double> _maxLatenessMs{ 0.0 };
};

class ClassFactory: public IClassFactory
{
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppv) override
    {
        if (!ppv) {
            return E_POINTER;
        }

        if (riid == IID_IUnknown || riid == IID_IClassFactory) {
            *ppv = static_cast<IClassFactory *>(this);
            return S_OK;
        }

        *ppv = nullptr;
        return E_NOINTERFACE;
    }

    // Static lifetime; reference counting is a no-op.
    ULONG STDMETHODCALLTYPE AddRef() override { return 2; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }

    HRESULT STDMETHODCALLTYPE CreateInstance(
        IUnknown *outer, REFIID riid, void **ppv) override
    {
        if (!ppv) {
            return E_POINTER;
        }

        *ppv = nullptr;

        if (outer) {
            return CLASS_E_NOAGGREGATION;
        }

        SoftwareClock *clock = new (std::nothrow) SoftwareClock();

        if (!clock) {
            return E_OUTOFMEMORY;
        }

        HRESULT hr = clock->QueryInterface(riid, ppv);

        clock->Release();
        return hr;
    }

    HRESULT STDMETHODCALLTYPE LockServer(BOOL lock) override
    {
        if (lock) {
            gLockCount++;
        } else {
            gLockCount--;
        }

        return S_OK;
    }
};

ClassFactory gFactory;

LSTATUS setRegistryString(
    HKEY root, const wchar_t *key, const wchar_t *name, const wchar_t *value)
{
    HKEY handle = nullptr;
    LSTATUS status = RegCreateKeyExW(
        root, key, 0, nullptr, 0, KEY_WRITE, nullptr, &handle, nullptr);

    if (status != ERROR_SUCCESS) {
        return status;
    }

    status = RegSetValueExW(
        handle, name, 0, REG_SZ, (const BYTE *)value,
        (DWORD)((wcslen(value) + 1) * sizeof(wchar_t)));
    RegCloseKey(handle);
    return status;
}

} // namespace

extern "C" BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        gModule = instance;
        DisableThreadLibraryCalls(instance);
    }

    return TRUE;
}

STDAPI DllGetClassObject(REFCLSID rclsid, REFIID riid, LPVOID *ppv)
{
    if (!ppv) {
        return E_POINTER;
    }

    if (rclsid != kClsid) {
        *ppv = nullptr;
        return CLASS_E_CLASSNOTAVAILABLE;
    }

    return gFactory.QueryInterface(riid, ppv);
}

STDAPI DllCanUnloadNow()
{
    return gObjectCount == 0 && gLockCount == 0 ? S_OK : S_FALSE;
}

STDAPI DllRegisterServer()
{
    wchar_t path[MAX_PATH] = {};

    if (!GetModuleFileNameW(gModule, path, MAX_PATH)) {
        return E_FAIL;
    }

    if (setRegistryString(HKEY_CLASSES_ROOT, kClsidKey, nullptr, SAR_TEST_CLOCK_NAME) != ERROR_SUCCESS ||
        setRegistryString(HKEY_CLASSES_ROOT, kInprocKey, nullptr, path) != ERROR_SUCCESS ||
        setRegistryString(HKEY_CLASSES_ROOT, kInprocKey, L"ThreadingModel", L"Both") != ERROR_SUCCESS) {
        return SELFREG_E_CLASS;
    }

    if (setRegistryString(HKEY_LOCAL_MACHINE, kAsioKey, L"CLSID", SAR_TEST_CLOCK_CLSID_STR) != ERROR_SUCCESS ||
        setRegistryString(HKEY_LOCAL_MACHINE, kAsioKey, L"Description", SAR_TEST_CLOCK_NAME) != ERROR_SUCCESS) {
        return SELFREG_E_CLASS;
    }

    return S_OK;
}

STDAPI DllUnregisterServer()
{
    RegDeleteTreeW(HKEY_LOCAL_MACHINE, kAsioKey);
    RegDeleteTreeW(HKEY_CLASSES_ROOT, kClsidKey);
    return S_OK;
}
