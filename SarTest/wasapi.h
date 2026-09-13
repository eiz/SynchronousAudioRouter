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

#ifndef _SAR_TEST_WASAPI_H
#define _SAR_TEST_WASAPI_H

#include "common.h"

#include <mmdeviceapi.h>

#include <atomic>
#include <memory>

namespace SarTest {

struct StreamStats
{
    std::string name;
    bool isCapture = false;
    bool started = false;
    bool deviceInvalidated = false;
    HRESULT lastError = S_OK;
    std::string errorStage;
    std::string format;
    int channels = 0;
    long long framesProcessed = 0;
    long long silentFrames = 0;
    long long startupSilentFrames = 0;
    long long validFrames = 0;
    long long discontinuities = 0;
    long long engineDiscontinuities = 0;
    // Undecodable frames once the signal has locked in: corruption.
    long long wrongChannelFrames = 0;
    // Undecodable frames before the signal locks in (at the start, and again
    // after a reopen): the engine ramping a new stream's volume in.
    long long transitionFrames = 0;
    long long rampRuns = 0;
    long long corruptRuns = 0;
    // Silent frames once the signal has locked in: dropouts.
    long long midStreamSilentFrames = 0;
    long long timeouts = 0;
    // AUDCLNT_E_DEVICE_INVALIDATED events, and how many times the stream
    // was reopened after one, as a well-behaved client would.
    int invalidations = 0;
    int reopens = 0;
    // Setup calls that failed and were retried (e.g. a mix format that went
    // stale because the endpoint was reconfigured in between).
    int setupErrors = 0;
    // The slowest WASAPI call the stream made, to name stalls in the audio
    // stack; calls over a second are also recorded in gapSamples.
    double slowestCallMs = 0.0;
    std::string slowestCall;
    // The first undecodable frames, with their raw sample values.
    std::vector<std::string> badFrameSamples;
    // The first dropouts and sequence jumps, timed from the start of the run.
    std::vector<std::string> gapSamples;
    bool passed = false;
    std::string failure;

    std::string toJson() const;
};

struct FoundEndpoints
{
    std::vector<IMMDevice *> render;    // [pair]
    std::vector<IMMDevice *> capture;   // [pair]

    ~FoundEndpoints();
    void release();
};

// Polls the MMDevice enumerator until every endpoint of the layout is active
// or the timeout passes.
bool findEndpoints(const EndpointLayout& layout, int timeoutSeconds, FoundEndpoints *out);

// Polls until none of the layout's endpoints are active any more.
bool waitForEndpointsGone(const EndpointLayout& layout, int timeoutSeconds);

struct WasapiOptions
{
    EndpointLayout layout;
    double durationSeconds = 5.0;
    // The ASIO host is going to be killed underneath the streams, so device
    // invalidation is the expected outcome rather than a failure.
    bool expectInvalidation = false;
    double minValidRatio = 0.5;
    long long maxDiscontinuities = -1;   // -1: report only
    // SarAsio broadcasts a format change whenever one of its endpoints
    // becomes active, which invalidates open streams: wait before streaming,
    // and reopen invalidated streams like a well-behaved client would.
    double settleSeconds = 2.0;
    int maxReopens = 20;
    long long maxTransitionFrames = 4800;
};

struct WasapiResult
{
    std::vector<StreamStats> streams;
    bool passed = false;

    std::string toJson() const;
};

// Streams the test signal into every playback endpoint and verifies it on
// every recording endpoint for the configured duration.
WasapiResult runWasapi(const WasapiOptions& options, const FoundEndpoints& endpoints);

struct RaceStats
{
    long long attempts = 0;
    long long opened = 0;
    long long noEndpoint = 0;
    long long hangsReported = 0;
    double maxCallMs = 0.0;
    std::string maxCallName;
    std::map<std::string, long long> errors;   // "<call> <HRESULT>" -> count

    std::string toJson() const;
};

// Threads that keep finding the layout's endpoints and opening, starting,
// holding and closing shared-mode streams on them while the ASIO host starts
// and stops underneath, the way applications that use a SAR endpoint as their
// default device behave while a DAW starts. A watchdog reports any WASAPI
// call that does not return within the timeout as a hang.
class RaceOpeners
{
public:
    RaceOpeners(const EndpointLayout& layout, int threads, int holdMs, int hangTimeoutSeconds);
    ~RaceOpeners();
    RaceOpeners(const RaceOpeners&) = delete;
    RaceOpeners& operator=(const RaceOpeners&) = delete;

    void start();
    // Stops the threads; false when one did not finish within the timeout.
    bool stop();
    RaceStats stats();

private:
    struct Worker;
    static DWORD WINAPI threadMain(LPVOID param);
    static DWORD WINAPI watchdogMain(LPVOID param);
    void work(Worker *worker);
    void watch();

    EndpointLayout _layout;
    int _holdMs;
    int _hangTimeoutMs;
    std::vector<std::unique_ptr<Worker>> _workers;
    std::vector<HANDLE> _threads;
    HANDLE _watchdog = nullptr;
    std::atomic<bool> _stop{ false };
    std::atomic<bool> _watchdogStop{ false };
    bool _stopped = false;
    bool _clean = true;
};

} // namespace SarTest
#endif // _SAR_TEST_WASAPI_H
