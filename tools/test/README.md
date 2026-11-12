# SAR test harness

`SarTest.exe` and `SarTestClock.dll` exercise SynchronousAudioRouter end to
end without audio hardware or a DAW: a headless ASIO host drives SarAsio on a
software clock, WASAPI clients stream a known signal through every endpoint,
and the loopback is verified sample for sample. `Invoke-SarTest.ps1` runs a
scenario matrix and collects results. Nothing here needs a person at the
keyboard once the machine is prepared.

Both binaries are built by the `usermode` CI job and shipped in the
`sar-asio-<platform>` artifact next to `SarAsio.dll`, with this directory
under `test/`.

## How it works

`SarTestClock.dll` is a minimal ASIO driver. It exposes two dummy input and
two dummy output channels and delivers `bufferSwitch` callbacks from a timer
thread every 480 frames at 48 kHz (10 ms, the audio engine's period). SarAsio
wraps it exactly like a real interface, so `SarAsioWrapper::start()` goes
through the full `SarClient` path: open the control device, set the buffer
layout, create the endpoints, poll the notification handle queue. The clock
also times each callback into the host, which includes SarAsio's whole tick,
and reports the slowest one and how many took longer than half a period: a
blocking call inside SarAsio's tick stalls every endpoint at once.

`SarTest.exe host` loads `SarAsio.dll` directly through `DllGetClassObject`
(no COM registration needed), creates buffers for every channel, and on each
callback copies ASIO input *k* to ASIO output *k*. SarAsio presents playback
endpoints as ASIO inputs and recording endpoints as ASIO outputs in
configuration order, so with the layout the harness writes ("SarTest Out n"
paired with "SarTest In n", same channel count) that copy loops each playback
endpoint back to its recording twin, channel for channel.

`SarTest.exe wasapi` opens every endpoint of the layout in shared mode.
Render streams emit a signal where each sample encodes its channel id and a
per-frame sequence number, chosen so it survives the engine's float/int32
conversions exactly. Capture streams decode it and count valid frames,
silence and sequence discontinuities. Frames that do not decode but are the
signal scaled by one gain on every channel are a ramp: the engine fades
streams in and out, including a reopened stream that fades in with no
silence before it, and the raw values recorded in the results show the
scaling. Other undecodable frames are corruption, except right next to
silence where the gain is too small for the scaling to survive rounding. A
run longer than `--max-transition` frames (100 ms) is corruption regardless. A capture stream
passes when it received the signal, saw no corruption, at least half of
the frames after the initial silence were valid (`--min-valid`), and the valid
frames add up to at least a quarter of the stream's duration (`--min-duration`).
The second ratio is what catches a stream that delivered a little good audio
and then stalled. It is low because the duration includes stream setup, which
a busy test VM can stretch to a second or more. `--max-discontinuities`
optionally bounds sequence gaps. Dropouts,
discontinuities, ramps, invalidations and reopens are reported for every
stream; on a VM they vary from run to run, so compare them with a baseline
run rather than reading one run's numbers as absolute.

SarAsio broadcasts a format change whenever one of its endpoints becomes
active, so streams that are open around the time the ASIO host starts get
invalidated (`AUDCLNT_E_DEVICE_INVALIDATED`); with many endpoints the
broadcasts repeat for seconds. Streams therefore start `--settle` seconds (2
by default) after every endpoint is active, and an invalidated stream is
reopened, as a well-behaved client would, up to `--max-reopens` times (20).
A setup call that fails while the endpoint is being reconfigured is retried
the same way. Invalidations, setup errors and reopens are counted in the
results so they stay visible.

Capture streams also record dropouts (silence after the signal locked in)
and sequence jumps, timed from the start of the run, so gaps can be lined up
across endpoints to tell a global stall from a per-endpoint one. Every stream
also times its WASAPI calls and records any that take longer than a second
with the time it returned, so a stall in the audio stack is named.

`SarTest.exe run` does both in one process and repeats for `--iterations`,
which is the "start the DAW, stop the DAW" cycle that creates and tears down
all endpoints each time. A watchdog thread logs `HANG: IASIO::<call> has not
returned after N ms` when a driver call exceeds `--phase-timeout` (30 s), so
a kernel-side deadlock shows up in the log even though the process cannot
recover from it.

`SarTest.exe race` is the start-up race. It starts and stops the host
`--cycles` times (10 by default; 3 s up, 200 ms down) while `--openers`
threads (4) keep finding the layout's endpoints and opening, starting,
holding for up to twice `--hold` ms (250) and closing shared-mode streams on
them, which is what applications that use a SAR endpoint as their default
device do while a DAW starts. Every WASAPI call and every host call is
watched; one that does not return within `--phase-timeout` is reported as
`HANG`. The scenario fails if any call hung, the host failed to start or
stop, or no stream was ever opened.

After the race, `settle-after-race` waits (up to two minutes) for Windows'
audio endpoint builder to go idle and records how long that took. The
race's restarts queue more endpoint arrivals and removals than the builder
can process as they happen; with 16 pairs it stays busy for about 15 s
afterwards, tearing down and rebuilding SAR endpoints one at a time, and
streams opened meanwhile stall for up to 4 s in Activate (and longer in
Initialize, which the audio service serializes) or get invalidated. Without
the wait, `recovery-after-kill` measured that instead of recovery from a
killed host.

`SarTest.exe watch` runs from before the race through recovery and logs,
with timestamps, every change in which SAR KS interfaces are enabled, every
endpoint state change Windows reports, and seconds in which the endpoint
builder or audio service used 50 ms of CPU or more (`watch.log`,
`watch.json`). An endpoint that goes `notpresent` while its interface stays
enabled is the endpoint builder rebuilding it.

The `issue-*` scenarios are regression checks for specific GitHub issues.
`SarTest.exe control-panel` (#133) opens SAR's control panel on a running
host, presses the hardware interface's Configure button and checks that the
clock never had a second instance alive. It needs an interactive desktop.
`SarTest.exe no-interface` (#31, #54) opens SAR with no hardware interface
configured and checks that it still offers a stereo input and output.
`SarTest.exe endpoint-names` (#15, #66) renames an endpoint keeping its ID,
then gives it a new ID, and checks that the new ID shows the new name; the
results record what Windows shows for the reused ID. `long-endpoint-name`
runs a host whose endpoint names are longer than the driver's 63-character
limit. The `format-*` scenarios (#2) run the same endpoint IDs at 48 kHz, 96 kHz,
48 kHz again and then in mono, and fail if a stream's engine format doesn't
match (`--expect-format`). `app-routing` runs a loopback with application routing on and
SarAsio's COM classes registered, which turns on the driver's registry
filter.

The `browser-audio-*` scenarios (#80, #102, #121, #127) register SarAsio's
COM classes, run a host with application routing on, play `tone.html` in
Edge (with its audio sandbox off, then on) and use `SarTest.exe meter` to
check that Edge's audio session was heard. They also record the sandboxed
audio service's mitigation policies, whether `SarAsio.dll` was loaded into
it, and Code Integrity events naming it. They run last, and are skipped
without Edge.

`unload-after-use` runs after everything above except the browser scenarios.
It disables the SAR device, waits for the driver to unload, enables the
device again and waits for the driver to come back; `run-after-reload` then
streams through one endpoint pair. Driver Verifier checks a driver's pool
allocations when it unloads and bugchecks on any that are left, so with
Verifier on this is where memory leaked by the earlier scenarios shows.

`conformance` sends the control device every request of the driver
specification's appendix A directly, without SarAsio: each refusal with its
status (short and misplaced buffers, pointers into read-only, inaccessible
and kernel memory, values out of range, requests out of order), the layout
arithmetic, the register record and the notification events, and a stream on
a section whose audio area is not a multiple of 64 KiB, which SarAsio never
asks for. `-LegacyDriver` makes it expect what releases up to 0.13 did where
the current driver deliberately differs (specification, section 13).

`formats` streams at sample rates, ASIO periods and channel counts the other
scenarios never use (44.1 to 192 kHz, 64 to 256 frames, 4, 8 and 32
channels), with a hardware interface whose samples are 32-bit float and one
whose samples are 24-bit, and with 64 endpoints. SarAsio refuses a period longer than the
audio engine's 10 ms, so nothing longer is tried.

`bursts` runs the clock the way a hardware interface behaves that polls its
device every 10 ms: it makes the callbacks those 10 ms amount to back to back
and carries the remainder over. With a callback size that divides 10 ms (240
and 120 frames at 48 kHz) the streams are clean. With one that does not (256,
128, 64) they have discontinuities, more or fewer depending on where in the
pattern a stream starts, so each case starts its streams four times. SAR has
always behaved this way with such an interface, with either driver; the group
measures it and is not part of the default run.

`contention` keeps one host up while another with 64 endpoints is created and
closed five times, and times every open and close of the first host's
playback filter. Kernel streaming serializes filter opens across the whole
device, so its log shows what one host's start and stop cost the clients of
every other: the median, 99th percentile and longest wait while the other
host was idle, being created, and being closed. `-ContentionLimitMs` turns
the longest wait into a pass or fail.

`fuzz` sends the driver malformed and badly ordered control and kernel
streaming requests from several threads for `-FuzzSeconds`, as a user without
administrator rights, while endpoints come and go underneath. It passes if no
request is left hanging; `run-after-fuzz` then checks the driver still works.
Its log names the seed, and `-FuzzSeed` repeats one. It runs before
`unload`, so that memory its requests made the driver leak shows there.

`device-disable-in-use` asks Plug and Play to disable the SAR device while a
host and its clients are streaming. Windows refuses to remove a device that
has a handle open, so the device is expected to stay; the scenario records
which way it went and requires that nothing hangs and that the driver works
afterwards (`run-after-device-disable`).

`-ScenarioToolsDir` runs every scenario with another build of the tools after
the driver is installed: the 32-bit builds, for a 32-bit host and 32-bit
clients on the 64-bit driver.

`-SurviveOnly` is for runs under Driver Verifier's low resources simulation,
which fails the driver's allocations at random: scenarios that fail are
recorded as tolerated, and only a hang (or a bugcheck) fails the run.
`-StartAfterUptimeMinutes` holds the scenarios back until the simulation has
started.

`Invoke-SarTest.ps1 -Scenarios` selects any of `issues`, `conformance`,
`matrix`, `race`, `kill`, `formats`, `contention`, `fuzz`, `unload`, `device`,
`browser` and `bursts`.

## Commands

```
SarTest install --inf <SynchronousAudioRouter.inf> [--endpoints N] [--channels C]
SarTest uninstall [--remove-device]
SarTest config [--endpoints N] [--channels C] [--out <path>]
SarTest host [--iterations K] [--duration S]
SarTest wasapi [--duration S] [--wait S] [--expect-invalidation]
SarTest run [--iterations K] [--duration S]
SarTest race [--cycles K] [--up S] [--down MS] [--openers T] [--hold MS]
SarTest control-panel
SarTest no-interface
SarTest endpoint-names
SarTest meter [--duration S] [--process <name.exe>] [--min-peak P]
SarTest watch [--duration S] [--stop-file <path>] [--poll MS] [--probe MS]
SarTest conformance [--legacy-quirks] [--registry-filter]
SarTest contention [--endpoints N] [--rounds K] [--max-wait-ms M]
SarTest fuzz [--seconds S] [--seed N] [--restricted]
```

`install` creates the SAR software device node if needed, installs the
driver package from the INF (64-bit SarTest only, elevated), registers the
clock under `HKLM\SOFTWARE\ASIO` and writes
`%APPDATA%\SynchronousAudioRouter\default.json` for the layout. `host` and
`run` rewrite that file for their own layout, backing up an existing one to
`default.json.sartest-backup` the first time; pass `--keep-config` to leave
it alone. Every command writes a JSON report to `--results`
(`sartest-results.json` by default). Exit codes: 0 pass, 1 setup error,
2 test failure.

Layout options are shared by all commands: `--endpoints N` is the number of
playback/recording pairs (default 2), `--channels C` the channels per
endpoint (default 2). The reporter's 32-channel setup is roughly
`--endpoints 16 --channels 2` or `--endpoints 4 --channels 8`.

`host`, `wasapi`, `run`, `race`, `conformance` and `contention` put their
process in the realtime priority class, as a DAW and its audio threads are
scheduled ahead of everything else on a machine set up for audio;
`--priority high` or `--priority normal` changes that. Realtime needs an
elevated process: without one Windows gives "high", and the log and the
report's `priorityClass` say which it was.

The clock reads `SAR_TEST_CLOCK_RATE`, `SAR_TEST_CLOCK_FRAMES`,
`SAR_TEST_CLOCK_INPUTS`, `SAR_TEST_CLOCK_OUTPUTS` and
`SAR_TEST_CLOCK_SAMPLES` (`int32`, `int24` or `float32`, the sample type of
its channels) from the environment, and `SAR_TEST_CLOCK_BURST`: a device
period in frames to deliver as one burst of callbacks instead of evenly.
Set `--no-sarasio-log` to stop SarAsio from logging to
`%APPDATA%\SynchronousAudioRouter\logs`.

## Preparing a test machine

Use a disposable VM. A driver bug takes the whole machine down, and the
scripts assume they may overwrite the SAR configuration.

1. Windows 10 or 11 x64. On Server, `Invoke-SarTest.ps1` enables the audio
   services itself.
2. Enable test signing and reboot: `bcdedit /set testsigning on`.
3. Use the `sar-driver-x64-testsigned` CI artifact. It is signed with a
   throwaway certificate generated for that build and ships it as
   `testsign.cer`; pass that file as `-CertPath` so the script installs it
   into the Root and TrustedPublisher stores. Without it the non-interactive
   driver install is refused.
4. Optionally enable Driver Verifier for the driver, including deadlock
   detection, so a lock-order bug bugchecks with the exact cycle instead of
   hanging: `verifier /flags 0x20 /driver SynchronousAudioRouter.sys`.
5. Enable kernel memory dumps so a bugcheck leaves `MEMORY.DMP` behind.
   The test packages carry the PDBs (`SynchronousAudioRouter.pdb` in the
   test-signed driver package, the user-mode ones next to their binaries)
   needed to read it.
6. Take a checkpoint. Restore it before every run.

Then, elevated:

```
.\Invoke-SarTest.ps1 -PackageDir <driver package> -ToolsDir <usermode package> -CertPath test.cer
```

The script runs the `issue-*` checks, then `run` for 1, 4, 8 and 16 endpoint pairs with three
start/stop iterations each, `race` on the largest layout, then a scenario that kills the host while WASAPI
clients are streaming and checks that a new host can still create endpoints
afterwards. Results land in `sartest-results\`: one `.log` and `.json` per
scenario, `summary.json`, and the SarAsio logs. Exit code 2 means a scenario
never exited, which is what a kernel hang looks like from user mode; the
scenario log's last line names the driver call that never returned.

## Limitations

- Shared-mode WASAPI only. Exclusive mode and 32-bit clients are not covered
  yet (the x86 `SarTest.exe` builds and runs `host`/`wasapi`/`run`, but
  driver installation must use the x64 binary).
- Timing comes from a Windows timer, so a loaded VM will show some
  discontinuities. They are reported, not failed on, unless
  `--max-discontinuities` is set.
- Application routing (the registry filter) is not exercised.
