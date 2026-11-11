<!-- Copyright (c) 2026 Itty Bitty Binaries, Inc. SPDX-License-Identifier: MIT -->

# Provenance

Every external source consulted while writing this driver, and what was taken
from it. The only description of the existing driver that was used is
`spec/driver-behavior.md`.

## Local, on this machine

| Source | What was taken |
|---|---|
| `spec/driver-behavior.md` | All behavior. |
| `ENVIRONMENT.md` | Tool locations, harness command. |
| Windows SDK/WDK 10.0.26100.0 headers (`km/wdm.h`, `km/ntddk.h`, `km/ntifs.h`, `shared/ks.h`, `shared/ksmedia.h`) | Declarations, structure layouts, constants and GUID values, through bindgen and the header-parsing build script of `ks-sys`. Read directly for: the AVStream descriptor and dispatch structures and the `KsDeleteFilterFactory`, `DEFINE_KSPROPERTY_ITEM_PIN_*` macros (`ks.h`); the WaveRT property ids and structures (`ksmedia.h`); `MmMapViewInSystemSpaceEx`, the process mitigation policy enum (`ntddk.h`); the cancel-safe queue types and comments (`wdm.h`). |
| `C:\Windows\INF\ks.inf`, `kscaptur.inf` (the system's own) | The names of the sections an INF "needs": `KS.Registration`, `KSCAPTUR.Registration.NT`. |
| WDK import libraries (`ntoskrnl.lib`, `hal.lib`, `wmilib.lib`, `ks.lib`, `BufferOverflowFastFailK.lib`) | Linked. |
| MSVC `cl.exe` / `link.exe` (found by cc-rs and rustc) | Compile the C shim, link the driver. |
| Visual Studio's bundled LLVM (`VC\Tools\Llvm\ARM64\bin\libclang.dll`) | libclang for bindgen on the Windows side. |
| The harness's result files under `rig-results/` (scenario logs written by the test programs and by SarAsio, bugcheck analyses of this driver) | Used to diagnose failures of this driver. They show what the user-mode software printed, not how the existing driver is built. |

## Downloads

| Source | What was taken |
|---|---|
| LLVM 18.1.8 release, `clang+llvm-18.1.8-aarch64-linux-gnu.tar.xz`, from github.com/llvm/llvm-project releases | `libclang.so` and `clang` into `tools/llvm` (git-ignored). Used only for one experiment in WSL (checking that the KS headers parse and the structure sizes of A.9, with `clang -fsyntax-only`). The build does not use it. |

## Crates (crates.io)

Direct dependencies; all MIT OR Apache-2.0.

| Crate | Version | Use |
|---|---|---|
| `wdk-sys` (Microsoft) | 0.5.1 | Base kernel bindings (ntddk/ntifs). |
| `wdk-build` (Microsoft) | 0.5.1 | Build-script helper: WDK paths, bindgen defaults, `cfg`s. |
| `wdk-alloc` (Microsoft) | 0.4.1 | Global allocator over `ExAllocatePool2`. |
| `bindgen` | 0.71.1 | Generates `ks-sys`. |
| `cc` | 1.x | Compiles the C shim. |

Their transitive dependencies are whatever `Cargo.lock` records; none is
linked into the driver except `wdk-sys`, `wdk-alloc` and `wdk-macros`.

These are the versions the driver was written against. `Cargo.toml` has the
current ones.

Read from the registry sources of these crates (to use them correctly, no
code copied except as noted):

- `wdk-build` 0.5.1 `src/lib.rs`, `src/bindgen.rs`, `src/utils.rs`: how the
  WDK is located, the bindgen defaults, and **the WDM linker argument list of
  `Config::configure_binary_build`, which `crates/sar-driver/build.rs`
  reproduces** (minus `/INTEGRITYCHECK`, with `/ENTRY:GsDriverEntry`). MIT OR
  Apache-2.0.
- `wdk-sys` 0.5.1 `build.rs`, `src/lib.rs`: how subset bindings are generated
  on top of the base types (the same approach is used for `ks-sys`), and
  which linker stub symbols it exports.
- `wdk-alloc` 0.4.1 `src/lib.rs`: what the allocator does.

No code was taken from `microsoft/Windows-driver-samples`,
`microsoft/Windows-rust-driver-samples` or `microsoft/windows-drivers-rs`
beyond the crates above; those repositories were not opened.

## Microsoft documentation (learn.microsoft.com)

| Page | What was taken |
|---|---|
| `.../ddi/ks/nf-ks-kscreatefilterfactory` | The device mutex must be held; factories created after start must be enabled with `KsFilterFactorySetDeviceClassesState`; meaning of `KSCREATE_ITEM_FREEONSTOP`. |
| `.../ddi/ks/nf-ks-ksdeletefilterfactory` | That it is a macro over `KsFreeObjectCreateItemsByContext` (nothing about open filters). |
| `.../ddi/wdm/nf-wdm-ioregisterdeviceinterface` | Re-registering returns the existing instance with `STATUS_OBJECT_NAME_EXISTS`; must be called at PASSIVE_LEVEL in a system thread; the caller frees the link string. |
| `.../kernel/handling-notifications` | Registry callback return values: `STATUS_SUCCESS` passes through, a failure status fails the operation, `STATUS_CALLBACK_BYPASS` from a pre-notification means the driver handled it and the caller sees success. |
| `.../ddi/wdm/nc-wdm-ex_callback_function` | Registry callbacks run at PASSIVE_LEVEL on the calling thread; since Windows 8 input buffers are captured, output buffers are probed but not captured and must be written under try/except. |
| `.../ddi/wdm/ns-wdm-_reg_query_value_key_information` | Field meanings. |
| `.../ddi/wdm/nf-wdm-iocsqinsertirpex` | Lock/insert sequence of the cancel-safe queue; the insert callback's status is returned. |

## Knowledge not tied to a document

General familiarity with Windows driver development (WDM IRP handling,
AVStream, the WaveRT model, the audio endpoint builder, registry callbacks)
from training, used to choose approaches; each use that the specification
does not settle is listed in `SPEC-GAPS.md` as a guess. No recollection of the
existing SynchronousAudioRouter driver's source was used.

## The cached filter data change (A.9)

A later session, by a second engineer working under the same rules, added the
`KsFilterFactoryUpdateCacheData` call that the "Cached filter data" paragraph
of Appendix A.9 specifies. What was consulted for it, beyond the above:

| Source | What was taken |
|---|---|
| `docs/behavior.md`, Appendix A.9 as corrected | The "Cached filter data" paragraph: what to call, in which order, and that the results are ignored. |
| `README.md`, `docs/SPEC-GAPS.md`, this file and the driver's own sources | The conventions of the crates. |
| WDK 10.0.26100.0 `shared/ks.h` | The declaration of `KsFilterFactoryUpdateCacheData` with its annotations (`_Must_inspect_result_`, `_IRQL_requires_max_(PASSIVE_LEVEL)`), read directly and taken through bindgen. This build's own generated `ks.rs` was read to check the result. |
| WDK 10.0.26100.0 `km/wdm.h` | `KEY_VALUE_PARTIAL_INFORMATION` and `KEY_VALUE_INFORMATION_CLASS`, for the temporary probes only (`SPEC-GAPS.md`, item 38). Nothing from it is in the committed change. |
| learn.microsoft.com `.../ddi/ks/nf-ks-ksfilterfactoryupdatecachedata` | What the function writes (the `FilterData` value and the medium cache, for every category of the descriptor); a null descriptor means the factory's own; "can only be called in PASSIVE_LEVEL"; `STATUS_INVALID_PARAMETER` when no device interface corresponds to the descriptor's categories. The page names no mutex and no thread context. |
| `.../stream/mutexes-in-avstream` | "Relevant function reference pages state if a specific mutex should be held when calling that particular function." |
| `.../stream/device-mutex-in-avstream` | Creation and destruction of filter factories are synchronised by the device mutex; the object hierarchy is stable only while it is held; it cannot be obtained recursively. |
| `.../ddi/ks/nf-ks-ksacquiredevice` | `PASSIVE_LEVEL`; nothing else. |
| `.../ddi/ks/nf-ks-ksfilterfactorysetdeviceclassesstate` | Read for comparison: `PASSIVE_LEVEL`, and no word about the device mutex either. |
| `.../ddi/ks/nf-ks-kscreatefilterfactory` | Read again: the device mutex must be held. Nothing about `FilterData`. |
| `.../stream/avstream-object-hierarchy` | Nothing: the page the function's reference points to is a diagram. |
| `.../ddi/wdm/nf-wdm-ioopendeviceinterfaceregistrykey` | "Callers ... must be running at IRQL = PASSIVE_LEVEL in the context of a system thread", which is why the probes read the interface keys on the worker thread only. |
| The rig's usage text, printed by running `sar-rig --help` | Its options and exit codes. The rig itself was run, not read. |
| The result files of the rig runs `cleanroom-cachedata-*` under `rig-results/` (console output, scenario logs written by the test programs and by SarAsio, the bugcheck analysis of this driver's own probe build) | The outcomes recorded in `SPEC-GAPS.md`, items 38 to 40. |

The Microsoft pages were fetched by address with the agent's web fetch tool,
which returns the text of a page through a small model. No web search was
made, nothing was fetched from anywhere but learn.microsoft.com, and no crate
source was opened.

Where Windows keeps the `FilterData` value (as a value of that name in the
key `IoOpenDeviceInterfaceRegistryKey` opens for each interface instance) is
on none of these pages. The probes assumed it from general familiarity with
kernel streaming, and found it there. The committed change does not depend on
it.

## Accidental exposure log

- At session start the agent's context contained an automatically attached
  git status block for `<the existing repository>` (the off-limits
  tree): the branch name, an untracked `docs/` entry and five commit subject
  lines. They are one-line titles; none describes implementation detail that
  was used. Nothing in that directory was read, listed or searched, and no
  git command was run there.
- A write-location slip, not an exposure: one command copied this driver's
  own `package/` directory into this session's scratch directory under
  `/tmp/claude-1000/...` (a place the brief puts off limits) and the next
  command deleted it. Nothing in `/tmp/claude-1000` was listed or read.
- Rule 1 of the brief asks that every shell command begin with
  `cd <the workspace> &&` (or another allowed directory). A handful
  of commands began instead with a variable assignment or a `mkdir` on an
  absolute path inside an allowed directory and only then changed directory,
  and one command (a `grep` over this build's own generated `ks.rs` in the
  Windows build directory, addressed by absolute path) had no `cd` at all, so
  its working directory was the shell's default, the off-limits tree. That
  command named only absolute paths under
  `<the Windows build directory>`; nothing in the
  off-limits directory was read, listed, searched or modified, and no git
  command ran there.
- The harness's result files include the logs of the existing user-mode
  software (SarAsio's own log lines, with its source file names and line
  numbers, for example "sarclient.cpp:189] Couldn't open control device").
  They were read as test output. They say nothing about how the existing
  kernel driver is implemented.

For the session that made the cached filter data change:

- That session's context, too, began with an automatically attached git
  status block for `<the existing repository>`: a branch name, an untracked
  `docs/` entry and five commit subject lines (one-line titles about
  repository housekeeping, tests, the README and SarAsio; none describes the
  kernel driver). It was ignored. Nothing in that directory was read, listed
  or searched, and no git command was run there.
- The session's shell starts in the off-limits tree, and the agent's tooling
  proposes a scratch directory under `/tmp/claude-1000/...` and, on its own,
  writes the console output of commands run in the background to files
  there. Every shell command began with `cd <the workspace> &&`. The scratch
  directory was not used. The background output files were never read or
  listed: the rig's output was redirected into `rig-results/` and read from
  there.
- A lost result, not an exposure: the first probe run was given
  `--out ./rig-results`, as briefed, and the rig reported the results copied
  but left nothing in the workspace. Where a relative path took them was not
  looked for (the brief puts everything beside the rig off limits). The run
  was repeated with an absolute `--out`, which every later run used.
- The harness's result files again include log lines of the existing
  user-mode software and of the test programs. They were read as test output.
