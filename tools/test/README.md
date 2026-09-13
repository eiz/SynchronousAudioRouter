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
passes when it received the signal, saw no corruption, and at least half of
the frames after the initial silence were valid (`--min-valid`;
`--max-discontinuities` optionally bounds sequence gaps). Dropouts,
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
stop, or no stream was ever opened. `Invoke-SarTest.ps1 -Scenarios` selects
any of `matrix`, `race` and `kill`.

## Commands

```
SarTest install --inf <SynchronousAudioRouter.inf> [--endpoints N] [--channels C]
SarTest uninstall [--remove-device]
SarTest config [--endpoints N] [--channels C] [--out <path>]
SarTest host [--iterations K] [--duration S]
SarTest wasapi [--duration S] [--wait S] [--expect-invalidation]
SarTest run [--iterations K] [--duration S]
SarTest race [--cycles K] [--up S] [--down MS] [--openers T] [--hold MS]
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

The clock reads `SAR_TEST_CLOCK_RATE`, `SAR_TEST_CLOCK_FRAMES`,
`SAR_TEST_CLOCK_INPUTS` and `SAR_TEST_CLOCK_OUTPUTS` from the environment.
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

The script runs `run` for 1, 4, 8 and 16 endpoint pairs with three
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
