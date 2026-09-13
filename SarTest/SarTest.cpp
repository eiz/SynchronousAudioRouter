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

// SarTest: a headless test harness for SynchronousAudioRouter. See
// tools/test/README.md for the workflow it is designed around.

#include "common.h"
#include "install.h"
#include "asiohost.h"
#include "wasapi.h"
#include "clockstats.h"

using namespace SarTest;

namespace {

int usage()
{
    printf(
        "SarTest - SynchronousAudioRouter test harness\n"
        "\n"
        "  SarTest install [--inf <path>] [--clock <dll>] [--endpoints N] [--channels C]\n"
        "      Create the SAR device node, install the driver package from the INF,\n"
        "      register the software clock and write the SarAsio configuration.\n"
        "  SarTest uninstall [--clock <dll>] [--remove-device]\n"
        "  SarTest config [--endpoints N] [--channels C] [--out <path>]\n"
        "      Write only the SarAsio configuration.\n"
        "  SarTest host [--iterations K] [--duration S] [--sarasio <dll>]\n"
        "      Run the headless ASIO host: create SAR endpoints, tick, stop, repeat.\n"
        "  SarTest wasapi [--duration S] [--wait S] [--expect-invalidation]\n"
        "      Stream the test signal through the endpoints of a running host and\n"
        "      verify the loopback.\n"
        "  SarTest run [--iterations K] [--duration S]\n"
        "      host and wasapi in one process, with end-to-end verification.\n"
        "\n"
        "Layout options: --endpoints N (playback/recording pairs, default 2)\n"
        "                --channels C (per endpoint, default 2) --prefix <name>\n"
        "Other options:  --results <file.json> --phase-timeout S --no-sarasio-log\n"
        "                --keep-config (host/run: do not rewrite default.json)\n"
        "                --registered (use the COM-registered SarAsio instead of a path)\n"
        "                --wait S --wait-gone S --restart-delay MS --rate HZ\n"
        "                --min-valid RATIO --max-discontinuities N\n"
        "\n"
        "Exit codes: 0 pass, 1 setup or usage error, 2 test failure.\n");
    return 1;
}

std::wstring sibling(const wchar_t *file)
{
    wchar_t path[MAX_PATH] = {};

    GetModuleFileNameW(nullptr, path, MAX_PATH);

    std::wstring s = path;
    auto slash = s.find_last_of(L"\\/");

    if (slash == std::wstring::npos) {
        return file;
    }

    return s.substr(0, slash + 1) + file;
}

AsioHostOptions hostOptions(const Args& args, const EndpointLayout& layout)
{
    AsioHostOptions options;

    options.layout = layout;

    if (!args.has(L"registered")) {
        options.sarAsioPath = args.get(L"sarasio", sibling(L"SarAsio.dll").c_str());
    }

    options.sampleRate = args.getDouble(L"rate", 48000.0);
    options.phaseTimeoutSeconds = args.getInt(L"phase-timeout", 30);
    options.enableSarAsioLog = !args.has(L"no-sarasio-log");
    return options;
}

WasapiOptions wasapiOptions(const Args& args, const EndpointLayout& layout)
{
    WasapiOptions options;

    options.layout = layout;
    options.durationSeconds = args.getDouble(L"duration", 5.0);
    options.expectInvalidation = args.has(L"expect-invalidation");
    options.minValidRatio = args.getDouble(L"min-valid", 0.9);
    options.maxDiscontinuities = args.getInt(L"max-discontinuities", -1);
    return options;
}

// Writes default.json for the layout unless --keep-config, backing up an
// existing configuration the first time.
bool prepareConfig(const Args& args, const EndpointLayout& layout)
{
    if (args.has(L"keep-config")) {
        return true;
    }

    std::wstring path = configurationPath();
    std::wstring backup = path + L".sartest-backup";

    if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES &&
        GetFileAttributesW(backup.c_str()) == INVALID_FILE_ATTRIBUTES) {
        if (CopyFileW(path.c_str(), backup.c_str(), TRUE)) {
            logf("Backed up the existing configuration to %s", narrow(backup).c_str());
        }
    }

    if (!writeDriverConfig(layout, SAR_TEST_CLOCK_CLSID_STR,
        args.getInt(L"wavert-min-frames", 0), path)) {
        return false;
    }

    logf("Wrote configuration for %d endpoint pairs x %d channels to %s",
        layout.pairs, layout.channels, narrow(path).c_str());
    return true;
}

void writeResults(const Args& args, const std::string& json)
{
    std::wstring path = args.get(L"results", L"sartest-results.json");

    if (writeTextFile(path, json + "\n")) {
        logf("Results written to %s", narrow(path).c_str());
    }
}

int cmdInstall(const Args& args)
{
    EndpointLayout layout = EndpointLayout::fromArgs(args);
    std::wstring inf = args.get(L"inf");
    std::wstring clock = args.get(L"clock", sibling(L"SarTestClock.dll").c_str());

    if (!inf.empty()) {
        if (!deviceNodeExists() && !createDeviceNode()) {
            return 1;
        }

        bool reboot = false;

        if (!installDriverPackage(inf, &reboot)) {
            return 1;
        }
    } else if (!deviceNodeExists()) {
        logf("No SAR device node is present and no --inf was given");
        return 1;
    } else {
        logf("SAR device node present; leaving the installed driver alone (no --inf)");
    }

    if (!registerComServer(clock, false)) {
        return 1;
    }

    return prepareConfig(args, layout) ? 0 : 1;
}

int cmdUninstall(const Args& args)
{
    std::wstring clock = args.get(L"clock", sibling(L"SarTestClock.dll").c_str());
    int rc = registerComServer(clock, true) ? 0 : 1;

    if (args.has(L"remove-device") && !removeDeviceNode()) {
        rc = 1;
    }

    return rc;
}

int cmdConfig(const Args& args)
{
    EndpointLayout layout = EndpointLayout::fromArgs(args);
    std::wstring out = args.get(L"out", configurationPath().c_str());

    if (!writeDriverConfig(layout, SAR_TEST_CLOCK_CLSID_STR,
        args.getInt(L"wavert-min-frames", 0), out)) {
        return 1;
    }

    logf("Wrote %s", narrow(out).c_str());
    return 0;
}

int cmdHost(const Args& args)
{
    EndpointLayout layout = EndpointLayout::fromArgs(args);
    int iterations = args.getInt(L"iterations", 1);
    double duration = args.getDouble(L"duration", 5.0);
    int restartDelay = args.getInt(L"restart-delay", 1000);
    int waitGone = args.getInt(L"wait-gone", 30);

    if (!prepareConfig(args, layout)) {
        return 1;
    }

    AsioHost host(hostOptions(args, layout));
    std::vector<std::string> iterationJson;
    int rc = 0;

    if (!host.load() || !host.open()) {
        rc = 1;
    }

    for (int i = 0; rc == 0 && i < iterations; ++i) {
        logf("--- host iteration %d of %d ---", i + 1, iterations);

        if (!host.start()) {
            rc = 2;
            break;
        }

        Sleep((DWORD)(duration * 1000.0));

        if (!host.stop()) {
            rc = 2;
            break;
        }

        JsonObject iteration;

        iteration.setInt("iteration", i + 1)
            .setDouble("startMs", host.stats().lastStartMs)
            .setDouble("stopMs", host.stats().lastStopMs)
            .setInt("ticks", host.stats().ticks);
        iterationJson.push_back(iteration.str());

        if (i + 1 < iterations) {
            waitForEndpointsGone(layout, waitGone);
            Sleep((DWORD)restartDelay);
        }
    }

    host.close();

    JsonObject result;

    result.setString("command", "host")
        .setBool("passed", rc == 0)
        .setInt("exitCode", rc)
        .setRaw("host", host.toJson())
        .setRaw("iterations", jsonArray(iterationJson));
    writeResults(args, result.str());
    return rc;
}

int cmdWasapi(const Args& args)
{
    EndpointLayout layout = EndpointLayout::fromArgs(args);
    WasapiOptions options = wasapiOptions(args, layout);
    FoundEndpoints endpoints;

    if (!findEndpoints(layout, args.getInt(L"wait", 30), &endpoints)) {
        JsonObject result;

        result.setString("command", "wasapi")
            .setBool("passed", false)
            .setInt("exitCode", 1)
            .setString("failure", "endpoints not found");
        writeResults(args, result.str());
        return 1;
    }

    WasapiResult result = runWasapi(options, endpoints);
    JsonObject json;
    int rc = result.passed ? 0 : 2;

    json.setString("command", "wasapi")
        .setBool("passed", result.passed)
        .setInt("exitCode", rc)
        .setRaw("wasapi", result.toJson());
    writeResults(args, json.str());
    return rc;
}

int cmdRun(const Args& args)
{
    EndpointLayout layout = EndpointLayout::fromArgs(args);
    int iterations = args.getInt(L"iterations", 1);
    int restartDelay = args.getInt(L"restart-delay", 1000);
    int waitGone = args.getInt(L"wait-gone", 30);
    int waitEndpoints = args.getInt(L"wait", 30);
    WasapiOptions wasapi = wasapiOptions(args, layout);

    if (!prepareConfig(args, layout)) {
        return 1;
    }

    AsioHost host(hostOptions(args, layout));
    std::vector<std::string> iterationJson;
    int rc = 0;

    if (!host.load() || !host.open()) {
        rc = 1;
    }

    for (int i = 0; rc == 0 && i < iterations; ++i) {
        JsonObject iteration;

        logf("--- iteration %d of %d ---", i + 1, iterations);
        iteration.setInt("iteration", i + 1);

        if (!host.start()) {
            rc = 2;
            iteration.setString("failure", "start");
            iterationJson.push_back(iteration.str());
            break;
        }

        iteration.setDouble("startMs", host.stats().lastStartMs);

        FoundEndpoints endpoints;

        if (!findEndpoints(layout, waitEndpoints, &endpoints)) {
            rc = 2;
            iteration.setString("failure", "endpoints not found");
            host.stop();
            iterationJson.push_back(iteration.str());
            break;
        }

        WasapiResult result = runWasapi(wasapi, endpoints);

        endpoints.release();
        iteration.setRaw("wasapi", result.toJson());

        if (!result.passed) {
            rc = 2;
            iteration.setString("failure", "verification");
        }

        if (!host.stop()) {
            rc = 2;
            iteration.setString("failure", "stop");
        }

        iteration.setDouble("stopMs", host.stats().lastStopMs);
        iterationJson.push_back(iteration.str());

        if (rc == 0 && i + 1 < iterations) {
            if (!waitForEndpointsGone(layout, waitGone)) {
                logf("Endpoints lingered after stop");
            }

            Sleep((DWORD)restartDelay);
        }
    }

    host.close();

    JsonObject result;

    result.setString("command", "run")
        .setBool("passed", rc == 0)
        .setInt("exitCode", rc)
        .setInt("endpointPairs", layout.pairs)
        .setInt("channels", layout.channels)
        .setRaw("host", host.toJson())
        .setRaw("iterations", jsonArray(iterationJson));
    writeResults(args, result.str());
    logf("%s", rc == 0 ? "PASSED" : "FAILED");
    return rc;
}

} // namespace

int wmain(int argc, wchar_t **argv)
{
    Args args = parseArgs(argc, argv);

    if (args.command.empty() || args.has(L"help")) {
        return usage();
    }

    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    if (FAILED(hr)) {
        logf("CoInitializeEx failed: %s", hresultText(hr).c_str());
        return 1;
    }

    int rc;

    if (args.command == L"install") {
        rc = cmdInstall(args);
    } else if (args.command == L"uninstall") {
        rc = cmdUninstall(args);
    } else if (args.command == L"config") {
        rc = cmdConfig(args);
    } else if (args.command == L"host") {
        rc = cmdHost(args);
    } else if (args.command == L"wasapi") {
        rc = cmdWasapi(args);
    } else if (args.command == L"run") {
        rc = cmdRun(args);
    } else {
        rc = usage();
    }

    CoUninitialize();
    return rc;
}
