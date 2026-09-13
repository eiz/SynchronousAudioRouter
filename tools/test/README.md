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
layout, create the endpoints, poll the notification handle queue.

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
silence, sequence discontinuities and frames carrying the wrong channel id.
A capture stream passes when it received the signal, never saw a wrong
channel, and at least 90% of the frames after the initial silence were
contiguous (`--min-valid`, `--max-discontinuities` tune this).

`SarTest.exe run` does both in one process and repeats for `--iterations`,
which is the "start the DAW, stop the DAW" cycle that creates and tears down
all endpoints each time. A watchdog thread logs `HANG: IASIO::<call> has not
returned after N ms` when a driver call exceeds `--phase-timeout` (30 s), so
a kernel-side deadlock shows up in the log even though the process cannot
recover from it.

## Commands

```
SarTest install --inf <SynchronousAudioRouter.inf> [--endpoints N] [--channels C]
SarTest uninstall [--remove-device]
SarTest config [--endpoints N] [--channels C] [--out <path>]
SarTest host [--iterations K] [--duration S]
SarTest wasapi [--duration S] [--wait S] [--expect-invalidation]
SarTest run [--iterations K] [--duration S]
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
3. Test-sign the CI package's `.sys` and `.cat` with a self-signed
   certificate, and pass that certificate's `.cer` as `-CertPath` so the
   script installs it into the Root and TrustedPublisher stores. Without it
   the non-interactive driver install is refused.
4. Optionally enable Driver Verifier for the driver, including deadlock
   detection, so a lock-order bug bugchecks with the exact cycle instead of
   hanging: `verifier /flags 0x20 /driver SynchronousAudioRouter.sys`.
5. Enable kernel memory dumps so a bugcheck leaves `MEMORY.DMP` behind.
6. Take a checkpoint. Restore it before every run.

Then, elevated:

```
.\Invoke-SarTest.ps1 -PackageDir <driver package> -ToolsDir <usermode package> -CertPath test.cer
```

The script runs `run` for 1, 4, 8 and 16 endpoint pairs with three
start/stop iterations each, then a scenario that kills the host while WASAPI
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
