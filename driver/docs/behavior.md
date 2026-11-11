<!-- Copyright (c) 2026 Itty Bitty Binaries, Inc. SPDX-License-Identifier: MIT -->

# SynchronousAudioRouter kernel driver: behavioral specification

This document says what `SynchronousAudioRouter.sys` does, in plain English:
the objects it exposes, the requests it accepts and how it answers them, and
how it sits on top of Windows kernel streaming.

It describes behavior, not code. Nothing here depends on how the driver is
organized inside: a different implementation that behaves as described is the
same driver as far as the rest of SAR and Windows can tell. Internal names are
not used.

Sections 1 to 13 explain what the driver does. Appendix A is the binary
interface: every request code, structure offset, status value, descriptor and
registry value another implementation has to reproduce exactly to be a drop-in
replacement.

It describes the 0.14.0 development tree as of November 2026, including the
hardening changes. Where today's behavior looks unintended, the body states
what happens and section 13 lists it as a known gap.

## Contents

1. What the driver is for
2. Terms
3. Footprint on the system
4. Architecture
5. The control interface
6. The shared section
7. Endpoints as Windows sees them
8. Streams
9. Notification events, end to end
10. Application routing support
11. Teardown
12. Limits and constants
13. Known gaps and quirks

Appendix A. Binary interface

## 1. What the driver is for

SAR lets a digital audio workstation (DAW) exchange audio with ordinary
Windows applications in step with its own ASIO processing cycle. The driver's
part is small and specific:

- It lets one user-mode program, the **host**, create virtual Windows audio
  endpoints at run time and remove them again.
- It gives the host and each application using an endpoint a shared piece of
  memory to pass audio through, and a small shared table of registers to
  coordinate with.
- It passes the notification events that applications register on to the host,
  so that the host can wake them.
- Optionally, it steers selected processes towards SAR's replacement for the
  Windows audio device enumerator, which is how per-application routing works.

The driver never touches audio samples. It does no mixing, resampling, format
conversion, gain or timing. Audio moves between the host and applications
entirely through shared memory, and the host is the clock. This is a design
rule: no feature of the driver may read or change sample values.

## 2. Terms

- **Host.** The program that opens the control interface and owns the
  endpoints. In practice this is `SarAsio.dll` loaded into a DAW.
- **Client.** A process that opens a stream on an endpoint. In practice this
  is the Windows audio engine (`audiodg.exe`) for shared-mode audio, or an
  application using exclusive mode.
- **Endpoint.** One virtual audio device, either playback (applications play
  into it, the host reads) or recording (the host writes, applications
  capture).
- **Stream.** One open instance of an endpoint's audio pin. An endpoint has at
  most one stream at a time.
- **Section.** The shared memory the host creates through the driver. It holds
  the audio for all of that host's endpoints, followed by the register table.
- **Cell.** A 65,536-byte unit of the section. Audio buffers are handed out in
  whole cells.
- **Register table.** The last cell of the section: one fixed-size record per
  endpoint.
- **Period.** The amount of audio the host processes per cycle.
- **Frame.** One sample for every channel of a stream.

## 3. Footprint on the system

### Device

The driver is installed for a single root-enumerated software device with the
hardware ID `SW\{0BCFFA5C-E754-48CF-A783-A64C0DC0BB2C}`, in the `MEDIA` setup
class. The installer creates that device node; the driver does not create
devices of its own.

The service is named `SynchronousAudioRouter`, is a kernel driver, and is
started on demand by Plug and Play when the device starts. The INF pulls in
the standard kernel streaming registrations (`ks.inf`, `kscaptur.inf`).

The driver requires Windows 10 version 2004 or later.

### Device interfaces

While the device is started, the driver publishes one device interface for
control:

- Interface class `{C16E8D6C-C4CC-4C76-B11C-79B8414EA968}`
- Reference string `{0EB287D4-6C04-4926-AE19-3C066A4C3F3A}`

The host finds the control interface by enumerating that interface class.

Each endpoint adds further device interfaces in the standard kernel streaming
audio categories (section 7). Those exist only while the endpoint does.

### Load and unload

On load the driver registers with kernel streaming as a streaming device
driver and opts in to non-executable pool memory. It builds the fixed list of
registry redirections used by application routing (section 10) but does not
start filtering.

On unload it stops registry filtering if it was started and releases what it
holds. Unloading with endpoints in existence is not something the driver
arranges for; it happens only when the device is stopped or removed.

## 4. Architecture

### Layering on kernel streaming

The driver is an AVStream minidriver: it hands Windows kernel streaming (KS) a
device description and lets KS own the device object, Plug and Play, power and
the dispatch of KS requests. Everything Windows audio sees of SAR is a KS
filter, and all of it is created dynamically.

The driver puts itself in front of KS for four kinds of request on the device:
open, cleanup, close and device control. For each one it looks at the name the
file was opened with:

- If the name is the control reference string, the request belongs to the
  control interface and the driver handles it itself.
- Otherwise the request is passed to KS unchanged.

So one device object carries two unrelated interfaces: SAR's private control
interface, and any number of KS filters.

```
            host (DAW + SarAsio)                 clients (audio engine, apps)
                 |        ^                              |
   control requests       |  shared section              |  KS property requests
                 v        |  (audio + registers)         v
        +---------------------------------------------------------------+
        |  control interface   |        kernel streaming (AVStream)     |
        |                      |   wave filter + topology filter        |
        |                      |   per endpoint                         |
        +---------------------------------------------------------------+
                         SynchronousAudioRouter device
```

### Endpoints are pairs of KS filters

Each endpoint is two KS filter factories created when the host asks for the
endpoint and deleted when it goes away:

- a **wave filter**, which has the streaming pin clients open, and
- a **topology filter**, which represents the "jack" and gives the endpoint
  its name and its place in the audio device graph.

The two are tied together by a physical connection, the standard way of
telling Windows that one filter's bridge pin is wired to another's.

### No audio passes through KS

The streaming pin uses the looped-streaming interface and the WaveRT property
set. A client asks the pin for a buffer and gets a piece of the shared section
mapped into its own address space; from then on it reads or writes audio there
directly. KS stream requests carry nothing, and the driver does no processing
when the pin is asked to process.

### What ties host and clients together

Three things, all set up by the driver and then used without it:

- the **audio buffers**, regions of the section mapped into both the host and
  the client;
- the **register table**, mapped into both, through which the driver tells the
  host where each stream's buffer is and whether it is running, and through
  which the host publishes its position in each buffer;
- the **notification events**, created by clients and delivered to the host
  through a queue on the control interface.

## 5. The control interface

### Opening and access

The host opens the control interface by its device interface path. Each open
is independent: it gets its own section, its own endpoints and its own
notification queue. More than one may be open at once, from the same or
different processes.

The driver makes no access decision of its own. Who may open the control
interface is decided by the security on the device, and every control request
requires a handle opened for both reading and writing data.

### Lifetime

Everything a control handle created ends when the last handle to that open is
closed (section 11). A host that exits or crashes therefore takes its
endpoints with it.

Requests that arrive after the handle has started closing fail with
`STATUS_FILE_CLOSED`. A request already in progress when the handle closes
either finishes normally or fails with that status; it never leaves anything
behind that outlives the handle.

### Common rules

Requests are device control requests with these codes:

| Request | Code | Buffering |
|---|---|---|
| Set buffer layout | `0x22C007` | neither |
| Create endpoint | `0x22C00B` | neither |
| Wait for notification events | `0x22C00C` | buffered |
| Start registry filter | `0x22C013` | neither |

For requests with an input structure, an input buffer shorter than the
structure fails with `STATUS_BUFFER_OVERFLOW`. Input is read from the caller's
buffer with the usual checks, and an invalid address fails the request with
the exception's status.

For requests with an output structure, a zero-length output buffer fails with
`STATUS_BUFFER_OVERFLOW` and one that is too short with
`STATUS_BUFFER_TOO_SMALL`; in both cases the required size is reported.

All structures are packed as natural 32-bit fields, little-endian. Pointers
and handles in them are always 64 bits wide, so 32-bit and 64-bit hosts use
the same layout. Appendix A gives every offset, the exact byte counts and
statuses each request reports, and how its buffers are passed.

### Set buffer layout

Creates the section and fixes the audio format for every endpoint this control
handle will create. It can succeed once per control handle.

Input (20 bytes):

| Field | Size | Meaning |
|---|---|---|
| `bufferSize` | 4 | Bytes of audio buffer space wanted in total |
| `periodSizeBytes` | 4 | Bytes of one channel's audio in one period |
| `sampleRate` | 4 | Samples per second |
| `sampleSize` | 4 | Bytes per sample |
| `minimumFrameCount` | 4 | Fewest periods a client's buffer may hold |

Output (16 bytes):

| Field | Size | Meaning |
|---|---|---|
| `virtualAddress` | 8 | Where the section is mapped in the calling process |
| `actualSize` | 4 | Size of the whole section in bytes |
| `registerBase` | 4 | Offset of the register table within the section |

The request is refused with `STATUS_INVALID_PARAMETER` unless all of these
hold:

- `bufferSize` is between 1 and 134,217,728 (128 MiB)
- `sampleSize` is between 1 and 4
- `sampleRate` is between 8,000 and 192,000
- `periodSizeBytes` is between 1 and `bufferSize`

`minimumFrameCount` is accepted as given.

On success:

- The audio area is `bufferSize` rounded up to a whole number of memory pages.
- The section is the audio area followed by one cell for the register table.
  `registerBase` is therefore the size of the audio area, and `actualSize` is
  `registerBase` plus 65,536.
- The whole section is mapped, readable and writable, into the calling
  process, and the mapping's address is returned.
- The section starts zeroed.

A second request on the same control handle fails with
`STATUS_INVALID_STATE_TRANSITION` and changes nothing.

The audio format is integer PCM. `sampleSize` of 2 means 16-bit samples, 3
means 24-bit and so on. Every endpoint of this control handle uses this sample
rate and sample size.

### Create endpoint

Creates one endpoint and makes it visible to Windows. The buffer layout must
have been set first; otherwise the request fails with
`STATUS_INVALID_STATE_TRANSITION`.

Input (268 bytes), no output:

| Field | Size | Meaning |
|---|---|---|
| `type` | 4 | 1 for recording, 2 for playback |
| `index` | 4 | The endpoint's slot in the register table |
| `channelCount` | 4 | Number of channels |
| `id` | 128 | Identifier, UTF-16, up to 63 characters and a terminator |
| `name` | 128 | Display name, UTF-16, up to 63 characters and a terminator |

The request is refused with `STATUS_INVALID_PARAMETER` unless `type` is 1 or
2, `index` is below 2,340, and `channelCount` is between 1 and 32. The driver
treats character 64 of `id` and `name` as a terminator whatever it holds.

The request completes asynchronously. The caller sees it as pending until the
endpoint's device interfaces have been set up and switched on, and then it
completes with success, or with the failure that stopped it, in which case no
endpoint exists. Several requests may be outstanding at once; they are
finished in the order they were made.

On success the endpoint exists as described in section 7 until the control
handle is closed. There is no request to remove a single endpoint.

The driver does not check that `index` is unused. Two endpoints with the same
index would share a register record; hosts must not do that.

### Wait for notification events

Collects notification events that clients have registered on this control
handle's endpoints (section 9).

No input. Output is an array of as many 16-byte records as the caller's buffer
holds:

| Field | Size | Meaning |
|---|---|---|
| `handle` | 8 | A handle to the event, valid in the calling process |
| `associatedData` | 8 | Low 32 bits: the stream's generation value. High 32 bits: the endpoint's index |

Behavior:

- If the output buffer cannot hold one record, the request fails with
  `STATUS_BUFFER_TOO_SMALL`.
- If events are waiting, the request completes at once with as many as fit,
  oldest first. The number of bytes returned says how many records were
  filled.
- If none are waiting, the request stays pending. When a client next registers
  an event, the oldest pending request is completed successfully with zero
  bytes. That is a wake-up, not a delivery: the host issues the request again
  to collect the event.
- A pending request can be cancelled, and completes with `STATUS_CANCELLED`.
- When the control handle closes, pending requests complete with
  `STATUS_CANCELLED`, and events nobody collected are dropped.

Each returned handle is new, is created in the process that made this request,
and allows signalling and waiting on the event. The host owns it and must
close it.

If one event in a batch cannot be turned into a handle, that event is dropped
and the others are still delivered.

The two-step shape (wake, then collect) exists so that a handle is only ever
created on a thread of the process that will own it.

### Start registry filter

Turns on application routing support (section 10) for the user the calling
process runs as. No input, no output.

It succeeds at most once for as long as the driver stays loaded. Later
requests, from any control handle, fail with `STATUS_RESOURCE_IN_USE`.

### Anything else

A device control request with an unrecognized code on a control handle
completes successfully and does nothing.

## 6. The shared section

### Layout

```
offset 0                                registerBase        registerBase + 65536
|---------------- audio area ----------------|---- register table ----|
   cell 0 | cell 1 | cell 2 | ...                one record per endpoint
```

The audio area is divided into cells of 65,536 bytes. A stream's buffer
occupies a run of consecutive cells. Cells are handed out when a stream asks
for its buffer and returned when the stream closes.

Only whole cells can be handed out. If the audio area is not a multiple of the
cell size, the remainder at its end is never used, and an audio area smaller
than one cell cannot hold any stream's buffer.

The register table is 2,340 records of 28 bytes. Record *n* belongs to the
endpoint created with `index` *n*.

### Who has it mapped

- The host has the whole section mapped, from the moment it sets the buffer
  layout until its control handle closes.
- A client process has the register table mapped while it has a stream open on
  any endpoint of that section, and additionally the cells of its own stream's
  buffer once it has asked for the buffer.

Both mappings are readable and writable. A client can therefore write to any
endpoint's register record, not only its own. Nothing read from the register
table may be trusted: the host must check every value against the section's
real bounds before using it, and the driver never uses register contents to
make a memory access.

### Register record

| Field | Size | Written by | Meaning |
|---|---|---|---|
| `generation` | 4 | driver | Bit 0: the stream is running. Bits 1 to 31: a counter that changes whenever the stream changes |
| `positionRegister` | 4 | host; driver resets it | The host's position in the stream's buffer, in bytes |
| reserved | 4 | nobody | Unused |
| `bufferOffset` | 4 | driver | Where the stream's buffer starts, in bytes from the start of the section |
| `bufferSize` | 4 | driver | Size of the stream's buffer in bytes |
| `notificationCount` | 4 | driver | How many notifications per pass of the buffer the client asked for |
| `activeChannelCount` | 4 | driver | Channels in the open stream |

### What the driver writes, and when

| Moment | Effect on the endpoint's record |
|---|---|
| Stream opened | Counter in `generation` goes up by one, running bit clear; `activeChannelCount` set |
| Buffer allocated | `bufferOffset`, `bufferSize` and `notificationCount` set |
| Stream starts running | Running bit set |
| Stream stops running | Running bit clear |
| Stream enters or leaves the stopped state | `positionRegister` set to 0 |
| Stream closed | Counter goes up by one, running bit clear |

Whenever the driver updates a record it writes the other fields first and
`generation` last, as a single atomic store. A reader that sees the same
`generation` before and after reading the other fields has a consistent set.

### What the host is expected to do

This is the host's side of the contract, given here because the layout exists
to serve it.

Once per period, for each endpoint, the host reads the record. If the running
bit is clear, or any value is out of bounds, it treats the endpoint as silent.
Otherwise it copies one period of audio (`periodSizeBytes` times
`activeChannelCount` bytes) out of or into the stream's buffer at
`positionRegister`, wrapping at `bufferSize`. It then reads `generation`
again; if it changed, the stream was replaced mid-copy and the data is
discarded. Otherwise it advances `positionRegister` by one period, and signals
the stream's notification event when the position crosses the end of the
buffer, and also when it crosses the middle if `notificationCount` is 2.

## 7. Endpoints as Windows sees them

### Identity

Windows identifies an endpoint by the reference string of its wave filter,
which the driver builds as

```
<id>_<channelCount>_<sampleRate>_<sampleSize>
```

so an endpoint whose channel count, sample rate or sample size changes is a
different endpoint to Windows. This is deliberate: Windows caches an
endpoint's format and does not reliably notice a change. The topology filter's
reference string is the same with `_topology` appended.

### Device interfaces and their properties

The wave filter is registered in the KS categories for audio, real-time audio,
and render (playback) or capture (recording). The topology filter is
registered in audio and topology.

Before switching the interfaces on, the driver writes these values under each
interface's registry key:

| Value | Type | Data |
|---|---|---|
| `FriendlyName` | string | The endpoint's `name` |
| `CLSID` | string | The KS proxy class, `{17CCA71B-ECD7-11D0-B908-00A0C9223196}` |
| `MSEP\0\{1DA5D803-D492-4EDD-8C23-E0C0FFEE7F0E},7` | number | 1: event-driven streaming is supported |
| `MSEP\0\{1DA5D803-D492-4EDD-8C23-E0C0FFEE7F0E},2` | string | The all-zero GUID: no jack association |
| `MSEP\0\{a45c254e-df1c-4efd-8020-67d146a850e0},2` | string | The endpoint's `name`, so Windows shows it and not a generic one |
| `MSEP\0\{F4B15B6F-8C3F-48B6-A115-42FDE19EF05B},0` | string | The endpoint's `id` exactly as the host gave it |

The last value is SAR's own. It lets user-mode code map a Windows endpoint
back to the identifier in the host's configuration.

If the jack association or the description cannot be written, the endpoint is
still created. A failure to write any of the other values fails the
endpoint's creation.

### Wave filter

Pins:

| Pin | Role | Direction | Formats |
|---|---|---|---|
| 0 | Streaming pin clients open | In for playback, out for recording | Integer PCM, exactly the layout's sample rate and bit depth, 1 to `channelCount` channels |
| 1 | Bridge to the topology filter | Opposite of pin 0 | Analog |

Pin 0 accepts one instance at a time, has a fixed format once opened, uses the
looped-streaming interface, and is marked as a renderer on playback endpoints.

There is one node between the pins: a digital-to-analog converter on playback
endpoints, an analog-to-digital converter on recording ones.

Properties on the filter (standard pin property set):

| Property | Behavior |
|---|---|
| Global instance count | For pin 0: one possible, and one current while a stream is open. For other pins: none |
| Proposed data format, get | For pin 0: the default format (below) |
| Proposed data format, set | For pin 0: accepts a format the pin could be opened with, refuses others with `STATUS_NO_MATCH` |
| Physical connection | For pin 1: the topology filter's interface path, and its pin 0 |

Property on the node (standard audio property set):

| Property | Behavior |
|---|---|
| Channel configuration, get | The endpoint's current speaker mask |
| Channel configuration, set | Stores a new speaker mask if it names no more speakers than the endpoint has channels; otherwise `STATUS_NOT_SUPPORTED` |

The speaker mask starts as zero ("direct out", no positions). It is kept per
endpoint, not per stream, and lasts as long as the endpoint.

The **default format** is PCM in the extensible wave format at the layout's
rate and bit depth. Its channel count is the number of speakers in the mask,
or `channelCount` when the mask is zero.

The filter and pin 0 both let a caller subscribe to the standard "pin
capabilities changed" event. The driver never raises it.

### Topology filter

Pins:

| Pin | Role | Direction |
|---|---|---|
| 0 | Bridge to the wave filter | In for playback, out for recording |
| 1 | The jack: a speaker on playback endpoints, a line connector on recording ones | Opposite of pin 0 |

Between them are a volume node and a mute node, in that order in the direction
of signal flow. They support no properties, so neither volume nor mute can be
read or set through the driver. Because the driver never changes sample
values, they never will: they describe controls the endpoint does not have.

Properties on the filter (standard pin property set):

| Property | Behavior |
|---|---|
| Pin name | For pin 1: the endpoint's `name` |
| Physical connection | For pin 0: the wave filter's interface path, and its pin 1 |

### Format matching

When Windows asks the streaming pin which format lies in both the pin's range
and a range the caller offers, the driver answers with extensible PCM:

- sample rate and bit depth: the smaller of the caller's maximum and the
  layout's value, provided the ranges overlap at all;
- channels: the number of speakers in the endpoint's mask, or `channelCount`
  when the mask is zero, reduced to the caller's maximum if that is lower;
- no speaker positions.

If the ranges do not overlap, or the caller's range is not an audio range, the
answer is `STATUS_NO_MATCH`.

## 8. Streams

### Opening

A client opens a stream by creating an instance of the wave filter's pin 0
with a format. The format must be extensible PCM at exactly the layout's
sample rate and bit depth, with no more channels than the endpoint has;
otherwise the open fails with `STATUS_NO_MATCH`. Fewer channels than the
endpoint has is allowed.

Only one stream may be open on an endpoint. A second open fails with
`STATUS_RESOURCE_IN_USE`.

On a successful open the driver:

- maps the register table into the opening process, if it is not already
  mapped there for this endpoint;
- records the stream's channel count, taken from the format;
- updates the endpoint's register record as in section 6.

### WaveRT properties on the stream

| Property | Behavior |
|---|---|
| Buffer | Allocates the stream's buffer (below). Notification count is 0 |
| Buffer with notification | The same, with the caller's notification count |
| Position register | Gives the address, in the caller's process, of this endpoint's `positionRegister`. Width 32 bits. Accuracy: one period of this stream, in bytes |
| Hardware latency | All zero |
| Query notification support | Yes |
| Register notification event | Queues the caller's event for the host (section 9) |
| Unregister notification event | `STATUS_NOT_IMPLEMENTED` |
| Clock register | `STATUS_NOT_IMPLEMENTED` |
| Packet count | `STATUS_NOT_IMPLEMENTED` |
| Presentation position | `STATUS_NOT_IMPLEMENTED` |

### Allocating the buffer

The client asks for a buffer of some size and may not ask for a particular
address (`STATUS_NOT_IMPLEMENTED` if it does).

The size of the buffer is the larger of

- the size the client asked for, and
- `minimumFrameCount` × `periodSizeBytes` × the stream's channel count,

rounded up to a whole number of frames. The memory set aside for it is that
size rounded up to whole cells.

The request fails with `STATUS_INVALID_PARAMETER` if the buffer would be
empty, and with `STATUS_INSUFFICIENT_RESOURCES` if it would not fit in the
audio area or no long enough run of free cells exists. Every step of the size
calculation is done without wrapping: a request that is too large is refused,
never reduced to a small number.

On success the driver takes the first run of free cells that is long enough,
maps exactly those cells, readable and writable, into the calling process,
updates the register record, and returns the buffer's size and address. The
client is told it need not issue a memory barrier.

If any later step fails, the cells are returned and the mapping is removed
before the failure is reported.

### State changes

The stream moves between the standard KS states (stop, acquire, pause, run).
The driver reacts only by updating the register record:

- entering run sets the running bit; leaving run clears it;
- entering or leaving stop resets the position to zero.

The counter in `generation` does not change on state changes.

### Closing

When the stream is closed the driver updates the register record, removes the
register-table and buffer mappings from every process that had them for this
endpoint, returns the buffer's cells, and makes the endpoint available for a
new stream.

## 9. Notification events, end to end

1. A client that wants to be woken as audio is consumed creates an event and
   registers it on its stream.
2. The driver resolves the client's handle to the event object at that moment,
   with the client's own rights, requiring that the client may signal it. A
   handle that does not name such an event fails the request. The driver keeps
   the event itself, not the handle value.
3. The driver queues the event for the host together with the endpoint's index
   and the stream's current `generation` value. If the host's control handle
   has already closed, the registration fails with `STATUS_CANCELLED`.
4. If a wait request from the host is pending, it is completed empty as a
   wake-up.
5. The host issues a wait request and receives a handle to the event in its
   own process, with the index and generation.
6. From then on the host signals the event itself, as described in section 6.
   The generation tells it which stream the event belongs to, so that an event
   from a stream that has since closed is not used for its successor.

The driver never signals the event and never creates a handle in a process
other than the one making a request.

## 10. Application routing support

### What it is for

SAR can route chosen applications to chosen endpoints regardless of the
Windows default device. That is done in user mode, by a replacement for the
Windows audio device enumerator that `SarAsio.dll` provides. The driver's part
is to get that replacement loaded into applications.

### What the driver does

Once the filter has been started, the driver watches registry reads system
wide and changes the answer to exactly two questions:

| Asked for | Answered with |
|---|---|
| The in-process server of the audio device enumerator class, `{BCDE0395-E52F-467C-8E3D-C4579291692E}` | That of SAR's wrapper class, `{9FB96668-9EDD-4574-AD77-76BD89659D5D}` |
| The in-process server of the audio interface activation class, `{E2F7A62A-862B-40AE-BBC2-5C0CA9A5B7E1}` | That of SAR's wrapper class, `{739191CC-CCBE-45D8-8D24-828D8E989E8E}` |

Precisely: it acts on a read of the default value of the `InprocServer32` key
of those classes under the machine-wide class registration, and on an
enumeration of that key's first value. A 64-bit process is matched on the
native registration; a 32-bit process on the 32-bit registration. Key names
are compared without regard to case. The answer is whatever the corresponding
value of the wrapper's key holds at that moment.

The answer is changed only when all of these are true:

- The reading process runs as the same user as the process that started the
  filter.
- The reading process is not restricted to loading only Microsoft-signed,
  Store-signed or similarly restricted images. Such a process could not load
  the wrapper and would lose audio altogether, so it is left alone. If the
  driver cannot find out, it treats the process as unrestricted.
- The wrapper's registry key can be opened. If SAR's user-mode part is not
  registered, reads pass through unchanged.

In every other case the read proceeds exactly as if the driver were not there.

If the wrapper's key opens but reading its value fails, for instance because
the caller's buffer is too small, the caller gets that failure, as it would
have from the real key.

### Lifetime

Filtering starts when a host asks for it and continues until the driver
unloads. It does not stop when that host exits, and the user it applies to
does not change.

## 11. Teardown

### The host closes its control handle, exits or crashes

When the last handle to a control open is closed:

1. New control requests on it fail with `STATUS_FILE_CLOSED`.
2. Each of its endpoints is withdrawn: its device interfaces are switched off,
   so Windows removes the endpoint, and it is marked as having no owner.
3. Pending wait requests are cancelled and uncollected events dropped.
4. The section is unmapped from the host, but only if the close is happening
   in the process that mapped it. If the handle was duplicated into another
   process and closed there, the mapping is left to go away with the process
   that owns it.

Streams that clients still have open on a withdrawn endpoint are not torn down
by the driver. The endpoint's filters, the section and the client's mappings
stay valid until the client closes them, which the Windows audio engine
normally does once the endpoint's interfaces disappear. Until then the stream
can still change state and be closed, but property requests on the withdrawn
endpoint's filters fail as if the endpoint were not found, and its filters
cannot be opened again. The memory and the endpoint are released when the last
such reference goes.

### A client exits or closes its stream

Closing the stream does everything in "Closing" in section 8. The host sees
the generation change and the running bit clear.

### What is released, and when

A host can start and stop any number of times between boots, and each time
has to leave nothing behind.

- **An endpoint.** Everything allocated for an endpoint, including the
  descriptors kernel streaming reads, is released once the endpoint has been
  withdrawn and the last filter and pin opened from it has been closed. It is
  not kept until the device stops.
- **A control open's section.** The section and the driver's own view of it
  are released once the control handle has closed and the last endpoint that
  used the section has been released.
- **Views in other processes.** The host's view goes as described above. A
  client's views go when its stream closes (section 8).

### The driver unloads

Registry filtering stops. Plug and Play does not unload the driver while its
device has open handles, so by then every control handle, filter and pin has
been closed, and by the rules above nothing is left: no pool memory, no
section, no mapped view, no registry callback. Driver Verifier's check for
leaked pool at unload has to pass whatever happened earlier, including a host
killed while clients were streaming.

## 12. Limits and constants

| Limit | Value |
|---|---|
| Audio area per control handle | 1 byte to 128 MiB, rounded up to pages |
| Cell size | 65,536 bytes |
| Register record size | 28 bytes |
| Endpoints per control handle | 2,340 (indexes 0 to 2,339) |
| Channels per endpoint | 1 to 32 |
| Sample size | 1 to 4 bytes |
| Sample rate | 8,000 to 192,000 Hz |
| Endpoint id and name | 63 UTF-16 characters each |
| Streams per endpoint | 1 |
| Registry filter altitude | 360000 |

## 13. Known gaps and quirks

Behavior that exists today and is probably not what a fresh design would
choose. An implementation written from this document should treat each as a
decision to make, not a requirement to copy.

- **Unknown control requests succeed.** They should fail.
- **A second buffer request on one stream.** The driver allocates again and
  forgets the first allocation: its cells stay taken until the control handle
  closes, and its mapping stays in the client until the client exits. One
  buffer per stream is the intent.
- **Unregistering a notification event is not implemented.** An event stays
  queued, or in the host's hands, until the stream's generation moves on.
- **The "capabilities changed" event is declared but never raised.**
- **Volume and mute nodes are declared but inert.** The driver does not change
  sample values by design, so they can never work. Whether the topology
  filter should declare them at all is open; what Windows does differently
  with and without them has not been tested.
- **Registry filtering cannot be stopped or handed to another user** short of
  unloading the driver.
- **Endpoint indexes are not checked for reuse.**
- **A failed reply to "set buffer layout" leaves the layout set.** If the
  layout was applied but the output buffer could not be written, the request
  fails and the section stays mapped, and the request cannot be repeated on
  that handle.
- **Clients can write the whole register table.** The host defends against
  that; the layout does not prevent it.
- **The section is created with a name** (`SynchronousAudioRouter_` and a
  random GUID, in the global named-object directory). Nothing opens it by
  name.
- **Other request types on a control handle** (read, write and so on) are
  passed to kernel streaming, which does not know the handle.
- **The INF offers the device on 32-bit x86** although only a 64-bit driver is
  built.
- **The shared header declares requests for an unrelated network filter**
  (`SarNdis`). The driver implements none of them.

## Appendix A. Binary interface

Everything a driver must match, bit for bit, to work with existing builds of
SAR's user-mode software and to look the same to Windows. Sections 5 to 10
say what each thing means; this appendix says exactly what goes over the wire.

Structure layouts and request codes were checked by compiling the shared
header for 32-bit and 64-bit hosts: they are identical for both. All integers
are little-endian. Values the Windows headers define (kernel streaming
identifiers, status codes) are referred to by their Windows names; values SAR
defines are given in full.

### A.1 Identification

| Item | Value |
|---|---|
| Hardware ID | `SW\{0BCFFA5C-E754-48CF-A783-A64C0DC0BB2C}` |
| Setup class | `MEDIA`, `{4d36e96c-e325-11ce-bfc1-08002be10318}` |
| Service name | `SynchronousAudioRouter` |
| Driver image | `SynchronousAudioRouter.sys` |
| Control interface class | `{C16E8D6C-C4CC-4C76-B11C-79B8414EA968}` |
| Control reference string | `{0EB287D4-6C04-4926-AE19-3C066A4C3F3A}` |

A file opened on the device is a control handle exactly when the file name the
driver receives is `\{0EB287D4-6C04-4926-AE19-3C066A4C3F3A}`, compared without
regard to case. That is the name an open of the control interface path
produces.

### A.2 Request codes

| Request | Code | Device type | Function | Method | Access |
|---|---|---|---|---|---|
| Set buffer layout | `0x0022C007` | `0x22` (unknown) | 1 | 3 (neither) | read and write data |
| Create endpoint | `0x0022C00B` | `0x22` | 2 | 3 (neither) | read and write data |
| Wait for notification events | `0x0022C00C` | `0x22` | 3 | 0 (buffered) | read and write data |
| Start registry filter | `0x0022C013` | `0x22` | 4 | 3 (neither) | read and write data |

Function 5 was used by releases before 0.14.0 and is retired. It must not be
given a new meaning.

### A.3 How buffers are passed

**Requests using method "neither" (1, 2 and 4).** The driver reads input
directly from the caller's input pointer and writes output directly to the
caller's output pointer, in the calling thread's context.

- The input length must be at least the size of the input structure. Bytes
  beyond it are ignored.
- The output length must be at least the size of the output structure. Space
  beyond it is untouched.
- Both pointers must be aligned to 4 bytes and lie in user address space. A
  misaligned pointer fails the request with `STATUS_DATATYPE_MISALIGNMENT`,
  an unreadable or unwritable one with `STATUS_ACCESS_VIOLATION`.

**The request using method "buffered" (3).** Output is returned through the
system buffer in the standard way. The input buffer is ignored. There is no
alignment requirement.

### A.4 Status codes

| Status | Value | Win32 error |
|---|---|---|
| `STATUS_SUCCESS` | `0x00000000` | 0 |
| `STATUS_PENDING` | `0x00000103` | 997 |
| `STATUS_OBJECT_NAME_EXISTS` | `0x40000000` | 698 |
| `STATUS_DATATYPE_MISALIGNMENT` | `0x80000002` | 998 |
| `STATUS_BUFFER_OVERFLOW` | `0x80000005` | 234 |
| `STATUS_UNSUCCESSFUL` | `0xC0000001` | 31 |
| `STATUS_NOT_IMPLEMENTED` | `0xC0000002` | 1 |
| `STATUS_ACCESS_VIOLATION` | `0xC0000005` | 998 |
| `STATUS_INVALID_PARAMETER` | `0xC000000D` | 87 |
| `STATUS_BUFFER_TOO_SMALL` | `0xC0000023` | 122 |
| `STATUS_INSUFFICIENT_RESOURCES` | `0xC000009A` | 1450 |
| `STATUS_NOT_SUPPORTED` | `0xC00000BB` | 50 |
| `STATUS_CANCELLED` | `0xC0000120` | 995 |
| `STATUS_FILE_CLOSED` | `0xC0000128` | 6 |
| `STATUS_NOT_FOUND` | `0xC0000225` | 1168 |
| `STATUS_NO_MATCH` | `0xC0000272` | 1169 |
| `STATUS_CALLBACK_BYPASS` | `0xC0000503` | 21 |
| `STATUS_RESOURCE_IN_USE` | `0xC0000708` | 1359 |
| `STATUS_INVALID_STATE_TRANSITION` | `0xC000A003` | 5023 |

### A.5 Control requests on the wire

"Information" is the byte count the driver reports when it completes the
request. Checks are made in the order listed; the first that applies decides
the outcome.

**Any control request**

| Condition | Status | Information | Completion |
|---|---|---|---|
| The control handle has started closing | `STATUS_FILE_CLOSED` | 0 | immediate |
| Unrecognized code | `STATUS_SUCCESS` | 0 | immediate |

**Set buffer layout** (`0x0022C007`). Always completes immediately.

| Condition | Status | Information |
|---|---|---|
| Input shorter than 20 bytes | `STATUS_BUFFER_OVERFLOW` | 0 |
| Input pointer unusable | see A.3 | 0 |
| A parameter out of range | `STATUS_INVALID_PARAMETER` | 0 |
| The section cannot be created or mapped | The memory manager's status, or `STATUS_INSUFFICIENT_RESOURCES` | 0 |
| The handle closed meanwhile | `STATUS_FILE_CLOSED` | 0 |
| Layout already set on this handle | `STATUS_INVALID_STATE_TRANSITION` | 0 |
| Output length 0 | `STATUS_BUFFER_OVERFLOW` | 16 |
| Output shorter than 16 bytes | `STATUS_BUFFER_TOO_SMALL` | 16 |
| Output pointer unusable | see A.3 | 16 |
| Otherwise | `STATUS_SUCCESS` | 16 |

In the last four rows the layout has already been applied (section 13).

Input structure, 20 bytes, alignment 4:

| Offset | Size | Type | Field |
|---|---|---|---|
| 0 | 4 | unsigned | `bufferSize` |
| 4 | 4 | unsigned | `periodSizeBytes` |
| 8 | 4 | unsigned | `sampleRate` |
| 12 | 4 | unsigned | `sampleSize` |
| 16 | 4 | unsigned | `minimumFrameCount` |

Output structure, 16 bytes, natural alignment 8 (only 4 is required of the
caller's pointer):

| Offset | Size | Type | Field |
|---|---|---|---|
| 0 | 8 | 64-bit address | `virtualAddress` |
| 8 | 4 | unsigned | `actualSize` |
| 12 | 4 | unsigned | `registerBase` |

`virtualAddress` is an address in the calling process. A 32-bit host uses its
low 32 bits; the high 32 are zero.

**Create endpoint** (`0x0022C00B`). No output buffer is used.

| Condition | Status | Information | Completion |
|---|---|---|---|
| Input shorter than 268 bytes | `STATUS_BUFFER_OVERFLOW` | 0 | immediate |
| Input pointer unusable | see A.3 | 0 | immediate |
| `type`, `index` or `channelCount` out of range | `STATUS_INVALID_PARAMETER` | 0 | immediate |
| No layout set | `STATUS_INVALID_STATE_TRANSITION` | 0 | immediate |
| Out of memory | `STATUS_INSUFFICIENT_RESOURCES` | 0 | immediate |
| Kernel streaming refuses either filter factory | Its status | 0 | immediate |
| The handle closed meanwhile | `STATUS_FILE_CLOSED` | 0 | immediate |
| Otherwise | `STATUS_PENDING`, then `STATUS_SUCCESS` or the status that stopped publication | 0 | later, on a system worker thread |

A request that passes the immediate checks always reports pending first, even
if publication would finish at once. Existing hosts issue it on an overlapped
handle without an overlapped structure and let Windows wait for it.

Input structure, 268 bytes, alignment 4:

| Offset | Size | Type | Field |
|---|---|---|---|
| 0 | 4 | unsigned | `type`: 1 recording, 2 playback |
| 4 | 4 | unsigned | `index` |
| 8 | 4 | unsigned | `channelCount` |
| 12 | 128 | 64 UTF-16 code units | `id` |
| 140 | 128 | 64 UTF-16 code units | `name` |

Each string ends at its first zero code unit. The driver sets the last code
unit of each array to zero before reading it, so a string can be at most 63
units. `id` becomes part of a device interface reference string and must not
contain `\` or `/`; the driver does not check.

**Wait for notification events** (`0x0022C00C`).

Let *capacity* be the output length divided by 16, rounded down.

| Condition | Status | Information | Completion |
|---|---|---|---|
| *capacity* is 0 | `STATUS_BUFFER_TOO_SMALL` | 16 | immediate |
| Events are queued | `STATUS_SUCCESS` | 16 × records filled | immediate |
| None queued, queue closed | `STATUS_CANCELLED` | 0 | immediate |
| None queued | `STATUS_PENDING`, then one of the two rows below | | |
| … a client registered an event | `STATUS_SUCCESS` | 0 | later |
| … cancelled, or the handle closed | `STATUS_CANCELLED` | 0 | later |

At most *capacity* events are taken per request. "Records filled" can be less
than the number taken, down to zero, if events could not be turned into
handles; filled records are always contiguous from the start of the buffer.

Output record, 16 bytes, natural alignment 8:

| Offset | Size | Type | Field |
|---|---|---|---|
| 0 | 8 | 64-bit handle value | `handle` |
| 8 | 8 | unsigned | `associatedData` |

`handle` is a handle in the calling process's handle table, not inheritable,
granted exactly the rights to signal the event and to wait on it. A 32-bit
host uses the low 32 bits. `associatedData` bits 0 to 31 are the endpoint's
`generation` register as it was when the client registered the event,
running bit included; bits 32 to 63 are the endpoint's index.

Existing hosts ask for 32 records at a time, with an overlapped structure, on
a handle bound to a completion port in "skip completion port on success" mode.
They treat a successful completion with zero bytes as the cue to ask again.

**Start registry filter** (`0x0022C013`). No buffers. Always completes
immediately.

| Condition | Status | Information |
|---|---|---|
| Already started since the driver loaded | `STATUS_RESOURCE_IN_USE` | 0 |
| The caller's user cannot be read, or the registry callback cannot be registered | That status | 0 |
| Otherwise | `STATUS_SUCCESS` | 0 |

### A.6 Section geometry

With `P` = 4,096 (the page size) and `C` = 65,536 (the cell size):

| Quantity | Value |
|---|---|
| Audio area size | `bufferSize` rounded up to a multiple of `P` |
| `registerBase` | Audio area size |
| `actualSize` (section size) | Audio area size + `C` |
| Usable cells | Audio area size ÷ `C`, rounded down |
| Cell *k* | Section offsets *k* × `C` to (*k* + 1) × `C` − 1 |
| Register record *n* | Section offset `registerBase` + 28 × *n*, for *n* from 0 to 2,339 |

The last 16 bytes of the register cell are unused.

A stream's buffer of *m* cells starting at cell *k* is mapped into the client
as one view of *m* × `C` bytes at section offset *k* × `C`. The host reaches
the same bytes at `virtualAddress` + *k* × `C`.

A view of a section has to start on a 64 KiB boundary, and `registerBase` is
only page-aligned, so a view of the register table cannot start at
`registerBase` in general. A client's view starts at `registerBase` rounded
down to a multiple of `C` and is (`registerBase` mod `C`) + `C` bytes long.
The register table begins (`registerBase` mod `C`) bytes into the view, and
the position register address reported to the client includes that offset.
When the audio area is not a multiple of `C`, the view therefore also covers
the tail of the audio area, which is never part of any stream's cells.

### A.7 Register record

28 bytes, alignment 4.

| Offset | Size | Type | Field |
|---|---|---|---|
| 0 | 4 | unsigned | `generation` |
| 4 | 4 | unsigned | `positionRegister` |
| 8 | 4 | | reserved |
| 12 | 4 | unsigned | `bufferOffset` |
| 16 | 4 | unsigned | `bufferSize` |
| 20 | 4 | unsigned | `notificationCount` |
| 24 | 4 | unsigned | `activeChannelCount` |

- `generation` = counter × 2 + running bit. The counter is 31 bits and wraps.
  A record that has never been used is all zero.
- `bufferOffset` = first cell × 65,536: an offset from the start of the
  section, always below `registerBase`.
- `bufferSize` is the size of the buffer, not of the cells holding it:
  `bufferOffset` + `bufferSize` never exceeds `registerBase`.
- `positionRegister` is a byte offset into the buffer, below `bufferSize`.

Each driver update is a read of the whole record followed by a write of
`positionRegister`, `bufferOffset`, `bufferSize`, `notificationCount` and
`activeChannelCount`, then a memory barrier, then one atomic store of
`generation`. The reserved field is never written. Because the update is a
read followed by a write, a position the host stores in between can be
overwritten with the value the driver read.

### A.8 Notification events

The handle a client passes when registering must name an event object and
allow "modify state" access in the client's own handle table; anything else
fails with the object manager's status. The host's handle (A.5) carries
"modify state" and "synchronize" access and nothing more.

### A.9 Endpoint filters

What each endpoint presents to kernel streaming. Sizes are for the 64-bit
driver.

**Reference strings.** Wave filter: `<id>_<channelCount>_<sampleRate>_<sampleSize>`,
numbers in decimal. Topology filter: the same followed by `_topology`. Both
factories are created to be freed when the device stops.

**Cached filter data.** Right after creating the two factories, and before
either one's device interfaces are switched on, the driver has kernel
streaming write each factory's cached filter data (the `FilterData` value and
the medium cache that Windows keeps with the factory's device interfaces)
from the factory's own descriptor: `KsFilterFactoryUpdateCacheData` with a
null descriptor, the wave factory first. The results are not checked: a
failure here does not fail the request.

**Wave filter** — categories `KSCATEGORY_AUDIO`, `KSCATEGORY_REALTIME`, and
`KSCATEGORY_RENDER` (playback) or `KSCATEGORY_CAPTURE` (recording).

| | Pin 0 | Pin 1 |
|---|---|---|
| Interfaces | `KSINTERFACESETID_Standard`, looped streaming | none |
| Mediums | none | none |
| Data ranges | the digital range | the analog range |
| Data flow | in (playback), out (recording) | out (playback), in (recording) |
| Communication | sink | none |
| Category | `KSCATEGORY_AUDIO` | `KSCATEGORY_AUDIO` |
| Name | none | none |
| Instances possible / necessary | 1 / 0 | 1 / 0 |
| Format matching handler | yes | no |

Flags on both pins: do not initiate processing, frames not required for
processing, process if any in run state, fixed format, do not use standard
transport. Pin 0 on playback endpoints adds renderer.

Digital range (`KSDATARANGE_AUDIO`, 88 bytes): format size 88, flags 0, sample
size 0; major format audio, subformat PCM, specifier `WAVEFORMATEX`; maximum
channels `channelCount`; minimum and maximum bits per sample both
`sampleSize` × 8; minimum and maximum sample frequency both `sampleRate`.

Analog range (`KSDATARANGE`, 64 bytes): format size 64, flags 0, sample size
0; major format audio, subformat analog, specifier none.

Node 0: `KSNODETYPE_DAC` (playback) or `KSNODETYPE_ADC` (recording).
Connections, playback: pin 0 → node 0 input; node 0 output → pin 1. Recording:
pin 1 → node 0 input; node 0 output → pin 0.

Properties. "Minimum" sizes are what kernel streaming enforces before the
driver sees the request.

| Where | Set | Property | Direction | Minimum request | Minimum value |
|---|---|---|---|---|---|
| Filter | `KSPROPSETID_Pin` | `GLOBALCINSTANCES` | get | 32 | 8 |
| Filter | `KSPROPSETID_Pin` | `PROPOSEDATAFORMAT` | get, set | 32 | 0 |
| Filter | `KSPROPSETID_Pin` | `PHYSICALCONNECTION` | get | standard | standard |
| Node 0 | `KSPROPSETID_Audio` | `CHANNEL_CONFIG` | get, set | 32 | 4 |
| Pin 0 | `KSPROPSETID_RtAudio` | `BUFFER` | get | 40 | 16 |
| Pin 0 | `KSPROPSETID_RtAudio` | `BUFFER_WITH_NOTIFICATION` | get | 40 | 16 |
| Pin 0 | `KSPROPSETID_RtAudio` | `POSITIONREGISTER` | get | 32 | 40 |
| Pin 0 | `KSPROPSETID_RtAudio` | `CLOCKREGISTER` | get | 32 | 40 |
| Pin 0 | `KSPROPSETID_RtAudio` | `HWLATENCY` | get | 24 | 12 |
| Pin 0 | `KSPROPSETID_RtAudio` | `QUERY_NOTIFICATION_SUPPORT` | get | 24 | 4 |
| Pin 0 | `KSPROPSETID_RtAudio` | `REGISTER_NOTIFICATION_EVENT` | get, set | 32 | 0 |
| Pin 0 | `KSPROPSETID_RtAudio` | `UNREGISTER_NOTIFICATION_EVENT` | get, set | 32 | 0 |
| Pin 0 | `KSPROPSETID_RtAudio` | `PACKETCOUNT` | get | 24 | 4 |
| Pin 0 | `KSPROPSETID_RtAudio` | `PRESENTATION_POSITION` | get | 24 | 16 |

Registering a notification event behaves the same whether it arrives as a get
or a set.

Events: `KSEVENTSETID_PinCapsChange`, `KSEVENT_PINCAPS_FORMATCHANGE`, on the
filter and on pin 0, minimum event data 32 bytes. Never raised.

Values returned:

| Property | Value |
|---|---|
| `GLOBALCINSTANCES` | Pin 0: possible 1, current 1 or 0. Any other pin: 0 and 0 |
| `PROPOSEDATAFORMAT`, get | Pin 0 only, else `STATUS_NOT_FOUND`. 104 bytes: the default format (below). Buffer of length 0: `STATUS_BUFFER_OVERFLOW`; shorter than 104: `STATUS_BUFFER_TOO_SMALL`; both report 104 |
| `PROPOSEDATAFORMAT`, set | Pin 0 only. Same length rules. `STATUS_SUCCESS` if the format is audio, PCM, `WAVEFORMATEX` specifier, extensible tag, PCM subformat, the layout's sample rate, bits per sample equal to `sampleSize` × 8 and at most `channelCount` channels; otherwise `STATUS_NO_MATCH` |
| `PHYSICALCONNECTION` | Pin 1 only, else `STATUS_NOT_FOUND`. `Size` = 12 + path bytes; `Pin` = 0; the topology filter's interface path, zero-terminated. Reports 12 + path bytes + 2. Length rules as above |
| `CHANNEL_CONFIG` | Node 0 only, else `STATUS_NOT_FOUND`. A 4-byte speaker mask. Buffer shorter than 4: `STATUS_BUFFER_TOO_SMALL` |
| `BUFFER`, `BUFFER_WITH_NOTIFICATION` | Actual buffer size, buffer address in the caller, memory barrier flag false |
| `POSITIONREGISTER` | Address of the record's `positionRegister` in the caller; width 32; numerator 0; denominator 0; accuracy `periodSizeBytes` × stream channels |
| `HWLATENCY` | FIFO size 0, chipset delay 0, codec delay 0 |
| `QUERY_NOTIFICATION_SUPPORT` | 4-byte true |

Default format, 104 bytes (`KSDATAFORMAT` followed by `WAVEFORMATEXTENSIBLE`):

| Field | Value |
|---|---|
| Format size | 104 |
| Flags | 0 |
| Sample size | block alignment |
| Major format, subformat, specifier | audio, PCM, `WAVEFORMATEX` |
| Format tag | `WAVE_FORMAT_EXTENSIBLE` (`0xFFFE`) |
| Channels | speakers in the endpoint's mask, or `channelCount` if the mask is 0 |
| Samples per second | `sampleRate` |
| Bits per sample, valid bits per sample | `sampleSize` × 8 |
| Block alignment | `sampleSize` × channels |
| Average bytes per second | block alignment × `sampleRate` |
| Extra size | 22 |
| Channel mask | the endpoint's mask |
| Extensible subformat | PCM |

The format returned by format matching is the same structure with the rate,
bit depth and channel count chosen as in section 7, and a channel mask of 0.

**Topology filter** — categories `KSCATEGORY_AUDIO`, `KSCATEGORY_TOPOLOGY`.

| | Pin 0 | Pin 1 |
|---|---|---|
| Interfaces, mediums | none | none |
| Data ranges | the analog range | the analog range |
| Data flow | in (playback), out (recording) | out (playback), in (recording) |
| Communication | none | none |
| Category | `KSCATEGORY_AUDIO` | `KSNODETYPE_SPEAKER` (playback), `KSNODETYPE_LINE_CONNECTOR` (recording) |
| Flags | 0 | 0 |
| Instances possible / necessary | 0 / 0 | 0 / 0 |

Node 0: `KSNODETYPE_VOLUME`. Node 1: `KSNODETYPE_MUTE`. Neither has
properties. Connections, playback: pin 0 → node 0 → node 1 → pin 1.
Recording: pin 1 → node 0 → node 1 → pin 0.

| Set | Property | Direction | Minimum request | Behavior |
|---|---|---|---|---|
| `KSPROPSETID_Pin` | `NAME` | get, set | 32 | Pin 1 only, else `STATUS_NOT_FOUND`. The endpoint's `name` in UTF-16 with a terminator. Reports name bytes + 2. Length 0: `STATUS_BUFFER_OVERFLOW`; too short: `STATUS_BUFFER_TOO_SMALL` |
| `KSPROPSETID_Pin` | `PHYSICALCONNECTION` | get | standard | Pin 0 only, else `STATUS_NOT_FOUND`. As on the wave filter, with the wave filter's interface path and `Pin` = 1 |

### A.10 Registry values per endpoint

Written to the parameters key of each interface instance before it is
switched on: the wave filter's instances in the audio, real-time and render
or capture categories, and the topology filter's in audio and topology.

| Key, relative to the instance's parameters key | Value name | Type | Data |
|---|---|---|---|
| (the key itself) | `FriendlyName` | `REG_SZ` | `name`, with terminator |
| (the key itself) | `CLSID` | `REG_SZ` | `{17CCA71B-ECD7-11D0-B908-00A0C9223196}`, with terminator |
| `MSEP\0` | `{1DA5D803-D492-4EDD-8C23-E0C0FFEE7F0E},7` | `REG_DWORD` | 1 |
| `MSEP\0` | `{1DA5D803-D492-4EDD-8C23-E0C0FFEE7F0E},2` | `REG_SZ` | `{00000000-0000-0000-0000-000000000000}`, with terminator |
| `MSEP\0` | `{a45c254e-df1c-4efd-8020-67d146a850e0},2` | `REG_SZ` | `name`, with terminator |
| `MSEP\0` | `{F4B15B6F-8C3F-48B6-A115-42FDE19EF05B},0` | `REG_SZ` | `id`, with terminator |

### A.11 Registry filter

Registered as a registry callback at altitude `360000`.

Keys matched, in kernel form, compared without regard to case. *G* is one of
the two class identifiers in section 10.

| Reading process | Key |
|---|---|
| 64-bit | `\REGISTRY\MACHINE\SOFTWARE\Classes\CLSID\`*G*`\InprocServer32` |
| 32-bit | `\REGISTRY\MACHINE\SOFTWARE\Classes\Wow6432Node\CLSID\`*G*`\InprocServer32` |

The substitute key is the same path with the wrapper's identifier in place of
*G*.

Operations acted on, before the registry performs them:

| Operation | Condition | Substitute |
|---|---|---|
| Query value | The value name is empty | The same query, same information class and buffer length, of the substitute key's default value |
| Enumerate value | The index is 0 | The same enumeration, index 0, of the substitute key |

On success the result and its length are written to the caller's buffers and
the operation is completed with `STATUS_CALLBACK_BYPASS`, which the registry
reports to the caller as success. If the substitute operation fails, its
status becomes the caller's result, with the result length filled in. If the
substitute key cannot be opened, or the process does not qualify, the callback
returns `STATUS_SUCCESS` and the registry carries out the original operation.
