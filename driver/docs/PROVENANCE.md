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
