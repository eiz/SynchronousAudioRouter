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

#ifndef _SAR_TEST_ASIOHOST_H
#define _SAR_TEST_ASIOHOST_H

#include "common.h"

#include <unknwn.h>
#include <atomic>
#include <thread>

#include "tinyasio.h"
#include "clockstats.h"

namespace SarTest {

struct AsioHostOptions
{
    // Path of SarAsio.dll to load directly. Empty: CoCreateInstance the
    // registered driver, the way a DAW does.
    std::wstring sarAsioPath;
    EndpointLayout layout;
    double sampleRate = 48000.0;
    // Watchdog: report a hang when a single driver call exceeds this.
    int phaseTimeoutSeconds = 30;
    bool enableSarAsioLog = true;
};

struct AsioHostStats
{
    long long ticks = 0;
    long long starts = 0;
    long long resetRequests = 0;
    long long hangsReported = 0;
    double lastStartMs = 0.0;
    double maxStartMs = 0.0;
    double lastStopMs = 0.0;
    double maxStopMs = 0.0;
    double createBuffersMs = 0.0;
    double disposeBuffersMs = 0.0;
    long bufferFrames = 0;
    double sampleRate = 0.0;
    long physicalInputs = 0;
    long physicalOutputs = 0;
    long virtualInputs = 0;
    long virtualOutputs = 0;
    SarTestClock::ClockStats clock = {};
};

// A headless ASIO host around SarAsio. Every ASIO input (a SAR playback
// endpoint channel) is copied to the ASIO output with the same index (a SAR
// recording endpoint channel) on each callback, so the WASAPI side can verify
// audio end to end.
class AsioHost
{
public:
    explicit AsioHost(const AsioHostOptions& options);
    ~AsioHost();

    bool load();
    bool open();
    bool start();
    bool stop();
    void close();

    bool running() const { return _running; }
    const AsioHostStats& stats() const { return _stats; }
    std::string toJson() const;

private:
    static AsioHost *sInstance;
    static void tickStub(long bufferIndex, Sar::AsioBool directProcess);
    static void sampleRateStub(double sampleRate);
    static long messageStub(
        Sar::AsioMessage selector, long value, void *message, double *opt);

    void onTick(long bufferIndex);
    void setPhase(const char *phase);
    void watchdog();
    void readClockStats();

    AsioHostOptions _options;
    AsioHostStats _stats;
    HMODULE _module = nullptr;
    Sar::IASIO *_asio = nullptr;
    std::vector<Sar::AsioBufferInfo> _infos;
    long _totalInputs = 0;
    long _totalOutputs = 0;
    int _sampleSize = 4;
    Sar::AsioCallbacks _callbacks = {};
    bool _buffersCreated = false;
    bool _running = false;
    std::atomic<long long> _ticks{ 0 };

    std::atomic<const char *> _phase{ "idle" };
    std::atomic<long long> _phaseStartMs{ 0 };
    std::atomic<bool> _watchdogStop{ false };
    std::thread _watchdogThread;
};

} // namespace SarTest
#endif // _SAR_TEST_ASIOHOST_H
