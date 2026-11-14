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

#include "contention.h"
#include "control.h"
#include "install.h"

#include <cfgmgr32.h>
#include <mmreg.h>
#include <ks.h>
#include <ksmedia.h>

#include <algorithm>
#include <atomic>
#include <thread>

namespace SarTest {

namespace {

enum Phase
{
    kBaseline,   // before the churning host first appears
    kCreating,   // the churning host is creating its endpoints
    kHeld,       // it has them all and does nothing
    kClosing,    // its control handle is being closed
    kBetween,    // it is gone; Windows is still tidying up after it
    kPhaseCount,
};

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

const char *const kPhaseNames[kPhaseCount] = {
    "baseline", "creating", "held", "closing", "between",
};

struct Sample
{
    int phase;
    double openMs;
    double closeMs;
};

struct Summary
{
    size_t count = 0;
    double median = 0.0;
    double p99 = 0.0;
    double max = 0.0;
};

Summary summarize(std::vector<double> values)
{
    Summary summary;

    if (values.empty()) {
        return summary;
    }

    std::sort(values.begin(), values.end());
    summary.count = values.size();
    summary.median = values[values.size() / 2];
    summary.p99 = values[(size_t)((values.size() - 1) * 0.99)];
    summary.max = values.back();
    return summary;
}

std::string summaryJson(const Summary& summary)
{
    JsonObject json;

    json.setInt("count", (long long)summary.count)
        .setDouble("medianMs", summary.median)
        .setDouble("p99Ms", summary.p99)
        .setDouble("maxMs", summary.max);
    return json.str();
}

std::wstring lower(std::wstring text)
{
    for (wchar_t& c : text) {
        c = (wchar_t)towlower(c);
    }

    return text;
}

// The interface path of the enabled wave filter of the endpoint with this
// ID; empty while Windows doesn't list one.
std::wstring waveFilterPath(const std::wstring& instanceId, const std::wstring& endpointId)
{
    ULONG length = 0;
    GUID category = KSCATEGORY_AUDIO;

    if (CM_Get_Device_Interface_List_SizeW(&length, &category, (DEVINSTID_W)instanceId.c_str(),
            CM_GET_DEVICE_INTERFACE_LIST_PRESENT) != CR_SUCCESS || length == 0) {
        return L"";
    }

    std::vector<wchar_t> buffer(length + 1, L'\0');

    if (CM_Get_Device_Interface_ListW(&category, (DEVINSTID_W)instanceId.c_str(), buffer.data(),
            length, CM_GET_DEVICE_INTERFACE_LIST_PRESENT) != CR_SUCCESS) {
        return L"";
    }

    // The reference string is the ID followed by the format
    // (driver/docs/behavior.md, section 7); the topology filter's ends in
    // "_topology".
    std::wstring wanted = L"\\" + lower(endpointId) + L"_";
    std::wstring topology = L"_topology";

    for (const wchar_t *p = buffer.data(); *p; p += wcslen(p) + 1) {
        std::wstring path = lower(p);

        if (path.find(wanted) != std::wstring::npos &&
            (path.size() < topology.size() ||
             path.compare(path.size() - topology.size(), topology.size(), topology) != 0)) {
            return p;
        }
    }

    return L"";
}

uint32_t setLayout(HANDLE device)
{
    LayoutRequest layout = {};
    LayoutReply reply = {};

    layout.bufferSize = 16 << 20;
    layout.sampleRate = 48000;
    layout.sampleSize = 4;
    layout.periodSizeBytes = 480 * 4;
    layout.minimumFrameCount = 2;
    return controlRequest(
        device, kSetBufferLayout, &layout, sizeof(layout), &reply, sizeof(reply)).status;
}

uint32_t createEndpoint(HANDLE device, uint32_t index, bool playback, const std::wstring& id)
{
    EndpointRequest endpoint = {};

    endpoint.type = playback ? 2 : 1;
    endpoint.index = index;
    endpoint.channelCount = 2;
    wcsncpy_s(endpoint.id, id.c_str(), _TRUNCATE);
    wcsncpy_s(endpoint.name, id.c_str(), _TRUNCATE);
    return controlRequest(
        device, kCreateEndpoint, &endpoint, sizeof(endpoint), nullptr, 0, 120000).status;
}

} // namespace

int cmdContention(const Args& args)
{
    int pairs = args.getInt(L"endpoints", 32);
    int rounds = args.getInt(L"rounds", 5);
    double limitMs = args.getDouble(L"max-wait-ms", 0.0);
    std::wstring controlPath = controlDevicePath();
    std::wstring instanceId = sarDeviceInstanceId();
    std::string failure;

    if (controlPath.empty() || instanceId.empty()) {
        logf("The SAR device is not present.");
        return 1;
    }

    // The bystander: a host with one endpoint in each direction, driven
    // straight through the control device so that nothing of SarAsio's is in
    // the measurement.
    HANDLE bystander = openControlDevice(controlPath);

    if (bystander == INVALID_HANDLE_VALUE) {
        logf("Couldn't open the control device: %s", lastErrorText(GetLastError()).c_str());
        return 1;
    }

    uint32_t status = setLayout(bystander);

    if (status == kStatusSuccess) {
        status = createEndpoint(bystander, 0, true, L"SarContendB-out");
    }

    if (status == kStatusSuccess) {
        status = createEndpoint(bystander, 1, false, L"SarContendB-in");
    }

    if (status != kStatusSuccess) {
        logf("Couldn't set up the bystander host: %s", statusText(status).c_str());
        CloseHandle(bystander);
        return 1;
    }

    std::wstring filterPath;

    for (int i = 0; i < 150 && filterPath.empty(); ++i) {
        filterPath = waveFilterPath(instanceId, L"SarContendB-out");

        if (filterPath.empty()) {
            Sleep(100);
        }
    }

    if (filterPath.empty()) {
        logf("The bystander's playback filter never appeared.");
        CloseHandle(bystander);
        return 1;
    }

    logf("Probing %s", narrow(filterPath).c_str());
    logf("%d rounds of a host with %d endpoints being created and closed", rounds, pairs * 2);

    std::atomic<int> phase(kBaseline);
    std::atomic<bool> stop(false);
    std::vector<Sample> samples;
    long long openFailures = 0;
    DWORD firstOpenError = 0;

    std::thread prober([&] {
        // A millisecond between probes. Sleep(1) lasts a whole timer tick,
        // 15.6 ms unless something has raised the resolution, which would
        // leave a handful of probes for each close of the other host.
        HANDLE timer = CreateWaitableTimerExW(
            nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        auto pause = [&] {
            LARGE_INTEGER due;

            due.QuadPart = -10000;

            if (timer && SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) {
                WaitForSingleObject(timer, INFINITE);
            } else {
                Sleep(1);
            }
        };

        while (!stop) {
            int before = phase;
            double t0 = nowMs();
            HANDLE filter = CreateFileW(
                filterPath.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED, nullptr);
            double t1 = nowMs();

            if (filter == INVALID_HANDLE_VALUE) {
                if (openFailures++ == 0) {
                    firstOpenError = GetLastError();
                }

                pause();
                continue;
            }

            CloseHandle(filter);

            double t2 = nowMs();
            int after = phase;

            // A probe that began while the other host was quiet and ended
            // after it had started creating or closing waited on that.
            int counted = (before == kCreating || before == kClosing) ? before : after;

            samples.push_back({ counted, t1 - t0, t2 - t1 });
            pause();
        }

        if (timer) {
            CloseHandle(timer);
        }
    });

    std::vector<double> createMs, closeMs;

    Sleep(2000);

    for (int round = 0; round < rounds && failure.empty(); ++round) {
        HANDLE churn = openControlDevice(controlPath);

        if (churn == INVALID_HANDLE_VALUE) {
            failure = "couldn't open the control device again: " + lastErrorText(GetLastError());
            break;
        }

        status = setLayout(churn);

        if (status != kStatusSuccess) {
            failure = "set buffer layout: " + statusText(status);
        }

        phase = kCreating;

        double start = nowMs();

        for (int i = 0; i < pairs * 2 && failure.empty(); ++i) {
            status = createEndpoint(
                churn, (uint32_t)i, (i & 1) == 0, L"SarContendA-" + std::to_wstring(i));

            if (status != kStatusSuccess) {
                failure = "create endpoint " + std::to_string(i) + ": " + statusText(status);
            }
        }

        createMs.push_back(nowMs() - start);
        phase = kHeld;
        Sleep(500);
        phase = kClosing;
        start = nowMs();
        CloseHandle(churn);
        closeMs.push_back(nowMs() - start);
        phase = kBetween;
        logf("  round %d: %d endpoints created in %.0f ms, closed in %.0f ms",
            round + 1, pairs * 2, createMs.back(), closeMs.back());
        Sleep(1500);
    }

    stop = true;
    prober.join();
    CloseHandle(bystander);

    std::vector<std::string> phases;
    double worstBusy = 0.0;

    for (int p = 0; p < kPhaseCount; ++p) {
        std::vector<double> opens, closes;

        for (const Sample& sample : samples) {
            if (sample.phase == p) {
                opens.push_back(sample.openMs);
                closes.push_back(sample.closeMs);
            }
        }

        Summary open = summarize(opens);
        Summary close = summarize(closes);
        JsonObject item;

        item.setString("phase", kPhaseNames[p])
            .setRaw("open", summaryJson(open))
            .setRaw("close", summaryJson(close));
        phases.push_back(item.str());
        logf("  %-9s %5zu probes; open median %.2f, 99th percentile %.2f, max %.2f ms; "
            "close median %.2f, max %.2f ms",
            kPhaseNames[p], open.count, open.median, open.p99, open.max, close.median, close.max);

        if (p == kCreating || p == kClosing) {
            worstBusy = std::max(worstBusy, std::max(open.max, close.max));
        }
    }

    if (failure.empty() && openFailures != 0) {
        failure = std::to_string(openFailures) + " opens of the bystander's filter failed, the first with " +
            lastErrorText(firstOpenError);
    }

    if (failure.empty() && limitMs > 0.0 && worstBusy > limitMs) {
        char text[160];

        sprintf_s(text, "an open or close waited %.0f ms while the other host was being created or closed; "
            "the limit is %.0f ms", worstBusy, limitMs);
        failure = text;
    }

    Summary create = summarize(createMs);
    Summary close = summarize(closeMs);
    JsonObject json;

    json.setString("command", "contention")
        .setBool("passed", failure.empty())
        .setInt("endpoints", pairs * 2)
        .setInt("rounds", (long long)createMs.size())
        .setDouble("worstWaitWhileBusyMs", worstBusy)
        .setDouble("limitMs", limitMs)
        .setRaw("createAllEndpoints", summaryJson(create))
        .setRaw("closeHost", summaryJson(close))
        .setRaw("phases", jsonArray(phases));

    if (!failure.empty()) {
        json.setString("failure", failure);
    }

    if (args.has(L"results")) {
        writeTextFile(args.get(L"results"), json.str());
    }

    if (failure.empty()) {
        logf("PASSED: the longest wait while the other host was being created or closed was %.0f ms",
            worstBusy);
        return 0;
    }

    logf("FAILED: %s", failure.c_str());
    return 2;
}

} // namespace SarTest
