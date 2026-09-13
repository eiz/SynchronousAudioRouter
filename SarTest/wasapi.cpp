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

#include "wasapi.h"

#include <initguid.h>
#include <audioclient.h>
#include <audiopolicy.h>
#include <endpointvolume.h>
#include <functiondiscoverykeys_devpkey.h>
#include <mmreg.h>

#include <algorithm>
#include <memory>
#include <mutex>
#include <thread>

namespace SarTest {

namespace {

template <class T>
class ComPtr
{
public:
    ComPtr() {}
    ~ComPtr() { reset(); }
    ComPtr(const ComPtr&) = delete;
    ComPtr& operator=(const ComPtr&) = delete;

    T *get() const { return _p; }
    T *operator->() const { return _p; }
    explicit operator bool() const { return _p != nullptr; }
    T **put() { reset(); return &_p; }
    void **putVoid() { reset(); return (void **)&_p; }
    T *detach() { T *p = _p; _p = nullptr; return p; }

    void reset()
    {
        if (_p) {
            _p->Release();
            _p = nullptr;
        }
    }

private:
    T *_p = nullptr;
};

enum class SampleKind { Float32, Int32, Unsupported };

// Common time origin for every stream of a run, so gaps line up.
double g_runOriginMs = 0.0;

SampleKind classifyFormat(const WAVEFORMATEX *format, std::string *description)
{
    WORD tag = format->wFormatTag;
    bool isFloat = tag == WAVE_FORMAT_IEEE_FLOAT;
    bool isPcm = tag == WAVE_FORMAT_PCM;

    if (tag == WAVE_FORMAT_EXTENSIBLE && format->cbSize >= 22) {
        auto *ext = (const WAVEFORMATEXTENSIBLE *)format;

        // KSDATAFORMAT_SUBTYPE_* GUIDs carry the classic tag in Data1.
        isFloat = ext->SubFormat.Data1 == WAVE_FORMAT_IEEE_FLOAT;
        isPcm = ext->SubFormat.Data1 == WAVE_FORMAT_PCM;
    }

    char buf[128];

    sprintf_s(buf, "%s %u-bit, %u ch, %lu Hz",
        isFloat ? "float" : isPcm ? "pcm" : "unknown",
        (unsigned)format->wBitsPerSample, (unsigned)format->nChannels,
        (unsigned long)format->nSamplesPerSec);
    *description = buf;

    if (isFloat && format->wBitsPerSample == 32) {
        return SampleKind::Float32;
    }

    if (isPcm && format->wBitsPerSample == 32) {
        return SampleKind::Int32;
    }

    return SampleKind::Unsupported;
}

std::wstring propertyString(IPropertyStore *store, const PROPERTYKEY& key)
{
    PROPVARIANT value;
    std::wstring result;

    PropVariantInit(&value);

    if (SUCCEEDED(store->GetValue(key, &value)) &&
        value.vt == VT_LPWSTR && value.pwszVal) {
        result = value.pwszVal;
    }

    PropVariantClear(&value);
    return result;
}

bool matchesName(IMMDevice *device, const std::wstring& name)
{
    ComPtr<IPropertyStore> store;

    if (FAILED(device->OpenPropertyStore(STGM_READ, store.put()))) {
        return false;
    }

    if (propertyString(store.get(), PKEY_Device_DeviceDesc) == name) {
        return true;
    }

    std::wstring friendly = propertyString(store.get(), PKEY_Device_FriendlyName);
    std::wstring prefix = name + L" (";

    return friendly == name ||
        (friendly.size() >= prefix.size() &&
         friendly.compare(0, prefix.size(), prefix) == 0);
}

// Returns the first active endpoint of the given flow whose name matches.
bool findActiveEndpoint(
    IMMDeviceEnumerator *enumerator, EDataFlow flow, const std::wstring& name,
    IMMDevice **found)
{
    ComPtr<IMMDeviceCollection> collection;

    if (FAILED(enumerator->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, collection.put()))) {
        return false;
    }

    UINT count = 0;

    collection->GetCount(&count);

    for (UINT i = 0; i < count; ++i) {
        ComPtr<IMMDevice> device;

        if (FAILED(collection->Item(i, device.put()))) {
            continue;
        }

        if (matchesName(device.get(), name)) {
            if (found) {
                *found = device.detach();
            }

            return true;
        }
    }

    return false;
}

bool createEnumerator(ComPtr<IMMDeviceEnumerator> *enumerator)
{
    HRESULT hr = CoCreateInstance(
        __uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
        __uuidof(IMMDeviceEnumerator), enumerator->putVoid());

    if (FAILED(hr)) {
        logf("CoCreateInstance(MMDeviceEnumerator) failed: %s", hresultText(hr).c_str());
        return false;
    }

    return true;
}

void setUnityVolume(IMMDevice *device, IAudioClient *client)
{
    ComPtr<IAudioEndpointVolume> endpointVolume;

    if (SUCCEEDED(device->Activate(
        __uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr, endpointVolume.putVoid()))) {
        endpointVolume->SetMasterVolumeLevelScalar(1.0f, nullptr);
        endpointVolume->SetMute(FALSE, nullptr);
    }

    ComPtr<ISimpleAudioVolume> sessionVolume;

    if (SUCCEEDED(client->GetService(__uuidof(ISimpleAudioVolume), sessionVolume.putVoid()))) {
        sessionVolume->SetMasterVolume(1.0f, nullptr);
        sessionVolume->SetMute(FALSE, nullptr);
    }
}

class Stream
{
public:
    Stream(const EndpointLayout& layout, int pair, IMMDevice *device, bool isCapture,
           int maxReopens, long long maxTransitionFrames)
        : _device(device), _isCapture(isCapture), _maxReopens(maxReopens),
          _maxTransitionFrames(maxTransitionFrames)
    {
        _device->AddRef();

        LPWSTR id = nullptr;

        if (SUCCEEDED(_device->GetId(&id)) && id) {
            _deviceId = id;
            CoTaskMemFree(id);
        }

        _stats.isCapture = isCapture;
        _stats.name = narrow(isCapture ? layout.recordingName(pair) : layout.playbackName(pair));

        for (int c = 0; c < layout.channels; ++c) {
            _channelIds.push_back(layout.channelId(pair, c));
        }
    }

    ~Stream()
    {
        end();
        _device->Release();
    }

    void begin()
    {
        _stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        _thread = std::thread([this] { run(); });
    }

    void end()
    {
        if (_stopEvent) {
            SetEvent(_stopEvent);
        }

        if (_thread.joinable()) {
            _thread.join();
        }

        if (_stopEvent) {
            CloseHandle(_stopEvent);
            _stopEvent = nullptr;
        }
    }

    const StreamStats& stats() const { return _stats; }

private:
    void run()
    {
        HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);

        bool gaveUp = false;

        for (;;) {
            _invalidated = false;
            _attemptStarted = false;

            if (_isCapture) {
                captureLoop();
            } else {
                renderLoop();
            }

            // Stopped normally, or failed for a reason reopening won't fix.
            if (!_invalidated) {
                break;
            }

            // The run is over anyway.
            if (WaitForSingleObject(_stopEvent, 0) == WAIT_OBJECT_0) {
                break;
            }

            if (_stats.reopens >= _maxReopens) {
                gaveUp = true;
                break;
            }

            if (WaitForSingleObject(_stopEvent, 500) == WAIT_OBJECT_0) {
                break;
            }

            _stats.reopens++;
            logf("%s: reopening the stream (%d of %d)",
                _stats.name.c_str(), _stats.reopens, _maxReopens);
            endBadRun(true);
            flushSilentRun(" (then invalidated)");

            // A reopened stream ramps in again and its sequence resumes from
            // wherever the render side is by then.
            _lockedIn = false;
            _haveLastSequence = false;
            refreshDevice();
        }

        endBadRun(true);
        flushSilentRun(" (until the end)");

        if (gaveUp) {
            _stats.deviceInvalidated = true;
        }

        if (SUCCEEDED(hr)) {
            CoUninitialize();
        }
    }

    // Re-resolves the endpoint by ID so a retry does not reuse a device
    // object that belonged to the invalidated instance.
    void refreshDevice()
    {
        ComPtr<IMMDeviceEnumerator> enumerator;
        IMMDevice *fresh = nullptr;

        if (_deviceId.empty() || !createEnumerator(&enumerator)) {
            return;
        }

        if (SUCCEEDED(enumerator->GetDevice(_deviceId.c_str(), &fresh)) && fresh) {
            _device->Release();
            _device = fresh;
        }
    }

    bool fail(const char *stage, HRESULT hr)
    {
        if (hr == AUDCLNT_E_DEVICE_INVALIDATED) {
            char buf[128];

            _invalidated = true;
            _stats.invalidations++;
            sprintf_s(buf, "invalidated during %s at %.3f s", stage, (nowMs() - g_runOriginMs) / 1000.0);
            recordGap(buf);
            logf("%s: device invalidated during %s", _stats.name.c_str(), stage);
        } else if (!_attemptStarted) {
            // A setup call can fail while the endpoint is being reconfigured
            // (the mix format goes stale, for one); retry it like an
            // invalidation. The error sticks only if the retries run out.
            _invalidated = true;
            _stats.setupErrors++;
            _stats.lastError = hr;
            _stats.errorStage = stage;
            logf("%s: %s failed during setup: %s", _stats.name.c_str(), stage, hresultText(hr).c_str());
        } else {
            _stats.lastError = hr;
            _stats.errorStage = stage;
            logf("%s: %s failed: %s", _stats.name.c_str(), stage, hresultText(hr).c_str());
        }

        return false;
    }

    // Shared-mode, event-driven client on the mix format.
    bool initialize(ComPtr<IAudioClient> *client, UINT32 *bufferFrames, HANDLE *event)
    {
        HRESULT hr = _device->Activate(
            __uuidof(IAudioClient), CLSCTX_ALL, nullptr, client->putVoid());

        if (FAILED(hr)) {
            return fail("Activate", hr);
        }

        WAVEFORMATEX *mix = nullptr;

        hr = (*client)->GetMixFormat(&mix);

        if (FAILED(hr) || !mix) {
            return fail("GetMixFormat", hr);
        }

        _kind = classifyFormat(mix, &_stats.format);
        _stats.channels = mix->nChannels;
        _rate = mix->nSamplesPerSec ? (double)mix->nSamplesPerSec : 48000.0;

        if (_kind == SampleKind::Unsupported) {
            CoTaskMemFree(mix);
            _stats.errorStage = "format";
            logf("%s: unsupported mix format %s", _stats.name.c_str(), _stats.format.c_str());
            return false;
        }

        if ((int)mix->nChannels < (int)_channelIds.size()) {
            logf("%s: engine format has %u channels but the endpoint was created with %zu",
                _stats.name.c_str(), (unsigned)mix->nChannels, _channelIds.size());
        }

        hr = (*client)->Initialize(
            AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
            0, 0, mix, nullptr);
        CoTaskMemFree(mix);

        if (FAILED(hr)) {
            return fail("Initialize", hr);
        }

        hr = (*client)->GetBufferSize(bufferFrames);

        if (FAILED(hr)) {
            return fail("GetBufferSize", hr);
        }

        *event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        hr = (*client)->SetEventHandle(*event);

        if (FAILED(hr)) {
            return fail("SetEventHandle", hr);
        }

        setUnityVolume(_device, client->get());
        return true;
    }

    void fill(BYTE *data, UINT32 frames)
    {
        int channels = _stats.channels;

        for (UINT32 f = 0; f < frames; ++f, ++_sequence) {
            for (int c = 0; c < channels; ++c) {
                int id = c < (int)_channelIds.size() ? _channelIds[(size_t)c] : 0;
                size_t index = (size_t)f * (size_t)channels + (size_t)c;

                if (_kind == SampleKind::Float32) {
                    ((float *)data)[index] = encodeFloat(id, _sequence);
                } else {
                    ((int32_t *)data)[index] = encodeSample(id, _sequence) << 8;
                }
            }
        }
    }

    void recordGap(const char *text)
    {
        if (_stats.gapSamples.size() < 16) {
            _stats.gapSamples.push_back(text);
        }
    }

    void flushSilentRun(const char *suffix)
    {
        if (_silentRun > 0) {
            char buf[192];

            sprintf_s(buf, "dropout of %lld frames (%.1f ms) at %.3f s%s",
                _silentRun, _silentRun * 1000.0 / _rate, _silentRunStartMs / 1000.0, suffix);
            recordGap(buf);
            _silentRun = 0;
        }
    }

    // True when the frame is the test signal scaled by one gain on every
    // channel: the engine ramping a stream's volume, not corrupted data.
    // Sample k on a channel with id i is i * 65536 + seq, so two channels
    // give the gain from their difference and every channel must agree.
    bool isScaledSignal(const int32_t *values, int count) const
    {
        if (count < 2 || _channelIds[0] == _channelIds[1]) {
            return false;
        }

        double ida = (double)_channelIds[0], idb = (double)_channelIds[1];
        double gain = (double)(values[1] - values[0]) / ((idb - ida) * 65536.0);

        if (!(gain > 0.0 && gain < 1.0)) {
            return false;
        }

        double seq = (double)values[0] / gain - ida * 65536.0;

        if (seq < -2.0 || seq > 65537.0) {
            return false;
        }

        for (int c = 0; c < count && c < 32; ++c) {
            double expected = gain * ((double)_channelIds[(size_t)c] * 65536.0 + seq);

            if (fabs((double)values[c] - expected) > 3.0 + fabs(expected) * 0.002) {
                return false;
            }
        }

        return true;
    }

    // Classifies a finished run of frames that did not decode. Frames that
    // are the signal scaled by one gain are a ramp wherever they occur (a
    // reopened stream can fade in with no silence before it). Other frames
    // are corruption, unless the run borders silence or the start of the
    // stream, where the gain is too small for the scaling to survive
    // rounding. A run longer than any ramp is corruption regardless.
    void endBadRun(bool bordersGap)
    {
        if (_badRun == 0) {
            return;
        }

        long long corrupt = bordersGap ? 0 : _badRunUnscaled;

        if (_badRun > _maxTransitionFrames) {
            corrupt = _badRun;
        }

        bool ramp = corrupt == 0;
        char head[160];

        _stats.transitionFrames += _badRun - corrupt;
        _stats.wrongChannelFrames += corrupt;

        if (ramp) {
            _stats.rampRuns++;
        } else {
            _stats.corruptRuns++;
        }

        sprintf_s(head, "%s run of %lld frames (%lld not a scaled signal) at %.3f s",
            ramp ? "ramp" : "corrupt", _badRun, _badRunUnscaled, _badRunStartMs / 1000.0);

        if (!ramp) {
            logf("%s: %s", _stats.name.c_str(), head);
        }

        // Keep room for corruption: ramps only get the first half of the list.
        size_t limit = ramp ? 8 : 16;

        if (_stats.badFrameSamples.size() < limit) {
            _stats.badFrameSamples.push_back(head);

            for (const auto& sample : _badRunSamples) {
                if (_stats.badFrameSamples.size() >= limit) {
                    break;
                }

                _stats.badFrameSamples.push_back("  " + sample);

                if (!ramp) {
                    logf("%s:   %s", _stats.name.c_str(), sample.c_str());
                }
            }
        }

        _badRun = 0;
        _badRunUnscaled = 0;
        _badRunSamples.clear();
    }

    void decode(const BYTE *data, UINT32 frames)
    {
        double packetMs = nowMs() - g_runOriginMs;
        int channels = _stats.channels;
        int expectedChannels = std::min(channels, (int)_channelIds.size());

        for (UINT32 f = 0; f < frames; ++f) {
            bool allZero = true, channelsOk = true, sequenceConsistent = true;
            uint32_t sequence = 0;
            int32_t values[32] = {};

            for (int c = 0; c < expectedChannels; ++c) {
                size_t index = (size_t)f * (size_t)channels + (size_t)c;
                int32_t k;

                if (_kind == SampleKind::Float32) {
                    k = decodeFloat(((const float *)data)[index]);
                } else {
                    k = ((const int32_t *)data)[index] >> 8;
                }

                if (c < 32) {
                    values[c] = k;
                }

                if (k != 0) {
                    allZero = false;
                }

                if (decodeChannel(k) != _channelIds[(size_t)c]) {
                    channelsOk = false;
                }

                if (c == 0) {
                    sequence = decodeSeq(k);
                } else if (decodeSeq(k) != sequence) {
                    sequenceConsistent = false;
                }
            }

            if (allZero) {
                // Silence right after undecodable frames: the engine faded out.
                endBadRun(true);
                _prevSilent = true;
                _stats.silentFrames++;

                if (_stats.validFrames == 0) {
                    _stats.startupSilentFrames++;
                }

                if (_lockedIn) {
                    _stats.midStreamSilentFrames++;

                    if (_silentRun++ == 0) {
                        _silentRunStartMs = packetMs;
                    }
                }

                continue;
            }

            flushSilentRun("");

            if (!channelsOk || !sequenceConsistent) {
                if (_badRun == 0) {
                    _badRunBordersGap = !_lockedIn || _prevSilent;
                    _badRunStartMs = packetMs;
                }

                _badRun++;
                _prevSilent = false;

                if (!isScaledSignal(values, expectedChannels)) {
                    _badRunUnscaled++;
                }

                if (_badRunSamples.size() < 3) {
                    char buf[512];
                    int len = sprintf_s(buf, "frame %lld (after %lld valid):",
                        _stats.framesProcessed + (long long)f, _stats.validFrames);

                    for (int c = 0; c < expectedChannels && c < 32 && len > 0 && len < 480; ++c) {
                        len += sprintf_s(buf + len, sizeof(buf) - (size_t)len,
                            " %08X/%02X", (unsigned int)values[c], (unsigned int)_channelIds[(size_t)c]);
                    }

                    _badRunSamples.push_back(buf);
                }

                continue;
            }

            // A valid frame ends a run that started after silence (fade in)
            // as a ramp, and one that started after valid frames as corruption.
            endBadRun(_badRunBordersGap);
            _prevSilent = false;

            if (_haveLastSequence && sequence != ((_lastSequence + 1) & 0xFFFF)) {
                char buf[128];

                _stats.discontinuities++;
                sprintf_s(buf, "sequence jump of %u frames at %.3f s",
                    (unsigned int)((sequence - _lastSequence - 1) & 0xFFFF), packetMs / 1000.0);
                recordGap(buf);
            }

            _lastSequence = sequence;
            _haveLastSequence = true;
            _lockedIn = true;
            _stats.validFrames++;
        }
    }

    void renderLoop()
    {
        ComPtr<IAudioClient> client;
        UINT32 bufferFrames = 0;
        HANDLE event = nullptr;

        if (!initialize(&client, &bufferFrames, &event)) {
            if (event) {
                CloseHandle(event);
            }

            return;
        }

        ComPtr<IAudioRenderClient> render;
        HRESULT hr = client->GetService(__uuidof(IAudioRenderClient), render.putVoid());

        if (FAILED(hr)) {
            fail("GetService(IAudioRenderClient)", hr);
            CloseHandle(event);
            return;
        }

        BYTE *data = nullptr;

        if (SUCCEEDED(render->GetBuffer(bufferFrames, &data))) {
            fill(data, bufferFrames);
            render->ReleaseBuffer(bufferFrames, 0);
        }

        hr = client->Start();

        if (FAILED(hr)) {
            fail("Start", hr);
            CloseHandle(event);
            return;
        }

        _stats.started = true;
        _attemptStarted = true;
        _stats.lastError = S_OK;
        _stats.errorStage.clear();
        logf("%s: rendering %s, buffer %u frames", _stats.name.c_str(),
            _stats.format.c_str(), (unsigned)bufferFrames);

        HANDLE handles[2] = { _stopEvent, event };

        for (;;) {
            DWORD wait = WaitForMultipleObjects(2, handles, FALSE, 2000);

            if (wait == WAIT_OBJECT_0) {
                break;
            }

            if (wait != WAIT_OBJECT_0 + 1) {
                _stats.timeouts++;

                if (_stats.timeouts >= 5) {
                    _stats.errorStage = "event timeout";
                    logf("%s: no buffer events for 10 s", _stats.name.c_str());
                    break;
                }

                continue;
            }

            UINT32 padding = 0;

            hr = client->GetCurrentPadding(&padding);

            if (FAILED(hr)) {
                fail("GetCurrentPadding", hr);
                break;
            }

            UINT32 available = padding < bufferFrames ? bufferFrames - padding : 0;

            if (available == 0) {
                continue;
            }

            hr = render->GetBuffer(available, &data);

            if (FAILED(hr)) {
                fail("GetBuffer", hr);
                break;
            }

            fill(data, available);
            hr = render->ReleaseBuffer(available, 0);

            if (FAILED(hr)) {
                fail("ReleaseBuffer", hr);
                break;
            }

            _stats.framesProcessed += available;
        }

        client->Stop();
        CloseHandle(event);
    }

    void captureLoop()
    {
        ComPtr<IAudioClient> client;
        UINT32 bufferFrames = 0;
        HANDLE event = nullptr;

        if (!initialize(&client, &bufferFrames, &event)) {
            if (event) {
                CloseHandle(event);
            }

            return;
        }

        ComPtr<IAudioCaptureClient> capture;
        HRESULT hr = client->GetService(__uuidof(IAudioCaptureClient), capture.putVoid());

        if (FAILED(hr)) {
            fail("GetService(IAudioCaptureClient)", hr);
            CloseHandle(event);
            return;
        }

        hr = client->Start();

        if (FAILED(hr)) {
            fail("Start", hr);
            CloseHandle(event);
            return;
        }

        _stats.started = true;
        _attemptStarted = true;
        _stats.lastError = S_OK;
        _stats.errorStage.clear();
        logf("%s: capturing %s, buffer %u frames", _stats.name.c_str(),
            _stats.format.c_str(), (unsigned)bufferFrames);

        HANDLE handles[2] = { _stopEvent, event };

        for (;;) {
            DWORD wait = WaitForMultipleObjects(2, handles, FALSE, 2000);

            if (wait == WAIT_OBJECT_0) {
                break;
            }

            if (wait != WAIT_OBJECT_0 + 1) {
                _stats.timeouts++;

                if (_stats.timeouts >= 5) {
                    _stats.errorStage = "event timeout";
                    logf("%s: no capture events for 10 s", _stats.name.c_str());
                    break;
                }

                continue;
            }

            UINT32 packet = 0;

            hr = capture->GetNextPacketSize(&packet);

            if (FAILED(hr)) {
                fail("GetNextPacketSize", hr);
                break;
            }

            bool failed = false;

            while (packet > 0) {
                BYTE *data = nullptr;
                UINT32 frames = 0;
                DWORD flags = 0;

                hr = capture->GetBuffer(&data, &frames, &flags, nullptr, nullptr);

                if (FAILED(hr)) {
                    fail("GetBuffer", hr);
                    failed = true;
                    break;
                }

                if (flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) {
                    _stats.engineDiscontinuities++;
                }

                if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
                    _stats.silentFrames += frames;

                    if (_stats.validFrames == 0) {
                        _stats.startupSilentFrames += frames;
                    }
                } else {
                    decode(data, frames);
                }

                hr = capture->ReleaseBuffer(frames);

                if (FAILED(hr)) {
                    fail("ReleaseBuffer", hr);
                    failed = true;
                    break;
                }

                _stats.framesProcessed += frames;
                hr = capture->GetNextPacketSize(&packet);

                if (FAILED(hr)) {
                    fail("GetNextPacketSize", hr);
                    failed = true;
                    break;
                }
            }

            if (failed) {
                break;
            }
        }

        client->Stop();
        CloseHandle(event);
    }

    IMMDevice *_device;
    bool _isCapture;
    StreamStats _stats;
    std::vector<int> _channelIds;
    SampleKind _kind = SampleKind::Unsupported;
    uint32_t _sequence = 0;
    uint32_t _lastSequence = 0;
    bool _haveLastSequence = false;
    bool _lockedIn = false;
    long long _silentRun = 0;
    double _silentRunStartMs = 0.0;
    double _rate = 48000.0;
    std::wstring _deviceId;
    int _maxReopens = 0;
    long long _maxTransitionFrames = 4800;
    bool _invalidated = false;
    bool _attemptStarted = false;
    bool _prevSilent = false;
    long long _badRun = 0;
    long long _badRunUnscaled = 0;
    bool _badRunBordersGap = false;
    double _badRunStartMs = 0.0;
    std::vector<std::string> _badRunSamples;
    HANDLE _stopEvent = nullptr;
    std::thread _thread;
};

} // namespace

FoundEndpoints::~FoundEndpoints()
{
    release();
}

void FoundEndpoints::release()
{
    for (auto *device : render) {
        if (device) {
            device->Release();
        }
    }

    for (auto *device : capture) {
        if (device) {
            device->Release();
        }
    }

    render.clear();
    capture.clear();
}

bool findEndpoints(const EndpointLayout& layout, int timeoutSeconds, FoundEndpoints *out)
{
    ComPtr<IMMDeviceEnumerator> enumerator;

    if (!createEnumerator(&enumerator)) {
        return false;
    }

    out->release();
    out->render.assign((size_t)layout.pairs, nullptr);
    out->capture.assign((size_t)layout.pairs, nullptr);

    double deadline = nowMs() + timeoutSeconds * 1000.0;
    double nextReport = nowMs() + 5000.0;

    for (;;) {
        int missing = 0;

        for (int pair = 0; pair < layout.pairs; ++pair) {
            if (!out->render[(size_t)pair]) {
                findActiveEndpoint(enumerator.get(), eRender,
                    layout.playbackName(pair), &out->render[(size_t)pair]);
            }

            if (!out->capture[(size_t)pair]) {
                findActiveEndpoint(enumerator.get(), eCapture,
                    layout.recordingName(pair), &out->capture[(size_t)pair]);
            }

            missing += !out->render[(size_t)pair];
            missing += !out->capture[(size_t)pair];
        }

        if (missing == 0) {
            logf("All %d endpoints are active", layout.pairs * 2);
            return true;
        }

        if (nowMs() > deadline) {
            for (int pair = 0; pair < layout.pairs; ++pair) {
                if (!out->render[(size_t)pair]) {
                    logf("Missing render endpoint: %s", narrow(layout.playbackName(pair)).c_str());
                }

                if (!out->capture[(size_t)pair]) {
                    logf("Missing capture endpoint: %s", narrow(layout.recordingName(pair)).c_str());
                }
            }

            return false;
        }

        if (nowMs() > nextReport) {
            logf("Waiting for %d of %d endpoints to become active", missing, layout.pairs * 2);
            nextReport = nowMs() + 5000.0;
        }

        Sleep(500);
    }
}

bool waitForEndpointsGone(const EndpointLayout& layout, int timeoutSeconds)
{
    ComPtr<IMMDeviceEnumerator> enumerator;

    if (!createEnumerator(&enumerator)) {
        return false;
    }

    double deadline = nowMs() + timeoutSeconds * 1000.0;

    for (;;) {
        int present = 0;

        for (int pair = 0; pair < layout.pairs; ++pair) {
            present += findActiveEndpoint(enumerator.get(), eRender, layout.playbackName(pair), nullptr);
            present += findActiveEndpoint(enumerator.get(), eCapture, layout.recordingName(pair), nullptr);
        }

        if (present == 0) {
            return true;
        }

        if (nowMs() > deadline) {
            logf("%d endpoints are still active after %d s", present, timeoutSeconds);
            return false;
        }

        Sleep(500);
    }
}

std::string StreamStats::toJson() const
{
    JsonObject o;

    o.setString("name", name)
        .setString("direction", isCapture ? "capture" : "render")
        .setBool("started", started)
        .setBool("deviceInvalidated", deviceInvalidated)
        .setString("error", lastError == S_OK ? "" : hresultText(lastError))
        .setString("errorStage", errorStage)
        .setString("format", format)
        .setInt("channels", channels)
        .setInt("framesProcessed", framesProcessed)
        .setInt("silentFrames", silentFrames)
        .setInt("startupSilentFrames", startupSilentFrames)
        .setInt("validFrames", validFrames)
        .setInt("discontinuities", discontinuities)
        .setInt("engineDiscontinuities", engineDiscontinuities)
        .setInt("wrongChannelFrames", wrongChannelFrames)
        .setInt("transitionFrames", transitionFrames)
        .setInt("midStreamSilentFrames", midStreamSilentFrames)
        .setInt("invalidations", invalidations)
        .setInt("reopens", reopens)
        .setInt("setupErrors", setupErrors)
        .setInt("rampRuns", rampRuns)
        .setInt("corruptRuns", corruptRuns)
        .setInt("timeouts", timeouts)
        .setBool("passed", passed)
        .setString("failure", failure);

    std::vector<std::string> bad;

    for (const auto& sample : badFrameSamples) {
        bad.push_back(jsonString(sample));
    }

    o.setRaw("badFrames", jsonArray(bad));

    std::vector<std::string> gaps;

    for (const auto& gap : gapSamples) {
        gaps.push_back(jsonString(gap));
    }

    o.setRaw("gaps", jsonArray(gaps));
    return o.str();
}

std::string WasapiResult::toJson() const
{
    std::vector<std::string> items;

    for (const auto& s : streams) {
        items.push_back(s.toJson());
    }

    JsonObject o;

    o.setBool("passed", passed).setRaw("streams", jsonArray(items));
    return o.str();
}

static void evaluate(StreamStats *s, const WasapiOptions& options)
{
    s->passed = false;

    if (!s->started) {
        s->failure = "stream never started";
        return;
    }

    if (s->lastError != S_OK) {
        s->failure = "error in " + s->errorStage;
        return;
    }

    if (!s->errorStage.empty()) {
        s->failure = s->errorStage;
        return;
    }

    if (s->deviceInvalidated && !options.expectInvalidation) {
        s->failure = "device invalidated and not recovered";
        return;
    }

    if (s->framesProcessed == 0) {
        s->failure = "no frames processed";
        return;
    }

    if (s->isCapture) {
        long long considered = s->framesProcessed - s->startupSilentFrames;

        if (s->validFrames == 0) {
            s->failure = "no valid test signal received";
            return;
        }

        if (s->wrongChannelFrames > 0) {
            s->failure = "corrupt frames";
            return;
        }

        if (considered > 0 &&
            (double)s->validFrames < options.minValidRatio * (double)considered) {
            s->failure = "too few valid frames";
            return;
        }

        if (options.maxDiscontinuities >= 0 &&
            s->discontinuities > options.maxDiscontinuities) {
            s->failure = "too many discontinuities";
            return;
        }
    }

    s->passed = true;
}

WasapiResult runWasapi(const WasapiOptions& options, const FoundEndpoints& endpoints)
{
    WasapiResult result;
    std::vector<std::unique_ptr<Stream>> streams;
    const EndpointLayout& layout = options.layout;

    for (int pair = 0; pair < layout.pairs; ++pair) {
        streams.emplace_back(new Stream(
            layout, pair, endpoints.render[(size_t)pair], false, options.maxReopens,
            options.maxTransitionFrames));
        streams.emplace_back(new Stream(
            layout, pair, endpoints.capture[(size_t)pair], true, options.maxReopens,
            options.maxTransitionFrames));
    }

    if (options.settleSeconds > 0) {
        logf("Letting the endpoints settle for %.1f s", options.settleSeconds);
        Sleep((DWORD)(options.settleSeconds * 1000.0));
    }

    g_runOriginMs = nowMs();

    for (auto& stream : streams) {
        stream->begin();
    }

    logf("Streaming on %zu endpoints for %.1f s", streams.size(), options.durationSeconds);
    Sleep((DWORD)(options.durationSeconds * 1000.0));

    // Stop captures first so the tail of silence after playback ends is
    // not counted against them.
    for (auto& stream : streams) {
        if (stream->stats().isCapture) {
            stream->end();
        }
    }

    for (auto& stream : streams) {
        stream->end();
    }

    result.passed = true;

    for (auto& stream : streams) {
        StreamStats stats = stream->stats();

        evaluate(&stats, options);

        if (stats.isCapture) {
            logf("%s: %lld frames, %lld valid, %lld silent (%lld at start, %lld dropout), "
                "%lld ramp, %lld discontinuities, %lld corrupt, %d reopens, %d setup errors: %s",
                stats.name.c_str(), stats.framesProcessed, stats.validFrames,
                stats.silentFrames, stats.startupSilentFrames, stats.midStreamSilentFrames,
                stats.transitionFrames, stats.discontinuities, stats.wrongChannelFrames,
                stats.reopens, stats.setupErrors, stats.passed ? "PASS" : stats.failure.c_str());
        } else {
            logf("%s: %lld frames rendered, %d reopens: %s", stats.name.c_str(),
                stats.framesProcessed, stats.reopens,
                stats.passed ? "PASS" : stats.failure.c_str());
        }

        result.passed = result.passed && stats.passed;
        result.streams.push_back(stats);
    }

    return result;
}

// ---------------------------------------------------------------------------
// Start-up race: stream openers.

static const char *const kIdlePhase = "idle";

struct RaceOpeners::Worker
{
    RaceOpeners *owner = nullptr;
    int index = 0;
    std::atomic<const char *> phase{ kIdlePhase };
    std::atomic<double> phaseStartMs{ 0.0 };
    std::atomic<bool> hangReported{ false };
    std::mutex lock;
    RaceStats stats;
};

std::string RaceStats::toJson() const
{
    std::vector<std::string> errorItems;

    for (const auto& error : errors) {
        JsonObject item;

        item.setString("call", error.first).setInt("count", error.second);
        errorItems.push_back(item.str());
    }

    JsonObject o;

    o.setInt("attempts", attempts)
        .setInt("opened", opened)
        .setInt("noEndpoint", noEndpoint)
        .setInt("hangsReported", hangsReported)
        .setDouble("maxCallMs", maxCallMs)
        .setString("maxCall", maxCallName)
        .setRaw("errors", jsonArray(errorItems));
    return o.str();
}

RaceOpeners::RaceOpeners(
    const EndpointLayout& layout, int threads, int holdMs, int hangTimeoutSeconds)
    : _layout(layout), _holdMs(holdMs), _hangTimeoutMs(hangTimeoutSeconds * 1000)
{
    for (int i = 0; i < threads; ++i) {
        _workers.emplace_back(new Worker());
        _workers.back()->owner = this;
        _workers.back()->index = i;
    }
}

RaceOpeners::~RaceOpeners()
{
    stop();

    // A thread stuck inside a WASAPI call still references its worker.
    if (!_clean) {
        for (auto& worker : _workers) {
            worker.release();
        }
    }
}

void RaceOpeners::start()
{
    if (!_threads.empty()) {
        return;
    }

    _stop = false;
    _watchdogStop = false;
    _stopped = false;
    _clean = true;

    for (auto& worker : _workers) {
        HANDLE thread = CreateThread(
            nullptr, 0, &RaceOpeners::threadMain, worker.get(), 0, nullptr);

        if (thread) {
            _threads.push_back(thread);
        }
    }

    _watchdog = CreateThread(nullptr, 0, &RaceOpeners::watchdogMain, this, 0, nullptr);
}

bool RaceOpeners::stop()
{
    if (_stopped) {
        return _clean;
    }

    _stopped = true;
    _stop = true;

    if (!_threads.empty()) {
        DWORD wait = WaitForMultipleObjects(
            (DWORD)_threads.size(), _threads.data(), TRUE, (DWORD)_hangTimeoutMs + 5000);

        _clean = wait != WAIT_TIMEOUT && wait != WAIT_FAILED;

        if (!_clean) {
            logf("Stream openers did not finish within %d s", _hangTimeoutMs / 1000 + 5);
        }
    }

    _watchdogStop = true;

    if (_watchdog) {
        WaitForSingleObject(_watchdog, 5000);
        CloseHandle(_watchdog);
        _watchdog = nullptr;
    }

    if (_clean) {
        for (HANDLE thread : _threads) {
            CloseHandle(thread);
        }

        _threads.clear();
    }

    return _clean;
}

RaceStats RaceOpeners::stats()
{
    RaceStats total;

    for (auto& worker : _workers) {
        std::lock_guard<std::mutex> guard(worker->lock);
        const RaceStats& s = worker->stats;

        total.attempts += s.attempts;
        total.opened += s.opened;
        total.noEndpoint += s.noEndpoint;
        total.hangsReported += s.hangsReported;

        if (s.maxCallMs > total.maxCallMs) {
            total.maxCallMs = s.maxCallMs;
            total.maxCallName = s.maxCallName;
        }

        for (const auto& error : s.errors) {
            total.errors[error.first] += error.second;
        }
    }

    return total;
}

DWORD WINAPI RaceOpeners::threadMain(LPVOID param)
{
    Worker *worker = (Worker *)param;

    worker->owner->work(worker);
    return 0;
}

DWORD WINAPI RaceOpeners::watchdogMain(LPVOID param)
{
    ((RaceOpeners *)param)->watch();
    return 0;
}

void RaceOpeners::watch()
{
    while (!_watchdogStop) {
        Sleep(250);

        double now = nowMs();

        for (auto& worker : _workers) {
            const char *phase = worker->phase.load();

            if (phase == kIdlePhase) {
                continue;
            }

            double elapsed = now - worker->phaseStartMs.load();

            if (elapsed > (double)_hangTimeoutMs && !worker->hangReported.exchange(true)) {
                logf("HANG: stream opener %d stuck in %s for %.0f ms", worker->index, phase, elapsed);

                std::lock_guard<std::mutex> guard(worker->lock);
                worker->stats.hangsReported++;
            }
        }
    }
}

void RaceOpeners::work(Worker *worker)
{
    HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ComPtr<IMMDeviceEnumerator> enumerator;
    std::vector<std::wstring> names;
    unsigned int cursor = (unsigned int)worker->index;
    unsigned int rng = 0x9E3779B9u * (unsigned int)(worker->index + 1);

    // Even indices are playback (render) endpoints, odd ones recording (capture).
    for (int pair = 0; pair < _layout.pairs; ++pair) {
        names.push_back(_layout.playbackName(pair));
        names.push_back(_layout.recordingName(pair));
    }

    auto enter = [worker](const char *phase) {
        worker->phaseStartMs = nowMs();
        worker->phase = phase;
    };

    auto leave = [worker](const char *call, HRESULT hr) {
        double elapsed = nowMs() - worker->phaseStartMs.load();

        worker->phase = kIdlePhase;

        std::lock_guard<std::mutex> guard(worker->lock);

        if (elapsed > worker->stats.maxCallMs) {
            worker->stats.maxCallMs = elapsed;
            worker->stats.maxCallName = call;
        }

        if (FAILED(hr)) {
            worker->stats.errors[std::string(call) + " " + hresultText(hr)]++;
        }
    };

    if (createEnumerator(&enumerator)) {
        while (!_stop) {
            size_t which = cursor++ % names.size();
            EDataFlow flow = (which % 2 == 0) ? eRender : eCapture;
            ComPtr<IMMDevice> device;

            {
                std::lock_guard<std::mutex> guard(worker->lock);
                worker->stats.attempts++;
            }

            enter("EnumAudioEndpoints");
            bool found = findActiveEndpoint(enumerator.get(), flow, names[which], device.put());
            leave("EnumAudioEndpoints", S_OK);

            if (!found) {
                std::lock_guard<std::mutex> guard(worker->lock);
                worker->stats.noEndpoint++;
                Sleep(5);
                continue;
            }

            ComPtr<IAudioClient> client;

            enter("Activate");
            HRESULT hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, client.putVoid());
            leave("Activate", hr);

            if (SUCCEEDED(hr)) {
                WAVEFORMATEX *mix = nullptr;

                enter("GetMixFormat");
                hr = client->GetMixFormat(&mix);
                leave("GetMixFormat", hr);

                if (SUCCEEDED(hr) && mix) {
                    enter("Initialize");
                    hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, 200000, 0, mix, nullptr);
                    leave("Initialize", hr);
                    CoTaskMemFree(mix);

                    if (SUCCEEDED(hr)) {
                        enter("Start");
                        hr = client->Start();
                        leave("Start", hr);

                        if (SUCCEEDED(hr)) {
                            rng = rng * 1664525u + 1013904223u;
                            Sleep(_holdMs > 0 ? (DWORD)(rng % (unsigned int)(2 * _holdMs)) : 0);

                            enter("Stop");
                            hr = client->Stop();
                            leave("Stop", hr);

                            std::lock_guard<std::mutex> guard(worker->lock);
                            worker->stats.opened++;
                        }
                    }
                }
            }

            enter("Release");
            client.reset();
            device.reset();
            leave("Release", S_OK);
            Sleep(2);
        }
    }

    if (SUCCEEDED(co)) {
        CoUninitialize();
    }
}


} // namespace SarTest
