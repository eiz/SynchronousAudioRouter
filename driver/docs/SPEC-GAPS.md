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

   *Later (item 61).* The driver's own mapping of the register table is no
   longer a view. It is a mapping of the table's locked pages and starts at
   `registerBase` exactly. A client's view is as described here.

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

    *Later (section K).* A 32-bit client has since been tested, and what
    arrives was measured: items 47 to 54.

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

    *Later (item 51).* The condition matters every time for an
    exclusive-mode stream: its pin is not created by the process that then
    uses it.

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
  (True when this was written. The test tools now exist as 32-bit builds;
  see section K.)
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

    *Later (items 55 and 58).* Still on the requesting thread, straight
    after the second factory has been created and before either one's
    interfaces are switched on, but no longer under any lock: the control
    handle's and the publish queue's are not held while an endpoint's
    factories are created, and the device mutex is not held for this call.

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

    *Decided again (item 55).* Section 14 asks that the device mutex be held
    "around the kernel streaming calls that need it and around nothing
    else". This call, and the switching of a factory's interfaces, are now
    made without it. The first of the two facts above is met another way
    (item 56); the second never said that the mutex is needed.

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

    *Later (item 63).* Not there any more. The routine kernel streaming
    calls to free a factory's block now hands the block to a system worker
    thread, and the endpoint and the section go on that thread.

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

## K. Requests of 32-bit clients, and what a request says about its origin (A.8)

A.8 gained "Whose handle it is" and its closing paragraph after the suite,
run with 32-bit test tools, stopped the machine in the driver: Driver
Verifier 0xC4, subcode 0xF6 ("a driver references a user-mode handle as
kernel mode"), when a 32-bit client registered the event of an
exclusive-mode stream.

47. **[A.8] Why a 32-bit client's event handle was looked up with kernel
    access.** The kernel streaming wrapper told the driver whether a
    property request was marked as coming from user mode, and the driver
    looked the event handle up with user access only then, and with kernel
    access otherwise. The mark is not the caller's. The requests of a 32-bit
    process that need converting reach the driver marked as kernel-mode
    requests, with the process's own handle value in them (item 48).
    `ObReferenceObjectByHandle` is documented for this: "Drivers should
    always specify UserMode for handles they receive from user address
    space", and Driver Verifier stops the machine with exactly this code
    when a driver does otherwise. [doc]

    Resolved: the handle is always looked up with user access, in the
    process the request runs in. And nothing can ask for anything else: the
    property request the wrapper hands the driver no longer says how the
    request was marked, and the unused accessor for the same mark on a
    control request was removed. Neither `ks` nor `sar-driver` reads a
    request's origin mark any more. **[rig]**

    - Run `cleanroom-thunk-fix32-20261111-180338` (32-bit tools, `issues`,
      `conformance`): the conformance test passes through to its end. A
      32-bit client opens an exclusive-mode, event-driven stream, the host is
      handed the event the client registered, the client's event is
      signalled when the host signals its handle, and the client refills its
      buffer twenty times.
    - Run `cleanroom-thunk-probeK-20261111-182757` (64-bit tools, `issues`):
      a build that, on the first registration, created an event with a
      kernel handle and looked that handle's value up the way a client's is,
      then stopped on purpose. The client's own handle (0x3F0, in the audio
      engine's process) had been accepted. The kernel handle's value
      (0xFFFFFFFF80002A5C) was refused with `STATUS_INVALID_HANDLE`, and the
      current-process pseudo-handle with `STATUS_OBJECT_TYPE_MISMATCH`. That
      is the object manager's answer for "a value that names a kernel
      handle".

48. **[A.8; item 20] How the requests of a 32-bit client arrive.** Microsoft
    documents the 32-bit WaveRT structures by name only
    (`KSRTAUDIO_BUFFER32` and the others have no remarks at all) and does
    not document `ksthunk.sys`; a search of learn.microsoft.com found no
    page that describes it. What follows was measured. **[rig]** Run
    `cleanroom-thunk-probeT32-20261111-183241` (32-bit tools, `issues`,
    `conformance`, `unload`) counted what the wrapper saw and reported it
    when the device was first told to stop:

    - Exactly three property requests arrived marked as coming from kernel
      mode: buffer with notification, position register, and register
      notification event. These are the WaveRT requests whose structures
      hold an address or a handle and so differ between 32 and 64 bits. All
      three ran on a thread of a 32-bit process, the client's own. For all
      three `IoIs32bitProcess` given the request answered "not 32-bit": the
      request the driver sees is not the one the 32-bit process issued.
    - The one other WaveRT request of the 32-bit client, hardware latency,
      whose structure is the same in both sizes, arrived marked as coming
      from user mode, unconverted.
    - For every property request of the run, however marked, the request
      and the value buffer handed to the driver were in kernel memory and
      did not overlap (the build would have stopped at once otherwise). That
      agrees with Microsoft's description of a property handler's
      arguments, "an aligned copy" of each.
    - No pin was created on a thread of a 32-bit process (item 51).

    So the driver does no conversion, sees 64-bit structures only (item 20),
    and cannot tell a 32-bit client by anything in the request. It does not
    need to.

49. **[A.8] Everything else a request carries, checked against "not to be
    trusted more because the request is marked as coming from kernel
    mode".** With the mark out of reach (item 47) nothing in the driver can
    depend on it. What each value is checked against instead:

    | What a request carries | What the driver does with it |
    |---|---|
    | Event handle (register notification event) | Looked up with user access in the requesting process (item 47). |
    | Requested buffer address (buffer requests) | Must be null; otherwise `STATUS_NOT_IMPLEMENTED`. Never used as an address. |
    | Requested buffer size | Goes through the size calculation of section 8, every step checked for overflow, and must fit the audio area. |
    | Notification count | Stored in the register record as given, as specified; the driver does not act on it. |
    | Requested register address (position register) | Ignored. |
    | Pin and node numbers | Compared with the numbers the filter has; anything else is `STATUS_NOT_FOUND`. |
    | Lengths of the request and of the value buffer | Every field is read through a length check in `sar-core`; the minimum sizes kernel streaming enforces are not relied on. |
    | Format of a proposed-format or pin-creation request | Checked field by field with length checks. The length of a pin's format is the one kernel streaming validated when it created the pin (`KsValidateConnectRequest` "validates ... basic data format"); the driver has nothing of its own to bound it by. |
    | Caller's data range in an intersection request | Was taken to be as long as it said, on the strength of kernel streaming's checks. Now used only if all of it lies inside the request (changed with this work; in run probeT32 every range did). |
    | State values | Anything but the four states is `STATUS_INVALID_PARAMETER`. |
    | Control requests (section 5) | Their buffers are always probed as user addresses; they never depended on the mark. |

    The addresses in the replies are the driver's own (item 50).

50. **[A.9] How an address given to a 32-bit client stays within its
    reach.** The specification does not say, and the driver does nothing
    special. A buffer or register view is mapped into the process the
    request runs in, at an address the system chooses. For a converted
    request that process is the 32-bit client itself (item 48: the request
    runs on the client's own thread), and in a 32-bit process "all user-mode
    address space above the 32-bit limit is reserved by the system" ("WOW64
    Implementation Details"; the limit is 2 GB, or 4 GB for an image marked
    large-address-aware). So the system cannot place the view where the
    32-bit `BufferAddress` or `Register` field could not hold it, and
    whatever converts the reply only has to drop zero bits. [doc] **[rig]**
    In run probeT32 the highest address any buffer or register reply to the
    32-bit client covered was 0x01103008, and in the runs without counters
    the client streamed through the buffer it was given (item 47). The
    host's view is in the same position: a
    32-bit host was given its whole section, 128 MiB in one case, at
    0x02E60000.

    Not settled: the driver would hand a 64-bit address to whatever asked,
    and does not itself check that the address fits the client. It could
    only check by learning that the client is 32-bit, which nothing in the
    request says (item 48).

51. **[section 8; item 27] Who creates the pin of an exclusive-mode
    stream.** Not the client. In run probeT32 a 32-bit client ran an
    exclusive-mode stream and made its buffer, register and event requests
    on its own threads, yet no pin's Create dispatch ran on a thread of a
    32-bit process: the pin was created in another process, and the client
    used it from its own. "Maps the register table into the opening
    process" therefore puts a view in a process that never asks for the
    register, and the client gets its own view when it asks for the position
    register (item 27, which is thus not a corner case). Both go when the
    stream closes. **[rig]**

52. **[section 10, A.11] Application routing for a 32-bit client.** With
    the 32-bit tools the `app-routing` scenario fails: "the client never
    loaded SarAsio.dll, so application routing was not in effect". The
    driver's part works, and the client does load it. **[rig]**

    - Run probeT32: 23 reads by 32-bit processes of the in-process server
      of one of the two classes, every one at the key A.11 gives for a
      32-bit process, none anywhere else.
    - Run `cleanroom-thunk-probeR32-20261111-184406` (32-bit tools,
      `issues`, `unload`): eight such reads of the device enumerator class;
      the wrapper's key opened every time; four were answered with the
      wrapper's value (a result of 78 bytes, which is a 32-character path
      such as that of the 32-bit `SarAsio.dll` of the test tools) and four
      with the status of the same read on the wrapper's key, as A.11 says.
    - Run `cleanroom-thunk-probeR2-32-20261111-185149` (32-bit tools,
      `issues`, `unload`) recorded process ids. The reads answered with the
      wrapper's value all came from one process, which never opened a
      control handle and into which an image named `SarAsio.dll` was mapped
      after it had been mapped into the scenario's host: the client.

    So the 32-bit client's read is redirected and `SarAsio.dll` is mapped
    into it. Why the harness concludes otherwise is outside the driver and
    was not pursued.

53. **[A.8] Still not settled or not exercised for 32-bit clients.**

    - How a 32-bit handle value is widened on the way down is not
      documented. It does not matter to the lookup (item 47), which refuses
      a kernel handle's value however it comes about; the refusal was shown
      for such a value in a 64-bit process, not for one sent by a 32-bit
      process.
    - Only the three converted requests of item 48 were seen marked as
      kernel-mode. Which others Windows would convert (the clock register
      and the unregistration have structures of the same kind) was not
      seen; they are answered `STATUS_NOT_IMPLEMENTED` whatever their mark.
    - A request that really comes from kernel code, with a kernel handle
      for its event, is now refused. The specification asks for that.
    - 32-bit clients of other kinds than x86 on x64 (the rig has no other).
    - A kernel streaming property request sent to a control handle never
      reaches the driver from a 32-bit process: it is answered above the
      driver with 0xC0000206, where a 64-bit process gets the driver's
      `STATUS_INVALID_DEVICE_REQUEST` (the conformance test records both as
      observed, not as required).

54. **[rig] What the suite shows with the 32-bit tools.** The committed code
    passes the whole suite with the 64-bit tools (run
    `cleanroom-thunk-accept64-20261111-185720`, 36 scenarios). With the
    32-bit tools it neither stops nor hangs the machine, and the
    conformance test, the 32-bit fuzzing (26,152 kernel streaming and 3,040
    control requests in one run) and the unload pass; but the run as a
    whole does not:

    - `app-routing` fails every time, for the reason in item 52.
    - In run `cleanroom-thunk-accept32-20261111-190915`, `recovery-after-kill`
      and `run-after-fuzz` also failed, both with "signal verified for too
      little of the duration": the 32-bit client's streams began about three
      seconds after the test started its five-second window (in the 64-bit
      run, 0.03 s and 0.07 s after). Neither failed again: not in run
      `cleanroom-thunk-accept32b-20261111-193733`, the same build and
      scenarios, where `app-routing` alone failed, and not in run
      `cleanroom-thunk-probeD32-20261111-192349`, a build with timers.

    Shared-mode streams are served to the audio engine's own process
    whatever the client is (in run probeT32 no property request came from a
    32-bit process outside the exclusive-mode stream), so nothing the driver
    is asked on their behalf depends on the client being a 32-bit one. Why a
    32-bit client is sometimes that slow to start was not found.

## L. How long requests hold each other up, and locked memory (sections 6 and 14)

Section 14, "It stays in physical memory" in section 6 and the last entry of
section 13 were added after the `contention` scenario showed what the driver
did to its clients: while a host with 64 endpoints was being closed, a client
opening a filter of another host waited for up to a third of a second, and
while one was being created, half of all opens and closes waited 40 ms or
more. The driver held kernel streaming's device mutex for the whole of each
endpoint's creation, for its publication, registry writes included, and for
the withdrawal of all of a closing handle's endpoints together.

55. **[section 14; item 39] Which kernel streaming calls "need" the device
    mutex.** The specification leaves that to the reader, and names only what
    does not: writing the registry values of A.10. Decided from Microsoft's
    pages. "Mutexes in AVStream" says that a function's reference page states
    whether a mutex has to be held for it. The page of
    `KsCreateFilterFactory` does; so does that of `KsAddItemToObjectBag`,
    for the bag's mutex, which for a factory's bag is the device mutex. And
    "Device Mutex in AVStream" says that the mutex synchronises the creation
    and destruction of filter factories, which covers
    `KsDeleteFilterFactory`. The pages of
    `KsFilterFactorySetDeviceClassesState`, `KsFilterFactoryUpdateCacheData`
    and `KsFilterFactoryGetSymbolicLink` name no mutex. [doc]

    Resolved: the device mutex is held for creating a factory and filling
    its bag, and for deleting it, one factory at a time, and for nothing
    else. It is taken inside the kernel streaming wrapper, which no longer
    gives the driver a way to hold it across anything. Switching a factory's
    interfaces on or off, writing its cached filter data and reading its
    symbolic link are done without it, as are the registry values. What a
    factory's creation needs in memory is allocated before the mutex is
    taken, and what a failed creation leaves is freed after it is released.

    That the three calls work without the mutex is not something Microsoft
    says in so many words. **[rig]** A build made for the purpose had one
    thread hold the device mutex while another made all three calls on a
    factory, switching it both on and off. In every one of five loads of
    that build (runs `cleanroom-rt-a1-20261111-230858`,
    `cleanroom-rt-a2-20261111-231529`, `cleanroom-rt-a3-20261111-232259`,
    Windows 11; the Windows 10 run is in item 66) all four calls succeeded
    and returned, in between 0.05 and 6.9 ms each, while the other thread
    still held the mutex. They do not take it themselves, then, and they do
    not need it. The suite, which runs on the result, is the rest of the
    evidence.

56. **[section 14; item 39] What keeps a factory in existence during a call
    that is made without the device mutex.** Item 39 held the mutex for
    these calls because a factory is only certain to exist under it. Two
    things can destroy one, and each is now ruled out by other means.

    - *The driver's own deletion of it.* A factory's handle cannot be
      copied, the calls take it exclusively and deletion consumes it; and an
      endpoint's two factories are in one place at a time: at rest in the
      endpoint's state, or in the hands of the one thread that is creating,
      publishing or withdrawing the endpoint (item 57). The compiler holds
      the driver to this; nothing is checked at run time.
    - *Kernel streaming's, when the device stops* (factories are created to
      be freed then). Every call that is made without the mutex is made
      inside a gate, Windows' run-down protection, one per device. The
      device's stop, remove and surprise-removal callbacks shut the gate
      first: that refuses later calls and waits for those under way. A
      device that starts again opens it.

    The gate rests on two things Microsoft does not document. First, that
    kernel streaming calls the device's stop callbacks before it frees the
    factories, not after. **[rig]** In the runs of item 55 a factory that
    the build never deleted still existed at the first of the two such
    callbacks a device removal makes, and was gone at the second; all 773
    factories of a run had been destroyed by then. Second, that the calls
    under way do not need the device mutex, which kernel streaming holds
    during those callbacks: shown in item 55. If the first were otherwise on
    some version of Windows, a call under way while the device is stopped
    could be working on a freed factory. That needs the device to be stopped
    while a control handle is open or still closing, which Plug and Play
    does not do for an ordinary removal (section 11) and which no scenario
    does (item 46).

57. **[sections 11 and 14] Withdrawing "endpoint by endpoint", and what had
    to change with it.** Each of these was implied by the one hold of the
    device mutex that is gone.

    - *Who may touch an endpoint's factories.* Publication (worker thread)
      and withdrawal (the thread closing the handle) used to exclude each
      other through the device mutex. Now the thread that has kernel
      streaming calls to make takes the two factories out of the endpoint's
      state, under the endpoint's lock, and puts them back or deletes them.
      A close that finds them taken removes the endpoint's owner and waits,
      holding nothing, for the other thread to notice and delete them.
    - *In which order an endpoint goes.* Section 11's order: its
      interfaces are switched off, then it loses its owner, then its
      factories are deleted. The driver used to take the owner away first,
      which nobody could see while the device mutex kept Windows out. Now
      Windows asks in between, and should find an endpoint gone, not
      present and refusing everything. For the same reason a closing
      handle's endpoints go on working until their turn comes. What all of
      them are told at the start of the close (memory only) is that they
      are going, so that nothing new is begun on them and a request for
      the same identity waits (below).
    - *What a close waits for.* It returns when none of the handle's
      factories is left, as before. An endpoint whose factories another
      thread is still creating is one of them: the endpoint joins the
      handle's endpoints before its factories exist (item 58), so the close
      sees it. The close withdraws every endpoint it can withdraw itself
      before it waits for any other, because the thread it waits for may be
      waiting for one of those (next point).
    - *The same identity, asked for while it is being withdrawn.* A host
      that is started while its predecessor's handle is still closing asks
      for endpoints whose factories may not have been deleted yet. Under the
      single hold its request simply waited for the whole close. Kernel
      streaming refuses a second factory with the same reference string, so
      without care the request would now fail. Resolved: the device keeps
      the reference strings of the endpoints whose factories exist. A
      request for one that is being withdrawn waits, holding no lock, until
      those factories are gone; one that collides with a live endpoint is
      left to kernel streaming to refuse, as before. Reference strings are
      compared without regard to the case of ASCII letters (erring towards
      "same" costs a short wait). [guess] for the comparison; no scenario
      is known to depend on it, but `recovery-after-kill` starts a host
      right after killing one.

58. **[sections 5 and 14] "Create endpoint" without the control handle's
    lock.** The request used to hold the device mutex, the control handle's
    lock and the publish queue's lock from its checks to the moment it was
    queued. Now:

    - The checks (closed, no layout, index in use) are made under the
      control handle's lock, the endpoint is allocated with no lock held,
      and it is put among the handle's endpoints under the lock, after the
      same checks again. That is what reserves its index.
    - Its factories are created with no lock of the driver's held.
    - A filter cannot be opened on it until both factories exist and their
      paths are known (`STATUS_NOT_FOUND` before). The single hold used to
      guarantee that; a client that knows a path from an earlier host could
      otherwise open a half-made endpoint.
    - It is marked pending and queued under the publish queue's lock alone.
    - If the handle closed while the factories were being created, the
      request deletes them and fails with `STATUS_FILE_CLOSED` at once (the
      row "the handle closed meanwhile" of A.5). If it closes after that,
      the request is already pending and completes with that status from
      the worker thread, as it always could.
    - Requests are still "finished in the order they were made". The device
      mutex no longer puts the worker threads in line, so one of them
      publishes at a time: a work item that finds another publishing
      returns, and the one that is publishing goes on until the queue is
      empty.

    Statuses are those of A.5. Where two refusals apply at once the order
    is: closed, no layout, index in use, a name that does not fit, out of
    memory, kernel streaming's refusal, closed meanwhile.

59. **[section 14] "Refused without looking at an endpoint's state": which
    requests, and what a request that is wrong twice now gets.** Section 11
    says that every property request on a withdrawn endpoint fails as if the
    endpoint were not found; section 14 says that some requests are refused
    without a look at the endpoint, which cannot then know that it has been
    withdrawn. The second is followed where the two meet. Refused before
    the endpoint's lock is taken, whatever the endpoint's state:

    | Request | Status | Before |
    |---|---|---|
    | Unregister notification event, clock register, packet count, presentation position | `STATUS_NOT_IMPLEMENTED` | `STATUS_NOT_FOUND` on a withdrawn endpoint |
    | Buffer request naming an address | `STATUS_NOT_IMPLEMENTED` | the same |
    | Buffer or event request too short to hold its fields | `STATUS_INVALID_PARAMETER` | the same |
    | Format matching on a pin other than the streaming pin | `STATUS_NO_MATCH` | the same |
    | A pin, node or property the filter does not have | `STATUS_NOT_FOUND` | unchanged |

    Every other property request still answers `STATUS_NOT_FOUND` on a
    withdrawn endpoint. The requests that are answered from the endpoint's
    fixed properties alone (proposed format, channel configuration, pin
    name) take the lock only to see that it still has an owner.

60. **[section 14] The notification queue under its spin lock.** The queue
    was a growable ring: registering an event could reallocate it under the
    lock, collecting events allocated a batch under it, and closing moved
    everything. Resolved, in `sar-core`, where it is unit-tested:

    - Events are kept in chunks of 128. The list of chunks is given room
      for the fullest queue (item G: 18,720 events) when the control handle
      is opened, so it never grows. A chunk is allocated, with no lock held,
      by the registration that finds the newest one full: the queue says so,
      and the event is offered a second time together with the chunk.
    - A wait request takes at most 32 events under one hold of the lock,
      into a batch on its own stack (512 bytes), and takes the lock again
      for the next 32 while its buffer has room. "As many as fit, oldest
      first" still holds; an event registered between two holds can now be
      delivered by the request that was already collecting. Existing hosts
      ask for 32 at a time.
    - A chunk that a collection empties leaves the queue with the batch and
      is freed after the lock is released; so are a refused event, a chunk
      that turned out not to be needed, and everything nobody collected
      when the queue closes (which is one pointer swap under the lock).

    Under the lock, then: no allocation, no free, and at most 32 events
    copied. **[rig]** In the runs of item 55 the lock was never held longer
    than 11 microseconds.

61. **[section 6] How the section is locked.** The specification says what
    is to be true, not how. A section's pages can only be locked through a
    view. Resolved: when the buffer layout is set the driver maps the whole
    section into system address space, describes the audio area and the
    register table by one memory descriptor list each, and has the memory
    manager make their pages resident and lock them (`MmProbeAndLockPages`,
    which raises an exception when it cannot; the one C file gained a fifth
    function for that). The view is then removed again: the locks are on the
    pages, not on the view. The register table's locked pages are mapped
    into system address space a second time (`MmMapLockedPagesSpecifyCache`,
    not executable), and that mapping, which starts at `registerBase`
    exactly, is the only one the driver reads and writes through. The audio
    area's pages are locked and never mapped: the temporary view is the one
    moment at which they appear in kernel address space, and nothing is read
    or written through it. [doc]

    Any failure on that path (the temporary view, either lock, the second
    mapping) is reported as `STATUS_INSUFFICIENT_RESOURCES`, and whatever
    had been made is undone before the request is answered: no section, no
    mapping, and the request can be repeated. The section's creation itself
    still reports the memory manager's status (A.5).

62. **[section 6] How it was checked.** "That the section is locked in
    physical memory cannot be seen from user mode, and the test suite does
    not check it." **[rig]** The builds of item 55 checked it from inside:

    - For every section, every page was read with the IRQL raised to
      `DISPATCH_LEVEL`: the audio area's through a mapping of its locked
      pages made for the test and removed again, the register table's
      through the mapping the driver uses. A page fault at that level stops
      the machine, and with Driver Verifier's IRQL checking, which the rig
      has on, pageable memory is thrown out of the working set whenever the
      level is raised. 33,024 such reads in each of the three runs; none
      faulted. Every descriptor list said its pages were locked, and every
      page read as zero.
    - No lock failed, and at the time the device was removed every section
      had been released and as many pages unlocked as locked (8 sections
      and 32,896 pages in each of the runs with `kill`, in which hosts are
      killed while clients are streaming).
    - The streaming scenarios pass only if the host sees what the driver
      writes through its own mapping, so that mapping is of the section's
      pages and of no copy.

    Not checked: that nothing is ever written to the paging file, and that
    the pages stay locked between the checks. Both are what Microsoft
    documents a locked descriptor list to mean.

63. **[sections 6 and 11] Views in other processes; what the lock belongs
    to; where it ends.** The driver locks no view into any process's working
    set: section 6 leaves that to the host. The locks belong to the section
    as the driver holds it and to nothing else, so they are taken once and
    last until the section is released by the rule of section 11: the
    control handle closed and the last endpoint gone. A host that dies with
    clients streaming changes nothing about them. Its handle closes, its
    endpoints are withdrawn, and the clients' streams go on working on
    memory that is still locked until the last of them closes its filter;
    then the pages are unlocked, the driver's mapping removed and the
    section closed (the counts in item 62 are from such runs).

    Where that happens changed. The last reference to an endpoint, and with
    it to its section, usually goes when kernel streaming frees a factory's
    block, inside its close of the endpoint's last filter (item 44), with
    the device mutex held. Freeing a block took 0.8 ms on average and up to
    5.6 ms in the runs of item 55, more than anything the driver itself now
    does under that mutex when a host closes, and unlocking a large section
    would add to it. Resolved: the free routine does not free. It hands the
    block to a system worker thread, through a work item allocated with the
    block, which also keeps the device and the driver in place until the
    block is freed. So "released once ... the last filter and pin opened
    from it has been closed" now means shortly after, on another thread.

64. **[sections 5 and 14] "Start registry filter" without a lock.** The
    request held a lock while it read the caller's user and registered the
    callback. Now the first request stakes a claim (one atomic flag), does
    both with nothing held, and gives the claim up again if either fails. A
    request that arrives while another is still registering the callback is
    answered `STATUS_RESOURCE_IN_USE`, as if it had come afterwards; if the
    first then fails, the second was told "in use" about a filter that was
    never started. [guess] that this is acceptable: the registration fails
    for lack of memory or because another driver has the altitude, and then
    the second would have failed too.

65. **[section 14] What is done under each of the driver's own locks now.**
    Reads and writes of the driver's own memory, of the register table
    (locked), and of the request and reply buffers kernel streaming hands a
    property handler; allocation of non-paged pool for the lists the locks
    guard; nothing else. None of the calls section 14 lists
    is made under one, and neither is a wait, the completion of a request
    or the release of a reference whose going could free something: each of
    those is done after the lock has been dropped. The device mutex is not
    the driver's lock; what the driver does under it is in item 55.

66. **[section 14] The kind of lock.** Section 14 first left it open; the
    project owner then decided it, and the section now says: fast mutexes
    wherever possible. Every lock of the driver's own is one now: the
    control handle's, the endpoint's, the section's cell allocator's, the
    publish queue's, the list of identities, the control interface's slot
    and the registry filter's slot. None had to stay a kernel mutex,
    because by item 65 nothing done under any of them needs
    `PASSIVE_LEVEL`. Where two are held at once (an endpoint's, then its
    section's cell allocator's) they are released in the reverse order,
    which a lock that restores the IRQL on release needs. The notification
    queue keeps its spin lock, and the device mutex and the filter control
    mutex are kernel streaming's.

    `ExInitializeFastMutex` is an inline function of `wdm.h`; the wrapper
    stores what it stores. A fast mutex cannot be taken twice by one
    thread (the second attempt waits for ever), so the wrapper's refusal
    of recursive locking now works by knowing which thread holds the lock,
    and panics before it would wait.

    **[rig]** Two full runs of the suite on Windows 11 with the locks as
    fast mutexes, under Driver Verifier with its IRQL checking on:
    `cleanroom-rt-fm1-20261111-234630` (an experiment, before the decision)
    and `cleanroom-rt-f64-20261112-011459` (the committed code). Neither
    stopped or hung the machine, and conformance, fuzzing, contention and
    the unload passed in both. The scenarios that failed are those of item
    67, which fail the same way with kernel mutexes.

67. **[section 14] What the `contention` scenario measured, and what the
    rest of the suite did.** **[rig]** Windows 11, longest open and close
    of the first host's filter, in milliseconds (median in brackets):

    | Run | while creating | while closing |
    |---|---|---|
    | Before (`cleanroom-rt-base-20261111-222712`) | open 100.72 (39.51), close 109.29 (44.87) | open 357.44, close 0.76 |
    | Before, again, later the same night (`cleanroom-rt-old-20261112-003428`) | open 178.12 (68.65), close 224.62 (80.72) | open 241.18, close 131.43 |
    | Committed code (`cleanroom-rt-f64-20261112-011459`) | open 74.87 (0.15), close 88.15 (0.22) | open 0.66, close 1.16 |

    What is left while a host is created is one `KsCreateFilterFactory`,
    which has to be made under the device mutex and took 20 ms on average
    and up to 74 ms in the runs with timers. On Windows 10 a build with
    timers (`cleanroom-rt-a4w10-20261111-232928`, kernel mutexes, before the
    order of withdrawal was changed) measured open 28.23 and close 23.75
    while creating, open 1.12 and close 0.99 while closing.

    The suite as a whole did not pass. In run f64 six streaming scenarios
    failed (`run-16x2`, `kill-host`, `channels-32`, `run-after-fuzz`,
    `run-after-reload`, `run-after-device-disable`), all in the same way:
    Windows' own calls (`Activate`, `Initialize`, `GetMixFormat`) took 4 s
    or a multiple of it and the stream did not start in its window, or the
    endpoints were not built in time. The driver as it was before this work,
    run in the same guest between two runs of the new one (run old above),
    failed four the same way (`channels-32` and the three `run-after-*`),
    and its own numbers were twice what they had been two hours earlier. So
    those four are not this change's. `run-16x2` failed in all four full
    runs of the new code and passed in the one of the old; `kill-host`
    failed in three of the four. Those two are probably this change's,
    though not through anything the driver gets wrong that was found: with
    Windows no longer held up while a host starts and stops, starting 32
    endpoints took 1 to 2 s where it took 0.4 to 0.9 s, the audio service
    works at the same time, and the scenarios' windows are missed. Not
    settled.

68. **[sections 6 and 14] Not settled, or not exercised.**

    - The order in which kernel streaming calls the stop callbacks and
      frees the factories, on versions of Windows other than the rig's two
      (item 56).
    - A device that is stopped while a host is creating, publishing or
      withdrawing endpoints: the gate of item 56 has only ever been shut
      with nobody inside.
    - A host that closes its handle from one thread while another is in
      "create endpoint", and a new host that asks for an identity whose
      old factories are still being deleted (item 57). In the runs with
      timers no wait for an endpoint's factories lasted longer than
      0.05 ms: none had to wait.
    - A second "start registry filter" while the first is in progress
      (item 64).
    - Memory that cannot be locked (item 61): no run produced the failure.
      The largest layout a scenario asks for is not known; 16 MiB sections
      were locked in every run.
    - How long closing a host takes. Withdrawing 64 endpoints took up to
      1.6 s in the runs with timers (0.35 s before), almost all of it in
      switching interfaces off, a call that took four to five times longer
      than it did when everything else on the device was held up for its
      duration. The time is no longer spent with the device mutex held,
      which is what section 14 asks for; that it is longer in all was not
      pursued.
    - Freeing a factory's block on a worker thread (item 63) means that an
      endpoint and its section outlive the last close of their filters by
      the time a work item takes to run. Nothing is known to depend on the
      old timing; the `unload` scenarios pass.
