<!-- Copyright (c) 2026 Itty Bitty Binaries, Inc. SPDX-License-Identifier: MIT -->

# Provenance

Every external source consulted while writing this driver, and what was taken
from it. The only description of the existing driver that was used is
`docs/behavior.md`.

## Local, on this machine

| Source | What was taken |
|---|---|
| `docs/behavior.md` | All behavior. |
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

## Releasing endpoints and sections (section 11)

The same engineer, in the same session, then made the driver release what it
allocates (section 11, "What is released, and when"; `SPEC-GAPS.md`, section
J). What was consulted for that, beyond the above:

| Source | What was taken |
|---|---|
| `docs/behavior.md`, section 11 as extended ("What is released, and when", "The driver unloads") | The three rules. |
| The brief for the task | That the existing driver passes the same run on the same VM, so the rules can be met with kernel streaming on this version of Windows. Nothing about how. |
| `rig-results/unload-evidence/` (a run of this driver's baseline with the new `unload` scenarios, made by the engineer who briefed the work: Driver Verifier's stop for pool left allocated, the debugger's `!verifier 3` listing of this driver's allocations, a summary by calling function, the run's log) | That every block, endpoint and section was still allocated at unload, and how many. That Driver Verifier's special pool is on for this driver in the rig and took every allocation. |
| learn.microsoft.com `.../stream/object-bags` | An item placed in more than one bag is not released until it is in none; a bag is emptied when its object is closed or deleted; a dynamically allocated descriptor belongs in the bag of the object it describes. |
| `.../ddi/ks/nf-ks-ksadditemtoobjectbag` | The free routine is called when the item is removed or the bag deleted. The mutex of the bag must be held: the device mutex for a device's or a filter factory's bag, the filter control mutex for a filter's or a pin's. |
| `.../ddi/ks/nf-ks-ksremoveitemfromobjectbag` | An item has one count of references across bags, and is freed only when it leaves the last. Read for that; the driver does not call the function. |
| `.../ddi/ks/nf-ks-kscopyobjectbagitems` | Nothing used. |
| `.../stream/filter-control-mutex-in-avstream` | Kernel streaming holds the filter control mutex while it calls a filter's or a pin's Create and Close dispatch; creation and destruction of filters and pins are synchronised by it. |
| `.../ddi/ks/nf-ks-ksdeletefilterfactory` | Read again: "deletes a given filter factory"; a macro over `KsFreeObjectCreateItemsByContext`. Nothing about open filters or about when the factory object goes. |
| `.../ddi/ks/nf-ks-ksfreeobjectcreateitemsbycontext` | Frees every create item with the given context. Nothing about the object behind them. |
| `.../ddi/ks/ns-ks-_ksfilterfactory`, `.../ns-ks-_ksfilter`, `.../ns-ks-_kspin` | What is in an object's bag is cleaned up when the factory is deleted, the filter is deleted, the pin is closed. A new filter's context is its factory's, a new pin's its filter's. |
| `.../ddi/ks/nf-ks-_ksedit` | `KsEdit` puts a dynamically allocated copy of an object's descriptor in that object's bag: Microsoft's own example of a descriptor that is freed when the bag is emptied. The driver does not call it. |
| The result files of the rig runs `cleanroom-unload-*` under `rig-results/` (console output, scenario logs, the bugcheck analyses of this driver's own probe builds) | The measurements recorded in `SPEC-GAPS.md`, items 41 to 45. |

The pages were fetched as before: by address, from learn.microsoft.com only,
with no web search. No crate source was opened. No header was read for this
change.

## Requests of 32-bit clients (A.8)

The same engineer, in the same session, then changed how a client's event
handle is looked up and went through what else a request carries
(`SPEC-GAPS.md`, section K). What was consulted for that, beyond the above:

| Source | What was taken |
|---|---|
| `docs/behavior.md`, A.8 as extended ("Whose handle it is" and the closing paragraph) | The requirement. |
| The brief for the task | That requests from 32-bit processes reach the driver through `ksthunk.sys` marked as kernel-mode requests (also in A.8 now); how to run the rig with the 32-bit tools. |
| `rig-results/thunk-evidence/` (a run of this driver with the 32-bit test tools, made by the engineer who briefed the work: the bugcheck analysis, the conformance test's log up to the stop) | Where the driver stopped the machine and with which handle; the addresses a 32-bit host is given for its section. |
| WDK 10.0.26100.0 `shared/ksmedia.h` | The WaveRT structures and their 32-bit variants (`KSRTAUDIO_BUFFER32`, `KSRTAUDIO_HWREGISTER32`, `KSRTAUDIO_NOTIFICATION_EVENT_PROPERTY32` and the request structures), to see which fields differ in size. |
| This build's generated `wdk-sys` bindings (in the Windows build directory) | The declarations of `IoIs32bitProcess`, `ZwCreateEvent`, `PsGetCurrentProcessId`, `PsSetLoadImageNotifyRoutine` and `KeQueryPerformanceCounter`, for the temporary probes only. |
| learn.microsoft.com `.../ddi/wdm/nf-wdm-obreferenceobjectbyhandle` | "Drivers should always specify UserMode for handles they receive from user address space"; with user access the requested access is compared with the granted one; Driver Verifier's stop 0xC4, 0xF6 for a user handle looked up with kernel access. |
| `.../audio/ksproperty-rtaudio-register-notification-event`, `.../audio/ksproperty-rtaudio-buffer` | The request carries "a user-mode event handle"; the client should ask for a null base address and is given "the virtual address of the allocated audio buffer". Nothing about 32-bit clients. |
| `.../ddi/ksmedia/ns-ksmedia-ksrtaudio_buffer32`, `.../ns-ksmedia-ksrtaudio_notification_event_property32` | The 32-bit structures are documented by their members' names only. |
| `.../kernel/supporting-32-bit-i-o-in-your-64-bit-driver`, `.../ddi/wdm/nf-wdm-iois32bitprocess` | The contents of device control buffers are not converted for a 32-bit caller, which is why something has to; `IoIs32bitProcess` only answers for the request the I/O manager issued for a user-mode process. |
| `.../ddi/wdm/nf-wdm-zwmapviewofsection` | With a null base address the system chooses where the view goes. |
| learn.microsoft.com (Win32) `.../winprog64/wow64-implementation-details`, `.../winprog64/performance-and-memory-consumption` | In a 32-bit process "all user-mode address space above the 32-bit limit is reserved by the system"; the limit is 2 GB, or 4 GB for a large-address-aware image. |
| `.../winprog64/registry-redirector`, `.../winprog64/shared-registry-keys` | Where a 32-bit process's view of the class registrations is kept (`...\SOFTWARE\Classes\Wow6432Node\CLSID`), read to judge the `app-routing` failure of the 32-bit tools. |
| `.../ddi/ks/nc-ks-pfnkshandler`, `.../nf-ks-kspropertyhandler` | A property handler is given "an aligned copy" of the request and of the value. |
| `.../ddi/ks/nc-ks-pfnksintersecthandlerex`, `.../nf-ks-kspindataintersectionex` | The arguments of the intersection handler. Nothing about what has been validated. |
| `.../ddi/ks/nf-ks-ksvalidateconnectrequest`, `.../nc-ks-pfnkspinirp` | A connection request's "basic data format" is validated; "the buffer is passed a copy of the original input buffer, if the originator was not in kernel mode". |
| The result files of the rig runs `cleanroom-thunk-*` under `rig-results/` (console output, scenario logs, the bugcheck analyses of this driver's own probe builds) | The measurements recorded in `SPEC-GAPS.md`, items 47 to 54. |

Two web searches were made for this work, both restricted to
learn.microsoft.com, to find any documentation of `ksthunk.sys`. They found
none. The result lists named some question-and-answer threads hosted there;
none was opened. Every page above was fetched by its address.

## Holding each other up, and locked memory (sections 6 and 14)

The same engineer, in the same session, then made the driver stop holding
locks across slow calls and lock its section in physical memory
(`SPEC-GAPS.md`, section L). What was consulted for that, beyond the above:

| Source | What was taken |
|---|---|
| `docs/behavior.md`, section 14, "It stays in physical memory" in section 6, and the last entry of section 13 | The rules. |
| The brief for the task | Numbers, and nothing else, about the existing driver: how long the `contention` scenario's probe waited with it on the two test machines (while a second host closes, never more than 0.3 ms and 1.7 ms; while one is created, a median open of 7 and 12 ms, a longest of 62 and 85 ms, and a median close of 0.2 and 0.5 ms). They were used as a target, and as evidence that kernel streaming can be driven without the device mutex being held while device interfaces are switched off. How the existing driver arranges that was not said and is not known. Also the numbers of this driver before the change, which the runs below reproduce. |
| learn.microsoft.com `.../ddi/wdm/nf-wdm-mmprobeandlockpages` | Makes the pages of an MDL resident and locks them in memory until `MmUnlockPages`; reports failure by raising an exception, so the call has to be made under an exception handler; pageable addresses need IRQL at or below `APC_LEVEL`. |
| `.../ddi/wdm/nf-wdm-mmunlockpages`, `.../ddi/wdm/nf-wdm-ioallocatemdl` | Unlocking; allocating an MDL for a range of virtual addresses and freeing it with `IoFreeMdl`. |
| `.../ddi/wdm/nf-wdm-mmmaplockedpagesspecifycache` | Maps the pages of a locked MDL into system address space; with kernel access and no bugcheck on failure it returns null when it cannot; `MdlMappingNoExecute`; undone with `MmUnmapLockedPages`. |
| `.../ddi/ks/nf-ks-ksfilterfactorygetsymboliclink` | `PASSIVE_LEVEL`; names no mutex. |
| `.../ddi/ks/nf-ks-ksfilterfactorysetdeviceclassesstate`, `.../nf-ks-ksfilterfactoryupdatecachedata`, `.../nf-ks-kscreatefilterfactory`, `.../nf-ks-ksdeletefilterfactory`, `.../stream/mutexes-in-avstream`, `.../stream/device-mutex-in-avstream` | As recorded above, read again from these notes for the question which calls need the device mutex: only the page of `KsCreateFilterFactory` asks for it, the device mutex synchronises the creation and destruction of filter factories, and a function's page says so if a mutex has to be held. |
| `.../kernel/run-down-protection`, `.../ddi/wdm/nf-wdm-exacquirerundownprotection`, `.../nf-wdm-exwaitforrundownprotectionrelease`, `.../nf-wdm-exrundowncompleted`, `.../nf-wdm-exreinitializerundownprotection` | How run-down protection is used: acquiring fails once the wait for release has been called; the wait returns when everybody has released; waiting on an object that is already run down does nothing; a structure is re-initialised after the wait (and, optionally, `ExRundownCompleted`); the holder should be in a critical region so that it cannot be suspended; the IRQL each routine allows. |
| `.../ddi/ks/nc-ks-pfnksdeviceirpvoid`, `.../ddi/ks/ns-ks-_ksdevice_dispatch` | Which Plug and Play requests the device callbacks stand for. Nothing about the device mutex in them, and nothing about when factories are freed relative to them. |
| This build's generated `wdk-sys` bindings (in the Windows build directory) | The declarations and structure layouts of the routines above, of `KeEnterCriticalRegion`, `KfRaiseIrql`, `KeLowerIrql`, `KeGetCurrentIrql` and of the fast mutex (the last four for the temporary probes only). |
| WDK 10.0.26100.0 `km/wdm.h` | What the inline `ExInitializeFastMutex` stores (`FM_LOCK_BIT` in `Count`, no owner, no contention, a synchronisation event that is not signalled), which the lock wrapper reproduces (`SPEC-GAPS.md`, item 66). |
| The coordinator's messages during the task | The project owner's decision that the driver's own locks are fast mutexes, and the paragraph of section 14 that says so, which was put into `docs/behavior.md` on the coordinator's behalf. |
| The result files of the rig runs `cleanroom-rt-*` under `rig-results/` (console output, scenario logs, the text file this driver's own probe builds wrote into the results directory) | The measurements recorded in `SPEC-GAPS.md`, section L. |

The pages were fetched as before: by address, from learn.microsoft.com only,
with no web search. No crate source was opened.

## Files kernel streaming must not be given (section 5)

Still the same engineer and session. Consulted for refusing opens relative to
a control handle (`SPEC-GAPS.md`, section M), beyond the above:

| Source | What was taken |
|---|---|
| `docs/behavior.md`, section 5, last paragraph of "Opening and access" | The requirement and the status. |
| The brief for the task | Where the defect is (the dispatch layer decides by the file's name alone), and that the conformance test has cases for it. The evidence directory `rig-results/relopen-evidence/` was not opened. |
| learn.microsoft.com `.../ddi/ks/ns-ks-kspin_connect`, `.../ddi/ks/nf-ks-ksvalidateconnectrequest` (again), `.../stream/ksproperty-stream-masterclock` | What a pin creation's `PinToHandle` is; that a connection request's communication requirements are validated; that a pin given a master clock handle references the file object behind it. |
| The result files of rig run `cleanroom-rt-rel1-20261113-221937` | The statuses in item 69. |

## A control handle's `FsContext` stays empty (section 5)

A later session, by an engineer working under the same rules who had not
worked on the driver before, made the dispatch layer keep a control handle's
state outside its file object (`SPEC-GAPS.md`, item 71). What was consulted
for that, beyond the above:

| Source | What was taken |
|---|---|
| `docs/behavior.md`, the last paragraph of "Opening and access" in section 5; sections 3, 4, 11 and 14 | The requirement; the rules for the driver's locks; that the driver is not unloaded while a handle is open. |
| `README.md`, `docs/SPEC-GAPS.md` (sections L and M), this file and the driver's own sources | The conventions of the crates, and the precedent for reserving room in a vector under a fast mutex. |
| The brief for the task | Where `FsContext` was written (the dispatch hook); that the lookup runs on every control request, the host's real-time thread's included, and that creates and closes of different control handles can run concurrently; that the conformance test has cases for a control handle given to kernel streaming. |
| WDK 10.0.26100.0 `shared/ks.h` | Searched for `FsContext` (it does not occur) and for the routines that take a file object (`KsGetObjectFromFileObject` and its relatives, `KsDispatchIrp`, `KsAllocateObjectHeader`): declarations only. The names of the headers under `km/` and `shared/` that mention `FsContext` were listed; none of them was opened. |
| learn.microsoft.com `.../ddi/wdm/ns-wdm-_file_object` | `FsContext` and `FsContext2` are for "whatever optional state a driver maintains about the file object; otherwise, NULL"; in a WDM stack only the functional device object uses them; `RelatedFileObject` is valid only while the create request is processed. |
| `.../kernel/irp-mj-close` | Close is sent when the last handle has been closed and "all outstanding I/O requests have been completed or canceled"; a driver undoes on close what it did on create; close "will always be sent at PASSIVE_LEVEL". |
| `.../ddi/ks/nf-ks-ksdispatchirp`, `.../ddi/ks/nf-ks-kssetmajorfunctionhandler`, `.../ddi/ks/nf-ks-ksallocateobjectheader`, `.../ddi/ks/nf-ks-ksgetobjectfromfileobject` | Where kernel streaming keeps its dispatch table (the first element of the structure `FsContext` points to); that its dispatching assumes that structure was set up by `KsAllocateObjectHeader` and, for a create, looks at the parent's; that `KsGetObjectFromFileObject` does not check the file and may cause a memory access error. Quoted in item 71. |
| The result files of rig run `cleanroom-cleanroom-fsctx1-20261113-224835` under `rig-results/` (console output, the run's summary, the conformance test's log) | The outcome recorded in item 71. |

The pages were fetched by address with the agent's web fetch tool, from
learn.microsoft.com only, with no web search. No crate source was opened.
`rustfmt` and `clippy` (the pinned toolchain's) were run on the changed
crates; their only finding outside this change, in `section.rs`, was left
alone.

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
- Nothing new for the release work, which was the same session continued.
  Its evidence directory, `rig-results/unload-evidence/`, holds output of
  this driver only (see the table above). Every shell command again began
  with `cd <the workspace> &&`, every rig run was given an absolute `--out`,
  and the background output files under `/tmp/claude-1000/...` were again
  left unread.
- For the work on 32-bit clients, still the same session: the result files
  include log files written by SarAsio itself. One of them was opened, for
  the host of the `app-routing` scenario, to see which processes had
  written one; its lines carry the names of source files and functions of
  SarAsio's user-mode ASIO wrapper. They were read as test output and say
  nothing about the kernel driver. Shell commands, `--out` and the
  background output files were handled as before.
- For the work on section 14 and locked memory, still the same session
  (continued from a summary after it ran out of room): the context again
  held an automatically attached git status block for `<the existing
  repository>` (a branch name, an untracked `docs/` entry and five commit
  subject lines about repository housekeeping, tests, the README and
  SarAsio). It was ignored. The summary also named the path of the session's
  own transcript under `<the agent's configuration directory>`, which the
  brief puts off limits; it was not opened. Shell commands, `--out` and the
  background output files under `/tmp/claude-1000/...` were handled as
  before: every command began with `cd <the workspace> &&`, and the
  notifications that name those output files were not followed.
- A write-location slip in the same work, not an exposure: one command
  redirected a `git diff` of this workspace into a file directly under
  `/tmp/claude-1000`, and the next command deleted that file by name.
  Nothing there was listed or read.

For the session that kept `FsContext` empty:

- Its context, too, began with an automatically attached git status block
  for `<the existing repository>`: a branch name, an untracked `docs/` entry
  and five commit subject lines (one-line titles about code owners and a
  security policy, tests, the README and SarAsio's control panel
  configuration; none describes the kernel driver). It was ignored. Nothing
  in that directory was read, listed or searched, and no git command was run
  there.
- The session's tooling again named the off-limits tree as its working
  directory and proposed a scratch directory under `/tmp/claude-1000/...`.
  Every shell command began with `cd <the workspace> &&`, files were read
  and edited by absolute paths in the workspace, and the scratch directory
  was not used.
- The workspace holds a git stash left by an earlier, unfinished attempt at
  the same change. One `git stash list` showed its one-line title; its
  contents were not shown, applied or otherwise inspected.
