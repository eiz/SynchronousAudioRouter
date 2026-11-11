<!-- Copyright (c) 2026 Itty Bitty Binaries, Inc. SPDX-License-Identifier: MIT -->

# Specification gaps

Questions `docs/behavior.md` did not answer, answered ambiguously, or
answered wrongly, and how each was resolved. "Source" says where the answer
came from: Microsoft documentation, a WDK header, reasoning, or the rig.

Status markers: **[rig]** confirmed by a rig run, **[doc]** taken from
Microsoft documentation or a WDK header, **[guess]** reasoned but not
independently confirmed.

## A. Things the specification gets wrong or states imprecisely

1. **[A.6, section 6] "A client's view of the register table is one view of
   C bytes at section offset registerBase."** This cannot be done in general.
   A view of a section must start on a 64 KiB boundary (the allocation
   granularity), while `registerBase` is only page-aligned (`bufferSize`
   rounded up to 4,096). Whenever the audio area is not a multiple of 65,536
   the view has to start at the boundary below `registerBase`, be
   `registerBase mod 65536` bytes longer, and the register table sits at that
   offset inside it. Resolved: the client view (and the driver's own kernel
   view) starts at `registerBase` rounded down to 64 KiB and is
   `(registerBase mod 65536) + 65536` bytes long; the position register
   address reported to clients includes the delta. A side effect the
   specification does not mention: the client then also has the unusable
   tail of the audio area mapped (never any stream's cells). Source:
   `ZwMapViewOfSection` documentation (SectionOffset is rounded down to the
   allocation granularity). [doc]

2. **[A.9] `KSPIN_PHYSICALCONNECTION` sizes.** "`Size` = 12 + path bytes ...
   Reports 12 + path bytes + 2." The structure's string starts at offset 8,
   so the bytes actually meaningful are 8 + path + 2; `Size` and the reported
   length both overstate by 4. Implemented literally as written (Size = 12 +
   path bytes, reply length 12 + path bytes + 2, the bytes after the
   terminator zeroed). Whether the 4 trailing bytes are zero or leftover in
   the existing driver is not stated. [guess]

3. **[section 3] "opts in to non-executable pool memory."** Not expressible
   as described for a Rust driver (the opt-in is a C macro/inline,
   `ExInitializeDriverRuntime`). Every allocation this driver makes uses
   `ExAllocatePool2`, which is non-executable by definition; allocations
   kernel streaming makes on the driver's behalf are outside its control.
   [doc]

4. **[A.5] Order of checks for "set buffer layout".** The table puts "the
   section cannot be created" before "handle closed" and "layout already
   set", i.e. the existing driver creates a section it then throws away.
   This implementation checks closed/already-set first and never creates the
   doomed section. Only a doubly-failing request can tell the difference
   (which of two error codes it gets).

## B. Kernel streaming behavior the specification relies on but does not state

5. **[section 11] What happens to a filter factory that is deleted while a
   filter created from it is still open?** The specification says streams on
   a withdrawn endpoint keep working, "its filters cannot be opened again",
   and "the memory and the endpoint are released when the last such reference
   goes", but not which kernel streaming call achieves that. Microsoft's
   documentation of `KsDeleteFilterFactory` says nothing about open filters.
   Resolved: on withdrawal the driver switches the interfaces off and calls
   `KsDeleteFilterFactory` for both factories at once (so the same reference
   string can be created again immediately, which a host that restarts
   needs). The endpoint's memory is not tied to that call: a reference is
   held by each factory's object bag and, additionally, by the bag of every
   open filter and pin, so the descriptors outlive whatever kernel streaming
   still points at them. **[rig]** Confirmed with a one-off instrumented
   build (run `cleanroom-probe-20261111-114531`, kill scenarios): the driver
   panicked on purpose at the point where a factory had just been deleted
   while one of its filters was open, reporting whether kernel streaming had
   already destroyed the factory object (its object bag emptied). It had
   not: the deleted factory stays alive while filters are open. The case
   occurs when the host process is killed (cleanup runs from the process
   rundown). The probe was then removed.

   *Corrected later (items 41 and 42).* The references described here were
   never all given back, which is why the driver released nothing. And this
   probe could not have seen the factory go: the item it watched was in the
   open filter's bag as well, and kernel streaming frees such an item only
   when it has left every bag. What it concluded is so all the same; item 42
   measured it with an item that is in the factory's bag alone.

6. **[section 5, A.5] Why "create endpoint" completes on a system worker
   thread.** Not explained. `IoRegisterDeviceInterface` is documented as
   callable only "in the context of a system thread", which is the likely
   reason; the implementation does all registry and interface work there.
   [doc]

7. **[section 7] How the per-interface symbolic links are found.** The
   values of A.10 go under "each interface instance", but AVStream only
   exposes the first category's link (`KsFilterFactoryGetSymbolicLink`).
   Resolved: call `IoRegisterDeviceInterface` again for each category with
   the same reference string; it returns the existing instance with
   `STATUS_OBJECT_NAME_EXISTS` (which is presumably why that status is in
   A.4), then `IoOpenDeviceInterfaceRegistryKey`. [doc]

8. **[section 7] Which "interface path" the physical connection reports.**
   Taken to be `KsFilterFactoryGetSymbolicLink` of the other filter's
   factory, i.e. the kernel-form link (`\??\...`) of its first category
   (`KSCATEGORY_AUDIO`), captured when the factory is created. [guess]

9. **[section 7] In which order the two filters' interfaces are switched
   on.** Not stated. Implemented: topology first, then wave. [guess]

10. **[A.9] Not stated for the filter descriptors:** the filter descriptor's
    `ReferenceGuid` (used `KSNAME_Filter`; irrelevant because a reference
    string is always given), node names (none), pin names (none), allocator
    framing (none), component id (none), descriptor flags (0), mediums
    (none, as stated). [guess]

11. **[section 4] "the driver does no processing when the pin is asked to
    process."** The return value is not given. The process callback returns
    `STATUS_PENDING` ("do not call again until asked"); with "do not initiate
    processing" and no transport it is never called anyway. [guess]

12. **[section 4] Fast I/O.** The specification says the driver intercepts
    four IRP major functions. If the framework had a fast I/O device-control
    routine, a control request would reach kernel streaming without passing
    the dispatch table. The implementation clears the driver object's
    `FastIoDispatch` pointer after `KsInitializeDriver` (it is expected to be
    null already for an AVStream minidriver). [guess]

13. **[section 8] Where the stream's format is validated.** AVStream offers
    a create callback and a set-data-format callback; the specification only
    gives the rule and the status. Validation is done in the pin create
    callback on `KSPIN::ConnectionFormat`. [guess]

## C. Control interface

14. **[section 5] The security of the device.** "Who may open the control
    interface is decided by the security on the device", but the
    specification does not say whether the INF sets a security descriptor.
    The INF written here sets none (system default for the device class).
    [guess]

15. **[A.5] Information value on a successful control open, cleanup and
    close.** Not stated; 0 is reported. [guess]

16. **[A.5] `STATUS_FILE_CLOSED` versus a request racing with close.** The
    specification says such a request "either finishes normally or fails
    with that status". For "wait for notification events" there is also the
    row "none queued, queue closed: STATUS_CANCELLED"; a wait that passes the
    closing check and then finds the queue closed gets `STATUS_CANCELLED`.
    [guess]

17. **[section 5] Cancellation of a pending "create endpoint".** Not
    mentioned. The request is not cancellable (it completes in bounded time).
    [guess]

18. **[section 5] Create endpoint with an `id` containing `\` or `/`.** "The
    driver does not check." Kernel streaming / `IoRegisterDeviceInterface`
    refuse such a reference string and that status is returned. [guess]

19. **[A.3] Alignment of the output pointer of "set buffer layout".** Stated
    as 4 although the structure contains a 64-bit field; implemented as 4.

## D. Streams and properties

20. **[section 8, A.9] 32-bit clients.** The WaveRT structures contain
    pointers and handles; `ksmedia.h` defines 32-bit variants
    (`KSRTAUDIO_BUFFER_PROPERTY32`, ...) and the minimum sizes in A.9 (40,
    32, ...) are the 64-bit ones. The specification does not say who
    converts. A rig stack trace shows `ksthunk.sys` (Windows' upper filter
    for the MEDIA class, which converts 32-bit kernel streaming requests)
    sitting above this driver, so the driver sees 64-bit structures only and
    does no conversion itself. It also means this driver is not the top of
    its device stack, which the specification does not mention; `ksthunk`
    calls down on the requesting thread, so "neither"-method buffers are
    still read in the right address space. **[rig]** (the stack trace; a
    32-bit client was not tested)

21. **[section 7] Format matching when the caller's range admits zero
    channels.** "Reduced to the caller's maximum if that is lower" would
    yield a format with 0 channels. Answered with `STATUS_NO_MATCH`. [guess]

22. **[section 7] What makes the caller's range "not an audio range".** Not
    defined. Implemented as: shorter than a `KSDATARANGE_AUDIO` (88 bytes),
    by its `FormatSize`. Kernel streaming has already matched the GUIDs
    against the pin's range before the handler runs. [guess]

23. **[section 8] Minimum channel count of a stream format.** The text says
    "no more channels than the endpoint has"; the pin table says "1 to
    channelCount". A format with 0 channels is refused (`STATUS_NO_MATCH`);
    it would also make the frame size zero. [guess]

24. **[section 8] Order of the open checks.** Format mismatch
    (`STATUS_NO_MATCH`) is checked before "already has a stream"
    (`STATUS_RESOURCE_IN_USE`). Not stated. [guess]

25. **[section 8] Opening a stream on a filter of a withdrawn endpoint.**
    The specification covers property requests ("fail as if the endpoint were
    not found") and filter opens ("cannot be opened again") but not a new pin
    on a filter that is still open. Refused with `STATUS_NOT_FOUND`. [guess]

26. **[A.9] Accuracy of the position register.** `periodSizeBytes` × stream
    channels can exceed 32 bits (2^27 × 32). Saturating multiplication is
    used. [guess]

27. **[section 8] "maps the register table into the opening process, if it
    is not already mapped there for this endpoint".** Since closing a stream
    removes all mappings and only one stream exists at a time, the condition
    can only matter for a later property request from another process.
    Implemented: the stream keeps one register view per process; the
    position-register property maps one on demand for a process that has
    none. [guess]

28. **[A.9] Information value of successful "set" property requests**
    (proposed format, channel configuration, register notification event).
    Not stated; 0 is reported. [guess]

29. **[A.9] `KSPROPERTY_PIN_NAME` "get, set".** The table gives one behavior
    for both; a set is answered like a get. [guess]

30. **[section 6] Register record at stream close.** Only `generation` is
    said to change; the other fields are left as they were. Implemented so.

31. **[section 9] `STATUS_CANCELLED` versus `STATUS_NOT_FOUND` for a
    registration after the host has gone.** Both are specified in different
    places (section 9 step 3 and section 11). Implemented: withdrawn endpoint
    (no owner) gives `STATUS_NOT_FOUND`; an endpoint that still has its owner
    but whose queue has already been closed gives `STATUS_CANCELLED`.

## E. Registry filter

32. **[section 10] How the driver finds out whether a process is restricted
    to Microsoft-signed images.** No documented kernel interface is named.
    Implemented with `ZwQueryInformationProcess(ProcessMitigationPolicy)` for
    `ProcessSignaturePolicy`; "restricted" means any of MicrosoftSignedOnly,
    StoreSignedOnly or MitigationOptIn. `ZwQueryInformationProcess` is
    documented by Microsoft but declared in no WDK header, and the layout of
    the query buffer (policy id, then the policy's flags) is not in the WDK
    headers either. If the call fails the process counts as unrestricted, as
    the specification says. [guess]

33. **[A.11] Partial data on `STATUS_BUFFER_OVERFLOW`.** "If the substitute
    operation fails, its status becomes the caller's result, with the result
    length filled in." For `STATUS_BUFFER_OVERFLOW` the registry also returns
    the part of the data that fits, and callers such as `RegQueryValueEx`
    read the type and length from it. The partial data is copied to the
    caller as well. [guess]

34. **[A.11] Whose buffers.** The caller's result buffer is user memory that
    the registry has probed but not captured; the value name is captured.
    The implementation reads the substitute value into kernel memory and
    copies under an exception handler. Source: `EX_CALLBACK_FUNCTION`
    documentation. [doc]

35. **[section 10] A caller's buffer length can be up to 4 GiB.** "The same
    query, same information class and buffer length" cannot be done
    literally with a kernel buffer of that size. The substitute read starts
    with a 1 KiB buffer and grows to what the value needs, never beyond the
    caller's length; the caller sees the same outcome. [guess]

36. **[A.11] Which user "the reading process runs as".** Taken to be the
    user of the process's primary token (not of an impersonation token), for
    both the starter and the reader. [guess]

## F. Installation

37. **[section 3] INF details not given:** destination directory (system32
    \drivers), `PnpLockdown`, the manufacturer/provider strings, the service
    display name, error control, the exact `Needs` sections
    (`KS.Registration`, `KSCAPTUR.Registration.NT`, read from the system's
    own `ks.inf` and `kscaptur.inf`). The harness's `InfVerif` accepted the
    INF. [rig]

## G. Section 13 choices (deliberate deviations from the existing driver)

Section 13 lists known gaps and lets a new implementation choose. Each
choice, and why it cannot affect existing hosts or Windows:

| Section 13 item | This driver | Why it is compatible |
|---|---|---|
| Unknown control requests succeed | Refused with `STATUS_INVALID_DEVICE_REQUEST` (this includes the retired function 5). | The 0.14.0 host issues only the four documented requests; the full rig suite, which drives the real `SarAsio.dll`, passes. A pre-0.14.0 host that still sends function 5 would now see a failure where it used to see a no-op. |
| A second buffer request on one stream forgets the first | Every buffer a stream obtains is tracked; all of them (cells and client mappings) are released when the stream closes. A second request still succeeds and the register record describes the newest buffer, as before. | Identical while the stream is open; only the leak is gone. Windows asks once per stream. |
| Unregistering a notification event is not implemented | Unchanged (`STATUS_NOT_IMPLEMENTED`). | Implementing it would need a way to take back handles already given to the host. |
| "Capabilities changed" event declared, never raised | Unchanged. | Part of how the filter looks to Windows. |
| Volume and mute nodes declared but inert | Unchanged. | Part of how the filter looks to Windows; the driver must not touch samples. |
| Registry filtering cannot be stopped or handed over | Unchanged. | No request exists for it. |
| Endpoint indexes are not checked for reuse | A create request whose `index` is already used by a live endpoint of the same control handle is refused with `STATUS_INVALID_PARAMETER`. | "Hosts must not do that"; the real host passes the suite. It also bounds the endpoints per handle at 2,340. |
| A failed reply to "set buffer layout" leaves the layout set | The reply buffer's length and address are checked before anything is created, and if the reply still cannot be written the mapping and the section are discarded: the handle has no layout and the request can be repeated. The status and byte count reported for each failure are those of A.5. | A host that passes a valid buffer sees no difference. |
| Clients can write the whole register table | Unchanged (inherent in the layout). The driver itself never uses register contents for a memory access; it only reads `generation` back to increment it and to stamp notifications, as A.7 prescribes. | |
| The section is created with a name | The section has no name. | "Nothing opens it by name." |
| Other request types on a control handle are passed to kernel streaming | Every major function other than create, cleanup, close and device control is completed with `STATUS_INVALID_DEVICE_REQUEST` for a control handle, and the driver object's fast I/O table is cleared so that no request can bypass the check. | Kernel streaming "does not know the handle"; nothing useful could come of passing them on. |
| The INF offers the device on 32-bit x86 | The INF has an `NTamd64` models section only (Windows 10 build 19041 and later). | Only a 64-bit driver exists. |
| The shared header declares `SarNdis` requests | Not implemented, as in the existing driver. | |

Other hardening that is not on the section 13 list:

- **Bounded notification queue.** At most 18,720 (8 x 2,340) registered
  events wait for the host per control handle; beyond that a registration
  fails with `STATUS_INSUFFICIENT_RESOURCES`. The specification sets no
  bound, which would let a client grow kernel memory without limit.
- **Format matching with a zero-channel range** answers `STATUS_NO_MATCH`
  instead of a zero-channel format (see item 21).
- **Register access is bounds-checked.** The driver's view of the section is
  the register cell only; every access checks the offset against the view.

## H. What the rig runs did and did not confirm

The suite drives the real 0.14.0 `SarAsio.dll` (through a test host) and
WASAPI clients in a Windows 11 x64 VM with Driver Verifier on this driver.

Confirmed by passing runs (the behavior had to be right for the scenario to
pass):

- Items 6, 7, 8, 9, 10: endpoints are built by Windows from the two filters,
  carry the host's names, and can be recreated with the same identity after
  the host stops, is restarted, or is killed.
- Items 11, 13, 24, 27: shared-mode render and capture streams open, run
  event-driven and close, on 1 to 32 endpoints at once, at 48 kHz and
  96 kHz, stereo and mono, and under rapid open/close.
- Item 14: the default device security lets the (non-elevated or elevated,
  not known) test user open the control interface and lets the audio engine
  open the filters.
- Items 33 to 36: application routing works for a 64-bit client process
  (the `app-routing` scenario); the first attempt crashed in exactly this
  path (see below), so the path is exercised.
- Item 5: by the instrumented run described there (but see the correction
  there).
- Item 20 (ksthunk above the driver): seen in a stack trace.
- Items 38 and 39: by the instrumented runs described in item 38.
- Items 41 to 45: by the instrumented runs described there and by the
  `unload` scenarios.

Found by the rig, not by reasoning:

- **Empty strings must be passed to `Zw` routines with a null buffer
  pointer.** The default value of a key has the empty name; a Rust empty
  slice has a dangling non-null pointer (0x2), and Driver Verifier stops the
  system (0xC4, subcode 0xE3, "Kernel Zw API called with user-mode address")
  when it sees one. Nothing in the specification could have warned of this;
  it is a property of the implementation language.

Not exercised by any scenario, so only reasoned about:

- Item 1 for a layout whose audio area is not a multiple of 65,536 bytes (it
  is not known which `bufferSize` values the host uses).
- 32-bit hosts and 32-bit clients; the 32-bit registry redirections.
- A process restricted to Microsoft-signed images (item 32).
- Every refusal path of the control interface (bad lengths, bad pointers,
  out-of-range parameters), the section 13 choices, a second buffer request,
  more than one process on a stream, event registration with a bad handle.
  Their status values come from unit-tested code in `sar-core`, but the
  kernel side (for example the exception codes of a failed probe) was not
  observed.
- Driver unload and device stop/removal: the VM is restored from a
  checkpoint after every run, so the unload path, and with it Driver
  Verifier's check for leaked pool, never ran. (True when this was written.
  The suite has since gained an `unload` group, which found that the driver
  released nothing; see section J.)
- A failure of `KsFilterFactoryUpdateCacheData` (item 40). The call was
  never seen to fail, so the code that ignores its result only ever had a
  success to ignore.

## I. The "Cached filter data" paragraph of A.9

This paragraph was added to the specification after the driver had been
written. The items continue the numbering of sections A to F.

38. **[A.9] On which thread the cached filter data is written.** "Right
    after creating the two factories, and before either one's device
    interfaces are switched on" fits two places: the requesting thread, where
    the factories are created before the request is answered as pending
    (A.5), and the worker thread that later writes the values of A.10 and
    switches the interfaces on. Implemented as written: on the requesting
    thread, straight after the second `KsCreateFilterFactory` and in the same
    hold of the device mutex, so that nothing can come between creating the
    factories and the write. Microsoft's page for
    `KsFilterFactoryUpdateCacheData` asks for `PASSIVE_LEVEL` and names no
    thread context. The call site is a device control dispatch on the
    requesting thread. Of the three locks held there, the control handle's
    and the publish queue's are kernel mutexes, which leave the IRQL at
    `PASSIVE_LEVEL`, and the device mutex is the one `KsCreateFilterFactory`,
    itself a `PASSIVE_LEVEL` routine, has to be called under. [doc] for the
    IRQL. **[rig]** for the rest, by two instrumented builds, both removed
    again:

    - Run `cleanroom-cachedata-probe-stop2-20261111-134939` (`issues`). The
      first endpoint the driver created went without the two calls. The
      second got them, and when it reached the worker thread the driver
      stopped the system on purpose and reported what it had seen. Both
      calls had returned a success status. Each of the five interface
      instances of the endpoint that got the calls (wave filter: audio,
      real-time, render or capture; topology filter: audio, topology) had a
      `FilterData` value; the wave filter's audio instance, the one whose
      value was also measured, held 160 bytes of `REG_BINARY`. The five
      instances of the endpoint that went without had none (the query was
      answered `STATUS_OBJECT_NAME_NOT_FOUND`). In this run, then,
      `KsCreateFilterFactory` alone left no such value and the call wrote
      one.
    - Run `cleanroom-cachedata-probe-fail-20261111-135429` (`issues`). Both
      calls were made as in the committed code, but a failure of either
      failed the create request, and so did an interface instance without a
      `FilterData` value once the endpoint reached the worker thread. Every
      scenario passed: for every endpoint those scenarios create, both
      calls succeeded and all five values were there.

    An earlier run of the first build
    (`cleanroom-cachedata-probe-stop-20261111-134451`) stopped the same way,
    but the rig did not deliver its results (see `PROVENANCE.md`). The
    committed code, which ignores both results, passes the whole suite (run
    `cleanroom-cachedata-final-20261111-135739`: `issues`, `matrix`, `race`,
    `kill`).

39. **[A.9] Whether the device mutex is held for the call.** The
    specification does not say, and neither does Microsoft: the function's
    page mentions no mutex, although "Mutexes in AVStream" says that the
    reference pages state which mutex a function needs. Two documented facts
    pull in opposite directions. A factory is only certain to exist while
    the device mutex is held ("Device Mutex in AVStream": the object
    hierarchy is stable only under it, and these factories are freed when
    the device stops). And the device mutex "cannot be obtained
    recursively", so a routine that took it itself would hang when called
    with it held. Resolved: called with the device mutex held, as
    `KsFilterFactorySetDeviceClassesState` always has been here (its page is
    just as silent). **[rig]** In the runs of item 38 the call returned, and
    succeeded, with the mutex held.

40. **[A.9] What else "the results are not checked" covers.** Decided as
    follows. [guess]

    - If the second factory cannot be created, the first is deleted and
      nothing is written for it: the paragraph begins "after creating the
      two factories".
    - The write comes before the driver reads the factories' interface
      paths (item 8). If reading those fails, the request fails and the
      factories are deleted; what was written is not taken back.
    - `ks.h` marks the function's result as one that must be inspected
      (`_Must_inspect_result_`). The specification wins: the wrapper returns
      the status and the driver discards it. Nothing is logged or retried,
      and the request's own status and byte count (A.5) do not change.

## J. Releasing endpoints and sections (section 11)

"What is released, and when" and the rewritten "The driver unloads" were
added to section 11 after a run with the new `unload` scenarios ended in
Driver Verifier's stop for pool left allocated at unload (0xC4, subcode
0x62): 38,744 allocations, among them every endpoint and every section the
run had created.

41. **[section 11] Why nothing was released: an object bag counts an item
    once, however many bags it is in.** The driver kept each factory's block
    (the descriptors kernel streaming reads, and the factory's reference to
    its endpoint) alive with a reference in the factory's object bag and one
    more in the bag of every filter and pin opened from it, each to be given
    back by the free routine registered with that bag (item 5). Kernel
    streaming does not call a free routine per bag. "Object Bags" says that
    an item placed in more than one bag is not released "until it is no
    longer contained in any object bag", and `KsRemoveItemFromObjectBag`
    "returns the number of references on Item": one pointer in several bags
    is one item, with one count and one routine. [doc] **[rig]** Run
    `cleanroom-unload-probeA-20261111-141438` (`issues`, `unload`) was the
    driver as it stood, with counters, stopped on purpose in its unload
    routine. 56 blocks had been put in a factory's bag, and 657 times one of
    them had been put in a filter's or a pin's (54 of those a pin's). The
    routine registered with the factories' bags had run 56 times, every time
    before the device was first told to stop. The routine registered with
    the filters' and pins' bags had never run. No block had been freed. Each
    block was left holding one reference for every filter and pin it had
    ever had, and every block had had some.

    Resolved: a factory's block is one allocation, with no count of the
    driver's own. It goes into the factory's bag and, as before, into the
    bag of every filter and pin created from the factory, always as the same
    item with the same free routine, and the single call kernel streaming
    makes of that routine frees it. The endpoint goes when both its blocks
    have gone and the control handle has let go of it; the section when its
    last endpoint and the control handle have.

42. **[section 11] When kernel streaming destroys a factory the driver has
    deleted.** Microsoft says that `KsDeleteFilterFactory` "deletes a given
    filter factory" and that what is in a factory's bag is cleaned up "when
    the filter factory is deleted". Whether that happens at once when
    filters are open, and when it happens otherwise, is not documented, and
    item 5's probe could not tell (see the correction there). It matters
    twice. The first rule of "What is released, and when" holds only if a
    deleted factory does not linger until the device stops. And a filter
    must not outlive the descriptor it was created from. **[rig]** Measured
    with an item that is in the factory's bag and nowhere else, so that its
    free routine marks the end of the factory object itself. (The driver
    keeps such an item for good: it is how a `FilterFactory` handle learns
    that its pointer to the factory is dead.)

    - Run `cleanroom-unload-probeB-20261111-142405` (`issues`, `kill`,
      `unload`), stopped on purpose when the device was first told to stop.
      All 184 factories had a filter open when the driver deleted them:
      Windows keeps an endpoint's filters open for as long as the endpoint
      exists. None was destroyed inside `KsDeleteFilterFactory`, none while
      one of its filters was open, and all 184 were gone by the time of the
      stop. All 184 blocks had been freed, each one when the last thing
      that had happened to its factory was a filter's Close dispatch: none
      inside the delete call, none after a pin's Close. The build would have
      stopped the system at once had a block been freed with a filter or pin
      open, or a filter been created from a factory already destroyed.
      Neither happened.
    - Run `cleanroom-unload-probeC-20261111-143042` (`issues`, `unload`):
      the same build, made to create a third factory for every endpoint and
      delete it at once, before anything could open a filter. All 28 were
      destroyed, and their blocks freed, before `KsDeleteFilterFactory`
      returned. The 56 real factories behaved as in the other run.

    So a deleted factory is destroyed at once if it has no filter, and
    otherwise when its last filter closes. It outlives its filters, and they
    outlive their pins. The driver depends on this in two places only. The
    first rule depends on a deleted factory not lingering. And one failure
    path frees a block directly: if the block cannot be put in a new
    factory's bag, the factory is deleted and the block is freed after that
    call, which is sound because that factory never had a filter. Everything
    else rests on what Microsoft documents about bags (item 43).

43. **[section 11] Which bags hold the block, and how it is known that
    nothing reaches it once it is freed.** The block is reached through the
    descriptor pointer and the context pointer of the factory, of each
    filter (kernel streaming gives a filter its factory's) and of each pin
    (its filter's), and through nothing else: the driver's own handle to a
    factory holds no pointer into it. Each of those objects has the block in
    its bag. Microsoft documents that a bag is emptied when its object is
    deleted (what is in it "is automatically cleaned up when the filter
    factory is deleted", "when the filter is deleted", "when the pin is
    closed") and that an item in several bags is freed when it has left the
    last. So the free routine cannot run while an object that points into
    the block exists. [doc]

    By item 42 the factory's bag alone would be enough on this version of
    Windows. The filters' and pins' bags are kept so that the free does not
    rest on that order, which is observed and not documented.

    `KsAddItemToObjectBag` asks for the mutex that guards the bag, and it is
    held each time: the device mutex for a factory's bag (whoever creates a
    factory holds it), and the filter control mutex for a filter's or a
    pin's, which kernel streaming holds while it calls a filter's or a pin's
    Create dispatch ("Filter Control Mutex in AVStream"). [doc]

    What no page says is whether kernel streaming reads an object's
    descriptor once more after emptying that object's bag, while it finishes
    deleting it. Microsoft's own pattern implies that it does not: `KsEdit`
    puts an object's dynamically allocated descriptor in that object's bag,
    where it is freed at this very moment. **[rig]** And the rig would show
    it. Driver Verifier's special pool is on for this driver and took every
    allocation it made (in the listing of the failing run, "Pool Allocations
    Succeeded SpecialPool" equals the number attempted), so a read of a
    freed block faults on the spot. The runs of item 42 freed every block
    they created, and so did the acceptance run of item 45 (whose scenarios
    made 1,172 blocks in the failing run), without one.

44. **[section 11] Where an endpoint and a section are released now.**
    Usually not on a thread of the host. When the host closes its control
    handle, the endpoint is withdrawn with filters still open (all 92
    endpoints of run probeB), and its last reference is dropped later,
    inside kernel streaming's cleanup of the last of its filters to close,
    on whatever thread closes that filter. If the control handle has gone by
    then, the section goes there too: the kernel view is unmapped, the
    section object dereferenced and its kernel handle closed. None of that
    needs a particular process, and nothing in it takes a lock that kernel
    streaming could be holding. **[rig]** Every release in the runs of item
    42 happened this way, under Driver Verifier's IRQL checking and deadlock
    detection.

45. **[section 11] Shown: released at the last close, not when the device
    stops.** A clean unload shows only that nothing is left at the end. Run
    probeB counted live endpoints and sections and reported them at the
    moment the device was first told to stop, which is before kernel
    streaming frees anything on account of the stop. Twelve control handles
    had come and gone by then. Of the 92 endpoints and 12 sections they had
    created, none existed. 32 of the endpoints, the 16 pairs of the
    `kill-host` scenario, had been withdrawn while a client still had their
    stream open, the host having been killed mid-stream; all 32 had been
    released. Run probeC: none of 28 endpoints and 10 sections existed.
    **[rig]**

    The committed code, without counters, passes the whole suite including
    the `unload` group (run `cleanroom-unload-accept-20261111-143701`, 20
    scenarios): the driver unloads with nothing left allocated and works
    again when the device is enabled.

46. **[section 11] Not exercised.** Only reasoned about:

    - A device stop while a host still has endpoints. Kernel streaming then
      frees the factories itself ("freed when the device stops"), their own
      items tell the handles, and the endpoints go when the host's handle
      closes. No scenario stops the device under a running host.
    - `KsAddItemToObjectBag` failing, in the three places it is called for a
      block or a factory's own item.
    - A filter or pin whose Create dispatch fails after the block has been
      put in its bag (a filter opened on an endpoint that has just been
      withdrawn, a second stream). Kernel streaming then deletes the object
      and empties its bag like any other. Whether a scenario produced one is
      not known.
    - Other versions of Windows than the rig's.
