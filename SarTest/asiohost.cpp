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

#include "asiohost.h"

#include <algorithm>
#include <cstring>

using namespace Sar;

namespace SarTest {

AsioHost *AsioHost::sInstance = nullptr;

AsioHost::AsioHost(const AsioHostOptions& options)
    : _options(options)
{
    _callbacks.tick = &AsioHost::tickStub;
    _callbacks.sampleRateDidChange = &AsioHost::sampleRateStub;
    _callbacks.asioMessage = &AsioHost::messageStub;
    _callbacks.tickWithTime = nullptr;
    sInstance = this;
    _watchdogThread = std::thread([this] { watchdog(); });
}

AsioHost::~AsioHost()
{
    close();
    _watchdogStop = true;

    if (_watchdogThread.joinable()) {
        _watchdogThread.join();
    }

    if (sInstance == this) {
        sInstance = nullptr;
    }

    // SarAsio.dll is deliberately left loaded: it owns logging state and
    // COM objects, and the process is about to exit anyway.
}

void AsioHost::setPhase(const char *phase)
{
    _phaseStartMs = (long long)nowMs();
    _phase = phase;
}

void AsioHost::watchdog()
{
    long long lastReport = 0;

    while (!_watchdogStop) {
        Sleep(250);

        const char *phase = _phase.load();

        if (strcmp(phase, "idle") == 0) {
            lastReport = 0;
            continue;
        }

        long long elapsed = (long long)nowMs() - _phaseStartMs.load();

        if (elapsed > (long long)_options.phaseTimeoutSeconds * 1000 &&
            elapsed - lastReport > 10000) {
            logf("HANG: IASIO::%s has not returned after %lld ms", phase, elapsed);
            _stats.hangsReported++;
            lastReport = elapsed;
        }
    }
}

bool AsioHost::load()
{
    const GUID clsid = __uuidof(Sar::IASIO);

    if (_options.enableSarAsioLog) {
        SetEnvironmentVariableW(L"SAR_ASIO_LOG", L"1");
    }

    setPhase("load");

    if (!_options.sarAsioPath.empty()) {
        typedef HRESULT (STDAPICALLTYPE *GetClassObjectFn)(REFCLSID, REFIID, LPVOID *);

        _module = LoadLibraryW(_options.sarAsioPath.c_str());

        if (!_module) {
            logf("LoadLibrary(%s) failed: %s", narrow(_options.sarAsioPath).c_str(),
                lastErrorText(GetLastError()).c_str());
            setPhase("idle");
            return false;
        }

        auto getClassObject =
            (GetClassObjectFn)GetProcAddress(_module, "DllGetClassObject");

        if (!getClassObject) {
            logf("%s does not export DllGetClassObject", narrow(_options.sarAsioPath).c_str());
            setPhase("idle");
            return false;
        }

        IClassFactory *factory = nullptr;
        HRESULT hr = getClassObject(clsid, IID_IClassFactory, (void **)&factory);

        if (FAILED(hr) || !factory) {
            logf("DllGetClassObject failed: %s", hresultText(hr).c_str());
            setPhase("idle");
            return false;
        }

        // ASIO drivers use their CLSID as the interface ID.
        hr = factory->CreateInstance(nullptr, clsid, (void **)&_asio);
        factory->Release();

        if (FAILED(hr) || !_asio) {
            logf("IClassFactory::CreateInstance failed: %s", hresultText(hr).c_str());
            _asio = nullptr;
            setPhase("idle");
            return false;
        }

        logf("Loaded %s", narrow(_options.sarAsioPath).c_str());
    } else {
        HRESULT hr = CoCreateInstance(
            clsid, nullptr, CLSCTX_INPROC_SERVER, clsid, (void **)&_asio);

        if (FAILED(hr) || !_asio) {
            logf("CoCreateInstance(SarAsio) failed: %s (is SarAsio registered?)",
                hresultText(hr).c_str());
            _asio = nullptr;
            setPhase("idle");
            return false;
        }

        logf("Created the registered SarAsio driver");
    }

    setPhase("idle");
    return true;
}

bool AsioHost::open()
{
    if (!_asio) {
        return false;
    }

    setPhase("init");

    if (_asio->init(nullptr) != AsioBool::True) {
        char message[124] = {};

        _asio->getErrorMessage(message);
        logf("IASIO::init failed: %s", message);
        setPhase("idle");
        return false;
    }

    char name[32] = {};

    _asio->getDriverName(name);
    logf("Driver: %s (version %ld)", name, _asio->getDriverVersion());

    setPhase("getChannels");

    if (_asio->getChannels(&_totalInputs, &_totalOutputs) != AsioStatus::OK) {
        logf("IASIO::getChannels failed");
        setPhase("idle");
        return false;
    }

    _stats.virtualInputs = _options.layout.totalChannels();
    _stats.virtualOutputs = _options.layout.totalChannels();
    _stats.physicalInputs = _totalInputs - _stats.virtualInputs;
    _stats.physicalOutputs = _totalOutputs - _stats.virtualOutputs;

    if (_stats.physicalInputs < 0 || _stats.physicalOutputs < 0) {
        logf("Driver reports %ld inputs and %ld outputs, fewer than the %d "
            "channels per direction the layout needs. Run 'SarTest install' "
            "or 'SarTest config' with the same layout first.",
            _totalInputs, _totalOutputs, _options.layout.totalChannels());
        setPhase("idle");
        return false;
    }

    logf("Channels: %ld inputs (%ld physical), %ld outputs (%ld physical)",
        _totalInputs, _stats.physicalInputs, _totalOutputs, _stats.physicalOutputs);

    setPhase("getBufferSize");

    long minSize = 0, maxSize = 0, preferredSize = 0, granularity = 0;

    if (_asio->getBufferSize(&minSize, &maxSize, &preferredSize, &granularity) != AsioStatus::OK) {
        logf("IASIO::getBufferSize failed");
        setPhase("idle");
        return false;
    }

    setPhase("setSampleRate");

    if (_asio->setSampleRate(_options.sampleRate) != AsioStatus::OK) {
        logf("setSampleRate(%.0f) rejected, continuing with the driver's rate",
            _options.sampleRate);
    }

    double sampleRate = 0.0;

    _asio->getSampleRate(&sampleRate);
    _stats.sampleRate = sampleRate;
    _stats.bufferFrames = preferredSize;
    logf("Sample rate %.0f Hz, buffer %ld frames (min %ld, max %ld, granularity %ld)",
        sampleRate, preferredSize, minSize, maxSize, granularity);

    setPhase("getChannelInfo");
    _infos.clear();

    for (int direction = 0; direction < 2; ++direction) {
        bool isInput = direction == 0;
        long count = isInput ? _totalInputs : _totalOutputs;

        for (long i = 0; i < count; ++i) {
            AsioChannelInfo info = {};

            info.index = i;
            info.isInput = isInput ? AsioBool::True : AsioBool::False;

            if (_asio->getChannelInfo(&info) != AsioStatus::OK) {
                logf("getChannelInfo(%s %ld) failed", isInput ? "input" : "output", i);
                setPhase("idle");
                return false;
            }

            if (info.sampleType != Int32LSB) {
                logf("Unsupported sample type %ld on %s %ld (%s)",
                    info.sampleType, isInput ? "input" : "output", i, info.name);
                setPhase("idle");
                return false;
            }

            AsioBufferInfo buffer = {};

            buffer.isInput = info.isInput;
            buffer.index = i;
            _infos.push_back(buffer);
        }
    }

    _sampleSize = 4;
    setPhase("createBuffers");

    double t0 = nowMs();
    AsioStatus status = _asio->createBuffers(
        _infos.data(), (long)_infos.size(), preferredSize, &_callbacks);

    _stats.createBuffersMs = nowMs() - t0;
    setPhase("idle");

    if (status != AsioStatus::OK) {
        logf("IASIO::createBuffers failed: %ld", (long)status);
        return false;
    }

    _buffersCreated = true;
    logf("createBuffers OK for %zu channels in %.1f ms", _infos.size(), _stats.createBuffersMs);
    return true;
}

bool AsioHost::start()
{
    if (!_buffersCreated) {
        return false;
    }

    setPhase("start");

    double t0 = nowMs();
    AsioStatus status = _asio->start();
    double elapsed = nowMs() - t0;

    setPhase("idle");
    _stats.lastStartMs = elapsed;
    _stats.maxStartMs = std::max(_stats.maxStartMs, elapsed);

    if (status != AsioStatus::OK) {
        logf("IASIO::start failed: %ld after %.1f ms", (long)status, elapsed);
        return false;
    }

    _running = true;
    _stats.starts++;
    logf("start OK in %.1f ms", elapsed);
    return true;
}

bool AsioHost::stop()
{
    if (!_running) {
        return true;
    }

    readClockStats();
    setPhase("stop");

    double t0 = nowMs();
    AsioStatus status = _asio->stop();
    double elapsed = nowMs() - t0;

    setPhase("idle");
    _running = false;
    _stats.lastStopMs = elapsed;
    _stats.maxStopMs = std::max(_stats.maxStopMs, elapsed);
    _stats.ticks = _ticks.load();

    if (status != AsioStatus::OK) {
        logf("IASIO::stop failed: %ld after %.1f ms", (long)status, elapsed);
        return false;
    }

    logf("stop OK in %.1f ms (%lld ticks so far, clock late %llu of %llu)",
        elapsed, _stats.ticks,
        (unsigned long long)_stats.clock.lateTicks,
        (unsigned long long)_stats.clock.ticks);
    return true;
}

void AsioHost::close()
{
    if (!_asio) {
        return;
    }

    stop();

    if (_buffersCreated) {
        setPhase("disposeBuffers");

        double t0 = nowMs();

        _asio->disposeBuffers();
        _stats.disposeBuffersMs = nowMs() - t0;
        setPhase("idle");
        _buffersCreated = false;
    }

    _asio->Release();
    _asio = nullptr;
}

void AsioHost::readClockStats()
{
    SarTestClock::ClockStats stats = {};

    stats.size = sizeof(stats);

    if (_asio && _asio->future(SarTestClock::kStatsSelector, &stats) == AsioStatus::OK) {
        _stats.clock = stats;
    }
}

void AsioHost::onTick(long bufferIndex)
{
    long index = bufferIndex & 1;
    long count = std::min(_stats.virtualInputs, _stats.virtualOutputs);
    size_t bytes = (size_t)_stats.bufferFrames * (size_t)_sampleSize;

    for (long k = 0; k < count; ++k) {
        const AsioBufferInfo& in = _infos[(size_t)(_stats.physicalInputs + k)];
        const AsioBufferInfo& out = _infos[(size_t)(_totalInputs + _stats.physicalOutputs + k)];

        if (in.asioBuffers[index] && out.asioBuffers[index]) {
            memcpy(out.asioBuffers[index], in.asioBuffers[index], bytes);
        }
    }

    _ticks++;
}

void AsioHost::tickStub(long bufferIndex, AsioBool)
{
    AsioHost *host = sInstance;

    if (host) {
        host->onTick(bufferIndex);
    }
}

void AsioHost::sampleRateStub(double sampleRate)
{
    logf("Driver reports sample rate change to %.0f Hz", sampleRate);
}

long AsioHost::messageStub(AsioMessage selector, long value, void *, double *)
{
    switch (selector) {
    case AsioMessage::SelectorSupported:
        switch ((AsioMessage)value) {
        case AsioMessage::EngineVersion:
        case AsioMessage::ResetRequest:
        case AsioMessage::BufferSizeChange:
        case AsioMessage::ResyncRequest:
        case AsioMessage::LatenciesChanged:
            return 1;
        default:
            return 0;
        }
    case AsioMessage::EngineVersion:
        return 2;
    case AsioMessage::ResetRequest:
        logf("Driver requested a reset");

        if (sInstance) {
            sInstance->_stats.resetRequests++;
        }

        return 1;
    case AsioMessage::BufferSizeChange:
    case AsioMessage::ResyncRequest:
    case AsioMessage::LatenciesChanged:
        return 1;
    default:
        // Includes SupportsTimeInfo: we only implement the plain callback.
        return 0;
    }
}

std::string AsioHost::toJson() const
{
    JsonObject clock;

    clock.setInt("ticks", (long long)_stats.clock.ticks)
        .setInt("lateTicks", (long long)_stats.clock.lateTicks)
        .setDouble("maxLatenessMs", _stats.clock.maxLatenessMs)
        .setDouble("periodMs", _stats.clock.periodMs);

    JsonObject host;

    host.setInt("ticks", _ticks.load())
        .setInt("starts", _stats.starts)
        .setInt("resetRequests", _stats.resetRequests)
        .setInt("hangsReported", _stats.hangsReported)
        .setDouble("lastStartMs", _stats.lastStartMs)
        .setDouble("maxStartMs", _stats.maxStartMs)
        .setDouble("lastStopMs", _stats.lastStopMs)
        .setDouble("maxStopMs", _stats.maxStopMs)
        .setDouble("createBuffersMs", _stats.createBuffersMs)
        .setDouble("disposeBuffersMs", _stats.disposeBuffersMs)
        .setInt("bufferFrames", _stats.bufferFrames)
        .setDouble("sampleRate", _stats.sampleRate)
        .setInt("physicalInputs", _stats.physicalInputs)
        .setInt("physicalOutputs", _stats.physicalOutputs)
        .setInt("virtualInputs", _stats.virtualInputs)
        .setInt("virtualOutputs", _stats.virtualOutputs)
        .setRaw("clock", clock.str());
    return host.str();
}

} // namespace SarTest
