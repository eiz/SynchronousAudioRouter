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
#include "watch.h"
#include "clockstats.h"

#include <prsht.h>
#include <thread>

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
        "  SarTest race [--cycles K] [--up S] [--down MS] [--openers T] [--hold MS]\n"
        "      Start and stop the host repeatedly while threads keep opening and\n"
        "      closing streams on its endpoints; report any call that hangs.\n"
        "  SarTest control-panel\n"
        "      Open SAR's control panel on a running host, press the hardware\n"
        "      interface's Configure button and close it again; fail if a second\n"
        "      clock instance was created (issue #133).\n"
        "  SarTest no-interface\n"
        "      Open SAR with no hardware interface configured, the way a DAW sees\n"
        "      it before first-time setup; fail unless it offers a stereo input\n"
        "      and output (issues #31, #54).\n"
        "  SarTest endpoint-names\n"
        "      Rename an endpoint keeping its ID, then give it a new ID, and report\n"
        "      the name Windows shows each time (issues #15, #66).\n"
        "  SarTest meter [--duration S] [--process <name.exe>] [--min-peak P]\n"
        "      Watch the audio sessions on every active playback endpoint and\n"
        "      report which processes played and how loud; with --process, fail\n"
        "      unless that process reached --min-peak (default 0.01).\n"
        "  SarTest watch [--duration S] [--stop-file <path>] [--poll MS] [--probe MS]\n"
        "      Log every change in which SAR KS interfaces are enabled and every\n"
        "      endpoint state change, with timestamps, until S seconds pass or\n"
        "      the stop file exists. --probe MS also opens every enabled SAR filter\n"
        "      that often and logs each change in the result.\n"
        "\n"
        "Layout options: --endpoints N (playback/recording pairs, default 2)\n"
        "                --channels C (per endpoint, default 2) --prefix <name>\n"
        "                --id-prefix <id> (endpoint IDs; default: the name prefix)\n"
        "Other options:  --results <file.json> --phase-timeout S --no-sarasio-log\n"
        "                --keep-config (host/run: do not rewrite default.json)\n"
        "                --app-routing (turn on application routing in default.json)\n"
        "                --expect-format (fail streams whose engine format isn't --rate\n"
        "                and --channels)\n"
        "                --registered (use the COM-registered SarAsio instead of a path)\n"
        "                --wait S --wait-gone S --restart-delay MS --rate HZ\n"
        "                --min-valid RATIO --max-discontinuities N\n"
        "                --settle S --max-reopens N --max-transition FRAMES\n"
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
    options.minValidRatio = args.getDouble(L"min-valid", 0.5);
    options.maxDiscontinuities = args.getInt(L"max-discontinuities", -1);
    options.settleSeconds = args.getDouble(L"settle", 2.0);
    options.maxReopens = args.getInt(L"max-reopens", 20);
    options.maxTransitionFrames = args.getInt(L"max-transition", 4800);

    if (args.has(L"expect-format")) {
        options.expectedRate = (unsigned long)args.getDouble(L"rate", 48000.0);
        options.expectedChannels = layout.channels;
    }

    return options;
}

// Writes default.json for the layout unless --keep-config, backing up an
// existing configuration the first time. An empty driverClsid leaves SAR
// without a hardware interface.
bool prepareConfig(const Args& args, const EndpointLayout& layout,
    const std::wstring& driverClsid = SAR_TEST_CLOCK_CLSID_STR)
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

    if (!writeDriverConfig(layout, driverClsid,
        args.getInt(L"wavert-min-frames", 0), path, args.has(L"app-routing"))) {
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
        args.getInt(L"wavert-min-frames", 0), out, args.has(L"app-routing"))) {
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

int cmdRace(const Args& args)
{
    EndpointLayout layout = EndpointLayout::fromArgs(args);
    int cycles = args.getInt(L"cycles", 10);
    double up = args.getDouble(L"up", 3.0);
    int down = args.getInt(L"down", 200);
    int threads = args.getInt(L"openers", 4);
    int hold = args.getInt(L"hold", 250);
    int hangTimeout = args.getInt(L"phase-timeout", 30);

    if (threads < 1) {
        threads = 1;
    }

    if (threads > 32) {
        threads = 32;
    }

    if (!prepareConfig(args, layout)) {
        return 1;
    }

    AsioHost host(hostOptions(args, layout));
    RaceOpeners openers(layout, threads, hold, hangTimeout);
    std::vector<std::string> cycleJson;
    int rc = 0;

    if (!host.load() || !host.open()) {
        rc = 1;
    }

    if (rc == 0) {
        logf("Starting %d stream openers on %d endpoint pairs", threads, layout.pairs);
        openers.start();
    }

    for (int i = 0; rc == 0 && i < cycles; ++i) {
        JsonObject cycle;

        logf("--- race cycle %d of %d ---", i + 1, cycles);
        cycle.setInt("cycle", i + 1);

        if (!host.start()) {
            rc = 2;
            cycle.setString("failure", "start");
            cycleJson.push_back(cycle.str());
            break;
        }

        cycle.setDouble("startMs", host.stats().lastStartMs);
        Sleep((DWORD)(up * 1000.0));

        if (!host.stop()) {
            rc = 2;
            cycle.setString("failure", "stop");
        }

        cycle.setDouble("stopMs", host.stats().lastStopMs)
            .setDouble("maxCallbackMs", host.stats().clock.maxCallbackMs)
            .setInt("slowCallbacks", (long long)host.stats().clock.slowCallbacks);
        cycleJson.push_back(cycle.str());
        Sleep((DWORD)down);
    }

    bool clean = openers.stop();
    RaceStats race = openers.stats();

    logf("Openers: %lld attempts, %lld streams opened, %lld without an active endpoint, "
        "slowest call %s (%.0f ms)", race.attempts, race.opened, race.noEndpoint,
        race.maxCallName.c_str(), race.maxCallMs);

    for (const auto& error : race.errors) {
        logf("  %s: %lld", error.first.c_str(), error.second);
    }

    if (!clean || race.hangsReported > 0 || host.stats().hangsReported > 0) {
        rc = 2;
        logf("A WASAPI or ASIO call hung");
    } else if (rc == 0 && race.opened == 0) {
        rc = 2;
        logf("No stream was ever opened; the openers never raced the host");
    }

    // Results first: closing the host can hang if the driver is wedged.
    JsonObject result;

    result.setString("command", "race")
        .setBool("passed", rc == 0)
        .setInt("exitCode", rc)
        .setInt("endpointPairs", layout.pairs)
        .setInt("channels", layout.channels)
        .setRaw("host", host.toJson())
        .setRaw("openers", race.toJson())
        .setRaw("cycles", jsonArray(cycleJson));
    writeResults(args, result.str());
    logf("%s", rc == 0 ? "PASSED" : "FAILED");
    host.close();
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

        iteration.setDouble("stopMs", host.stats().lastStopMs)
            .setDouble("maxCallbackMs", host.stats().clock.maxCallbackMs)
            .setInt("slowCallbacks", (long long)host.stats().clock.slowCallbacks);
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

// Control IDs in SarAsio's endpoints page (SarAsio.rc).
const int kConfigureHardwareInterfaceButton = 1002;

struct ControlPanelWindow
{
    DWORD pid = 0;
    HWND sheet = nullptr;
    HWND button = nullptr;
};

BOOL CALLBACK findConfigureButton(HWND hwnd, LPARAM lparam)
{
    auto *find = (ControlPanelWindow *)lparam;
    wchar_t cls[32] = {};

    GetClassNameW(hwnd, cls, 32);

    if (GetDlgCtrlID(hwnd) == kConfigureHardwareInterfaceButton &&
        _wcsicmp(cls, L"Button") == 0) {
        find->button = hwnd;
        return FALSE;
    }

    return TRUE;
}

BOOL CALLBACK findControlPanel(HWND hwnd, LPARAM lparam)
{
    auto *find = (ControlPanelWindow *)lparam;
    DWORD pid = 0;

    GetWindowThreadProcessId(hwnd, &pid);

    if (pid != find->pid || !IsWindowVisible(hwnd)) {
        return TRUE;
    }

    EnumChildWindows(hwnd, findConfigureButton, lparam);

    if (find->button) {
        find->sheet = hwnd;
        return FALSE;
    }

    return TRUE;
}

// Issue #133: with the host running, the hardware interface's Configure
// button in SAR's control panel created a second instance of the inner ASIO
// driver instead of using the running one. ASIO drivers may assume one
// instance per process; FlexASIO crashed on it.
int cmdControlPanel(const Args& args)
{
    EndpointLayout layout = EndpointLayout::fromArgs(args);

    if (!prepareConfig(args, layout)) {
        return 1;
    }

    AsioHost host(hostOptions(args, layout));

    if (!host.load() || !host.open()) {
        return 1;
    }

    if (!host.start()) {
        host.close();
        return 1;
    }

    bool found = false;
    bool enabled = false;
    bool clicked = false;
    std::thread clicker([&] {
        ControlPanelWindow window;

        window.pid = GetCurrentProcessId();

        for (int i = 0; i < 200 && !window.button; ++i) {
            Sleep(50);
            EnumWindows(findControlPanel, (LPARAM)&window);
        }

        if (!window.button) {
            logf("SAR's control panel did not appear within 10 s");
            return;
        }

        found = true;
        enabled = IsWindowEnabled(window.button) != FALSE;
        logf("Found the control panel; Configure is %s", enabled ? "enabled" : "disabled");

        DWORD_PTR ignored = 0;

        // What a click on the button sends its page. The handler runs on the
        // panel's thread and returns once the inner driver's panel does.
        clicked = SendMessageTimeoutW(GetParent(window.button), WM_COMMAND,
            MAKEWPARAM(kConfigureHardwareInterfaceButton, BN_CLICKED),
            (LPARAM)window.button, SMTO_NORMAL, 10000, &ignored) != 0;

        if (!clicked) {
            logf("The Configure handler did not return within 10 s");
        }

        PostMessageW(window.sheet, PSM_PRESSBUTTON, PSBTN_CANCEL, 0);
    });

    logf("Opening SAR's control panel");

    bool opened = host.controlPanel();

    clicker.join();
    host.readClockStats();

    SarTestClock::ClockStats clock = host.stats().clock;

    host.close();
    logf("Clock instances: %u alive, at most %u; inner controlPanel calls: %llu",
        clock.liveInstances, clock.maxLiveInstances,
        (unsigned long long)clock.controlPanelCalls);

    int rc = 0;
    std::string failure;

    if (!opened || !found || !enabled || !clicked) {
        rc = 2;
        failure = "could not drive the control panel";
    } else if (clock.controlPanelCalls == 0) {
        rc = 2;
        failure = "Configure did not open the clock's control panel";
    } else if (clock.maxLiveInstances > 1) {
        rc = 2;
        failure = "Configure created a second clock instance";
    }

    if (rc != 0) {
        logf("%s", failure.c_str());
    }

    JsonObject result;

    result.setString("command", "control-panel")
        .setBool("passed", rc == 0)
        .setInt("exitCode", rc)
        .setBool("panelFound", found)
        .setBool("configureEnabled", enabled)
        .setBool("configureClicked", clicked)
        .setInt("maxClockInstances", clock.maxLiveInstances)
        .setInt("clockControlPanelCalls", (long long)clock.controlPanelCalls)
        .setRaw("host", host.toJson());

    if (!failure.empty()) {
        result.setString("failure", failure);
    }

    writeResults(args, result.str());
    logf("%s", rc == 0 ? "PASSED" : "FAILED");
    return rc;
}

// Issues #31 and #54: before a hardware interface is chosen, SAR offered one
// mono input and one mono output, and DAWs that need a stereo output (Live,
// Reason) refused to load it, so its control panel couldn't be reached.
int cmdNoInterface(const Args& args)
{
    EndpointLayout layout = EndpointLayout::fromArgs(args);

    layout.pairs = 0;

    if (!prepareConfig(args, layout, L"")) {
        return 1;
    }

    AsioHost host(hostOptions(args, layout));
    int rc = 0;
    std::string failure;

    if (!host.load()) {
        return 1;
    }

    if (!host.open()) {
        rc = 2;
        failure = "open failed";
    } else if (!host.start() || !host.stop()) {
        rc = 2;
        failure = "start/stop failed";
    } else if (host.stats().physicalInputs < 2 || host.stats().physicalOutputs < 2) {
        rc = 2;
        failure = "no stereo pair without a hardware interface";
    }

    host.close();

    if (rc != 0) {
        logf("%s", failure.c_str());
    }

    JsonObject result;

    result.setString("command", "no-interface")
        .setBool("passed", rc == 0)
        .setInt("exitCode", rc)
        .setRaw("host", host.toJson());

    if (!failure.empty()) {
        result.setString("failure", failure);
    }

    writeResults(args, result.str());
    logf("%s", rc == 0 ? "PASSED" : "FAILED");
    return rc;
}

// Starts a host on the layout, records whether Windows shows the layout's
// endpoints (and, when stale is given, endpoints under those names instead)
// and stops it again.
bool showsEndpoints(const Args& args, const EndpointLayout& layout,
    const EndpointLayout *stale, bool *staleShown)
{
    if (!prepareConfig(args, layout)) {
        return false;
    }

    AsioHost host(hostOptions(args, layout));

    if (!host.load() || !host.open() || !host.start()) {
        host.close();
        return false;
    }

    FoundEndpoints found;
    bool shown = findEndpoints(layout, args.getInt(L"wait", 30), &found);

    found.release();

    if (stale) {
        *staleShown = findEndpoints(*stale, 5, &found);
        found.release();
    }

    host.close();

    if (!waitForEndpointsGone(layout, args.getInt(L"wait-gone", 30))) {
        logf("Endpoints lingered after stop");
    }

    return shown;
}

// Issues #15 and #66: SarAsio's control panel gave a new endpoint the lowest
// unused "ep_N", so a deleted endpoint's ID went to the next one, and Windows
// kept showing the old endpoint's name for it. Windows keys what it stores
// about an endpoint (its name, the user's settings) on the ID. Checks that a
// fresh ID gets the new name, and reports what a reused ID shows.
int cmdEndpointNames(const Args& args)
{
    EndpointLayout before = EndpointLayout::fromArgs(args);

    before.pairs = 1;
    before.prefix = L"SarTest Alpha";
    before.idPrefix = L"SarTest-names";

    EndpointLayout renamed = before;

    renamed.prefix = L"SarTest Beta";

    EndpointLayout fresh = renamed;

    fresh.idPrefix = L"SarTest-names-fresh";

    logf("--- %s, IDs %s ---", narrow(before.prefix).c_str(), narrow(before.idPrefix).c_str());

    bool firstShown = showsEndpoints(args, before, nullptr, nullptr);

    logf("--- renamed to %s, same IDs ---", narrow(renamed.prefix).c_str());

    bool oldNameShown = false;
    bool renamedShown = showsEndpoints(args, renamed, &before, &oldNameShown);

    logf("--- %s, new IDs %s ---", narrow(fresh.prefix).c_str(), narrow(fresh.idPrefix).c_str());

    bool freshShown = showsEndpoints(args, fresh, nullptr, nullptr);

    logf("Reused IDs: Windows shows the %s name", renamedShown ? "new" :
        oldNameShown ? "old" : "neither");

    int rc = 0;
    std::string failure;

    if (!firstShown) {
        rc = 2;
        failure = "endpoints never appeared";
    } else if (!freshShown) {
        rc = 2;
        failure = "endpoints with new IDs did not show their names";
    }

    if (rc != 0) {
        logf("%s", failure.c_str());
    }

    JsonObject result;

    result.setString("command", "endpoint-names")
        .setBool("passed", rc == 0)
        .setInt("exitCode", rc)
        .setBool("reusedIdShowsNewName", renamedShown)
        .setBool("reusedIdShowsOldName", oldNameShown)
        .setBool("newIdShowsNewName", freshShown);

    if (!failure.empty()) {
        result.setString("failure", failure);
    }

    writeResults(args, result.str());
    logf("%s", rc == 0 ? "PASSED" : "FAILED");
    return rc;
}

int cmdMeter(const Args& args)
{
    SessionMeterOptions options;

    options.durationSeconds = args.getDouble(L"duration", 10.0);
    options.process = args.get(L"process", L"");
    options.minPeak = args.getDouble(L"min-peak", 0.01);

    SessionMeterResult meter = runSessionMeter(options);
    int rc = meter.passed ? 0 : 2;
    JsonObject result;

    result.setString("command", "meter")
        .setBool("passed", meter.passed)
        .setInt("exitCode", rc)
        .setString("process", narrow(options.process))
        .setDouble("processPeak", meter.processPeak)
        .setRaw("sessions", meter.sessionsJson);

    if (!meter.failure.empty()) {
        result.setString("failure", meter.failure);
        logf("%s", meter.failure.c_str());
    }

    writeResults(args, result.str());
    logf("%s", rc == 0 ? "PASSED" : "FAILED");
    return rc;
}

int cmdWatch(const Args& args)
{
    WatchOptions options;

    options.durationSeconds = args.getDouble(L"duration", 60.0);
    options.stopFile = args.get(L"stop-file", L"");
    options.pollMs = args.getInt(L"poll", 20);
    options.probeMs = args.getInt(L"probe", 0);
    writeResults(args, runWatch(options));
    return 0;
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
    } else if (args.command == L"race") {
        rc = cmdRace(args);
    } else if (args.command == L"control-panel") {
        rc = cmdControlPanel(args);
    } else if (args.command == L"no-interface") {
        rc = cmdNoInterface(args);
    } else if (args.command == L"endpoint-names") {
        rc = cmdEndpointNames(args);
    } else if (args.command == L"meter") {
        rc = cmdMeter(args);
    } else if (args.command == L"watch") {
        rc = cmdWatch(args);
    } else {
        rc = usage();
    }

    CoUninitialize();
    return rc;
}
