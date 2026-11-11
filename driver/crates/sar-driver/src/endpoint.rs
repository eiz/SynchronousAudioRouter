// Copyright (c) 2026 Itty Bitty Binaries, Inc.
// SPDX-License-Identifier: MIT

//! One endpoint: its two filter factories, its stream, and the property
//! requests Windows makes on it (sections 7, 8 and 9).
#![forbid(unsafe_code)]

use alloc::vec::Vec;
use core::sync::atomic::{AtomicU32, Ordering};

use ks::{
    device::DeviceGuard,
    filter::{DataFormat, FilterFactory, Pin, PropertyCall, Target},
    guids, ids,
};
use ntk::{
    alloc_ext::Shared,
    interface::DeviceInterface,
    object::EventObject,
    registry::{RegistryKey, KEY_CREATE_SUB_KEY, KEY_SET_VALUE},
    section::UserView,
    sync::Mutex,
    Guid,
};
use ntstatus::{IoStatus, NtStatus};
use sar_core::{
    buffer::{position_accuracy, stream_buffer_geometry},
    cells::CellRun,
    endpoint::{
        reference_string, Direction, EndpointRequest, FilterKind, NameString, ReferenceString, WideString,
    },
    format::{self, EndpointFormat, PinAudioRange},
    layout::CELL_SIZE,
    notify::Notification,
    properties::{self, BufferRequest},
    registers::{Event, StreamState},
    registry::{InterfaceValue, ValueData, INTERFACE_VALUES, MSEP_SUBKEY_PATH},
};

use crate::{
    control::NotifyQueue,
    filters::{self, pin, same_guid, TopologyFilter, WaveFilter},
    section::SharedSection,
};

/// One open instance of the streaming pin.
struct Stream {
    /// Identifies the pin instance that owns the stream.
    pin_key: usize,
    /// Channels of the stream, from the format it was opened with.
    channels: u32,
    /// The register table as mapped into each process that needed it.
    register_views: Vec<UserView>,
    /// The stream's buffers: the cells and their mapping in the client.
    /// Normally one; see `allocate_buffer` for why it is a list.
    buffers: Vec<(CellRun, UserView)>,
}

struct EndpointState {
    /// The owning control handle's notification queue; `None` once the
    /// endpoint has been withdrawn ("marked as having no owner").
    owner: Option<Shared<NotifyQueue>>,
    wave: Option<FilterFactory>,
    topology: Option<FilterFactory>,
    /// Interface paths of the two filters, for the physical connections.
    wave_link: Vec<u16>,
    topology_link: Vec<u16>,
    stream: Option<Stream>,
}

impl EndpointState {
    /// Whether `pin` is the pin instance that has the endpoint's stream.
    fn owns_stream(&self, pin: &Pin) -> bool {
        self.stream.as_ref().is_some_and(|stream| stream.pin_key == pin.key())
    }
}

pub struct Endpoint {
    direction: Direction,
    index: u32,
    channel_count: u32,
    id: NameString,
    name: NameString,
    wave_reference: ReferenceString,
    topology_reference: ReferenceString,
    section: Shared<SharedSection>,
    /// The speaker mask: per endpoint, zero ("direct out") to begin with.
    speaker_mask: AtomicU32,
    state: Mutex<EndpointState>,
}

fn not_found() -> IoStatus {
    IoStatus::status(NtStatus::NOT_FOUND)
}

impl Endpoint {
    /// Creates the endpoint and its two filter factories. Nothing is
    /// visible to Windows yet: the device interfaces are off until
    /// [`publish`](Self::publish).
    pub fn create(
        guard: &DeviceGuard<'_>,
        request: &EndpointRequest,
        section: Shared<SharedSection>,
        owner: Shared<NotifyQueue>,
    ) -> Result<Shared<Self>, NtStatus> {
        let layout = *section.layout();
        let id = NameString::from_units(request.id()).ok_or(NtStatus::INVALID_PARAMETER)?;
        let name = NameString::from_units(request.name()).ok_or(NtStatus::INVALID_PARAMETER)?;
        let wave_reference = reference_string(id.as_units(), request.channel_count, &layout, FilterKind::Wave)?;
        let topology_reference =
            reference_string(id.as_units(), request.channel_count, &layout, FilterKind::Topology)?;

        let endpoint = Shared::try_new(Self {
            direction: request.direction,
            index: request.index,
            channel_count: request.channel_count,
            id,
            name,
            wave_reference,
            topology_reference,
            section,
            speaker_mask: AtomicU32::new(0),
            state: Mutex::try_new(EndpointState {
                owner: Some(owner),
                wave: None,
                topology: None,
                wave_link: Vec::new(),
                topology_link: Vec::new(),
                stream: None,
            })?,
        })?;

        let range = PinAudioRange::of(&endpoint.format());
        let wave = filters::with_wave_spec(endpoint.direction, range, |spec| {
            FilterFactory::create::<WaveFilter>(
                guard,
                spec,
                endpoint.wave_reference.as_units_with_nul(),
                endpoint.clone(),
            )
        })?;
        let topology = filters::with_topology_spec(endpoint.direction, |spec| {
            FilterFactory::create::<TopologyFilter>(
                guard,
                spec,
                endpoint.topology_reference.as_units_with_nul(),
                endpoint.clone(),
            )
        });
        let topology = match topology {
            Ok(topology) => topology,
            Err(status) => {
                wave.delete(guard);
                return Err(status);
            }
        };
        let links = wave
            .symbolic_link(guard)
            .and_then(|wave_link| Ok((wave_link, topology.symbolic_link(guard)?)));
        let (wave_link, topology_link) = match links {
            Ok(links) => links,
            Err(status) => {
                wave.delete(guard);
                topology.delete(guard);
                return Err(status);
            }
        };
        {
            let mut state = endpoint.state.lock();
            state.wave = Some(wave);
            state.topology = Some(topology);
            state.wave_link = wave_link;
            state.topology_link = topology_link;
        }
        Ok(endpoint)
    }

    pub fn index(&self) -> u32 {
        self.index
    }

    fn format(&self) -> EndpointFormat {
        let layout = self.section.layout();
        EndpointFormat {
            sample_rate: layout.sample_rate,
            sample_size: layout.sample_size,
            channel_count: self.channel_count,
        }
    }

    /// `STATUS_NOT_FOUND` once the endpoint has been withdrawn: its filters
    /// cannot be opened again.
    pub fn check_not_withdrawn(&self) -> Result<(), NtStatus> {
        if self.state.lock().owner.is_some() {
            Ok(())
        } else {
            Err(NtStatus::NOT_FOUND)
        }
    }

    /// Writes the registry values of every interface instance of both
    /// filters, then switches the interfaces on (section 7, A.10). Runs on
    /// a system worker thread.
    pub fn publish(&self, guard: &DeviceGuard<'_>) -> Result<(), NtStatus> {
        if self.state.lock().owner.is_none() {
            // The control handle closed while the request was queued.
            return Err(NtStatus::FILE_CLOSED);
        }
        let pdo = guard.device().physical_device_object();
        for category in filters::wave_categories(self.direction) {
            self.write_interface_values(pdo, &category, self.wave_reference.as_units())?;
        }
        for category in filters::topology_categories() {
            self.write_interface_values(pdo, &category, self.topology_reference.as_units())?;
        }

        // The topology filter first, so that when the wave filter (the one
        // with the streaming pin) arrives, the whole path to the jack can be
        // followed.
        let (wave, topology) = {
            let state = self.state.lock();
            (state.wave.clone(), state.topology.clone())
        };
        let (Some(wave), Some(topology)) = (wave, topology) else {
            return Err(NtStatus::FILE_CLOSED);
        };
        topology.set_enabled(guard, true)?;
        wave.set_enabled(guard, true)
    }

    fn write_interface_values(
        &self,
        pdo: &ntk::device::DeviceObject,
        category: &Guid,
        reference: &[u16],
    ) -> Result<(), NtStatus> {
        // Kernel streaming registered the interface when the factory was
        // created; registering it again returns the same instance.
        let interface = DeviceInterface::register(pdo, category, Some(reference))?;
        let key = interface.open_parameters_key(KEY_SET_VALUE | KEY_CREATE_SUB_KEY)?;

        // The endpoint property values live in the subkey MSEP\0. Failing to
        // open it only matters if a required value has to go there.
        let mut endpoint_key: Result<RegistryKey, NtStatus> = Err(NtStatus::UNSUCCESSFUL);
        for value in &INTERFACE_VALUES {
            let target = if value.subkey.is_some() {
                if endpoint_key.is_err() {
                    endpoint_key = open_endpoint_key(&key);
                }
                endpoint_key.as_ref().map_err(|status| *status)
            } else {
                Ok(&key)
            };
            let result = target.and_then(|key| self.write_value(key, value));
            if value.required {
                result?;
            }
        }
        Ok(())
    }

    fn write_value(&self, key: &RegistryKey, value: &InterfaceValue) -> Result<(), NtStatus> {
        let name = WideString::<64>::from_ascii(value.name).ok_or(NtStatus::UNSUCCESSFUL)?;
        match value.data {
            ValueData::Name => key.set_string(name.as_units(), self.name.as_units_with_nul()),
            ValueData::Id => key.set_string(name.as_units(), self.id.as_units_with_nul()),
            ValueData::Text(text) => {
                let text = WideString::<64>::from_ascii(text).ok_or(NtStatus::UNSUCCESSFUL)?;
                key.set_string(name.as_units(), text.as_units_with_nul())
            }
            ValueData::Dword(number) => key.set_dword(name.as_units(), number),
        }
    }

    /// Withdraws the endpoint: its interfaces are switched off, it loses
    /// its owner, and its factories are deleted so that the same identity
    /// can be created again. Streams and filters that are still open keep
    /// working on the section until their clients close them (section 11).
    pub fn withdraw(&self, guard: &DeviceGuard<'_>) {
        let (wave, topology) = {
            let mut state = self.state.lock();
            state.owner = None;
            (state.wave.take(), state.topology.take())
        };
        for factory in [&wave, &topology].into_iter().flatten() {
            let _ = factory.set_enabled(guard, false);
        }
        for factory in [&wave, &topology].into_iter().flatten() {
            factory.delete(guard);
        }
    }

    // ---- Streams (section 8) -------------------------------------------

    pub fn open_stream(&self, pin: &Pin) -> Result<(), NtStatus> {
        let channels =
            format::accepted_channels(pin.connection_format(), &self.format()).ok_or(NtStatus::NO_MATCH)?;
        let mut state = self.state.lock();
        if state.owner.is_none() {
            return Err(NtStatus::NOT_FOUND);
        }
        if state.stream.is_some() {
            return Err(NtStatus::RESOURCE_IN_USE);
        }
        let mut stream = Stream {
            pin_key: pin.key(),
            channels,
            register_views: Vec::new(),
            buffers: Vec::new(),
        };
        stream
            .register_views
            .try_reserve(1)
            .map_err(|_| NtStatus::INSUFFICIENT_RESOURCES)?;
        // The register table goes into the opening process.
        stream
            .register_views
            .push(self.section.map_registers_into_current_process()?);
        self.section
            .update_record(self.index, Event::StreamOpened { channels });
        state.stream = Some(stream);
        Ok(())
    }

    pub fn close_stream(&self, pin: &Pin) {
        let stream = {
            let mut state = self.state.lock();
            if !state.owns_stream(pin) {
                return;
            }
            self.section.update_record(self.index, Event::StreamClosed);
            state.stream.take()
        };
        // Remove the mappings from every process that had them, and return
        // the cells.
        if let Some(stream) = stream {
            for (cells, view) in stream.buffers {
                drop(view);
                self.section.free_cells(cells);
            }
            drop(stream.register_views);
        }
    }

    pub fn set_stream_state(&self, pin: &Pin, to: u32, from: u32) -> Result<(), NtStatus> {
        let (Some(to), Some(from)) = (StreamState::from_ks(to), StreamState::from_ks(from)) else {
            return Err(NtStatus::INVALID_PARAMETER);
        };
        let state = self.state.lock();
        // Still allowed on a withdrawn endpoint: the stream "can still
        // change state and be closed".
        if state.owns_stream(pin) {
            self.section
                .update_record(self.index, Event::StateChanged { from, to });
        }
        Ok(())
    }

    pub fn intersect(&self, pin_id: u32, caller_range: &[u8]) -> Result<DataFormat, NtStatus> {
        // Intersection is a property request like the others: on a
        // withdrawn endpoint it fails as if the endpoint were not found.
        self.check_not_withdrawn()?;
        if pin_id != pin::WAVE_STREAMING {
            return Err(NtStatus::NO_MATCH);
        }
        let mask = self.speaker_mask.load(Ordering::Relaxed);
        let bytes = format::intersect(caller_range, &self.format(), mask)?;
        DataFormat::new(&bytes).ok_or(NtStatus::NO_MATCH)
    }

    // ---- Property requests ---------------------------------------------

    pub fn wave_property(&self, target: Target<'_>, call: &PropertyCall<'_>, data: &mut [u8]) -> IoStatus {
        match target {
            Target::Filter if same_guid(&call.set, &guids::KSPROPSETID_Pin) => self.wave_pin_set(call, data),
            Target::Node if same_guid(&call.set, &guids::KSPROPSETID_Audio) => self.wave_node(call, data),
            Target::Pin(pin) if same_guid(&call.set, &guids::KSPROPSETID_RtAudio) => self.rt_audio(pin, call, data),
            _ => not_found(),
        }
    }

    /// The standard pin property set on the wave filter.
    fn wave_pin_set(&self, call: &PropertyCall<'_>, data: &mut [u8]) -> IoStatus {
        let Some(pin_id) = properties::pin_id(call.request) else {
            return IoStatus::status(NtStatus::INVALID_PARAMETER);
        };
        let state = self.state.lock();
        if state.owner.is_none() {
            return not_found();
        }
        match call.id {
            ids::KSPROPERTY_PIN_GLOBALCINSTANCES => {
                if pin_id == pin::WAVE_STREAMING {
                    properties::instance_counts(data, 1, u32::from(state.stream.is_some()))
                } else {
                    properties::instance_counts(data, 0, 0)
                }
            }
            ids::KSPROPERTY_PIN_PROPOSEDATAFORMAT if pin_id == pin::WAVE_STREAMING => {
                if call.is_set {
                    match properties::proposed_format_input(data) {
                        Ok(proposed) if format::accepted_channels(proposed, &self.format()).is_some() => {
                            IoStatus::SUCCESS
                        }
                        Ok(_) => IoStatus::status(NtStatus::NO_MATCH),
                        Err(refusal) => refusal,
                    }
                } else {
                    let mask = self.speaker_mask.load(Ordering::Relaxed);
                    properties::proposed_format(data, &format::default_format(&self.format(), mask))
                }
            }
            ids::KSPROPERTY_PIN_PHYSICALCONNECTION if pin_id == pin::WAVE_BRIDGE => {
                properties::physical_connection(data, pin::TOPOLOGY_BRIDGE, &state.topology_link)
            }
            _ => not_found(),
        }
    }

    /// The channel configuration of the wave filter's converter node.
    fn wave_node(&self, call: &PropertyCall<'_>, data: &mut [u8]) -> IoStatus {
        if self.state.lock().owner.is_none() {
            return not_found();
        }
        if call.id != ids::KSPROPERTY_AUDIO_CHANNEL_CONFIG || properties::node_id(call.request) != Some(pin::WAVE_NODE)
        {
            return not_found();
        }
        if call.is_set {
            match properties::speaker_mask_input(data) {
                Ok(mask) if format::speaker_mask_allowed(mask, self.channel_count) => {
                    self.speaker_mask.store(mask, Ordering::Relaxed);
                    IoStatus::SUCCESS
                }
                Ok(_) => IoStatus::status(NtStatus::NOT_SUPPORTED),
                Err(refusal) => refusal,
            }
        } else {
            properties::speaker_mask(data, self.speaker_mask.load(Ordering::Relaxed))
        }
    }

    /// The WaveRT property set on the stream.
    fn rt_audio(&self, pin: &Pin, call: &PropertyCall<'_>, data: &mut [u8]) -> IoStatus {
        let mut state = self.state.lock();
        let Some(owner) = state.owner.clone() else {
            return not_found();
        };
        let Some(stream) = state.stream.as_mut().filter(|s| s.pin_key == pin.key()) else {
            return not_found();
        };
        match call.id {
            ids::KSPROPERTY_RTAUDIO_BUFFER => self.allocate_buffer(stream, call, false, data),
            ids::KSPROPERTY_RTAUDIO_BUFFER_WITH_NOTIFICATION => self.allocate_buffer(stream, call, true, data),
            ids::KSPROPERTY_RTAUDIO_POSITIONREGISTER => self.position_register(stream, data),
            ids::KSPROPERTY_RTAUDIO_HWLATENCY => properties::hw_latency(data),
            ids::KSPROPERTY_RTAUDIO_QUERY_NOTIFICATION_SUPPORT => properties::boolean(data, true),
            ids::KSPROPERTY_RTAUDIO_REGISTER_NOTIFICATION_EVENT => {
                drop(state);
                IoStatus::status(self.register_event(&owner, call))
            }
            // Unregistering, the clock register, packet counts and the
            // presentation position are not implemented (section 8).
            _ => IoStatus::status(NtStatus::NOT_IMPLEMENTED),
        }
    }

    /// Allocates a buffer for the stream (section 8, "Allocating the
    /// buffer").
    ///
    /// A stream is meant to have one buffer. If a client asks again, the
    /// new buffer becomes the one described by the register record, and the
    /// earlier ones stay mapped and reserved until the stream closes, when
    /// all of them are released (the specification's section 13 notes that
    /// the earlier allocation used to be forgotten and never released).
    fn allocate_buffer(
        &self,
        stream: &mut Stream,
        call: &PropertyCall<'_>,
        with_notification: bool,
        data: &mut [u8],
    ) -> IoStatus {
        let Some(request) = BufferRequest::parse(call.request, with_notification) else {
            return IoStatus::status(NtStatus::INVALID_PARAMETER);
        };
        if request.base_address != 0 {
            // The client may not ask for a particular address.
            return IoStatus::status(NtStatus::NOT_IMPLEMENTED);
        }
        let geometry = match stream_buffer_geometry(self.section.layout(), stream.channels, request.requested_size) {
            Ok(geometry) => geometry,
            Err(status) => return IoStatus::status(status),
        };
        if stream.buffers.try_reserve(1).is_err() {
            return IoStatus::status(NtStatus::INSUFFICIENT_RESOURCES);
        }
        let (cells, view) = match self.section.allocate_buffer(geometry.cells) {
            Ok(allocation) => allocation,
            Err(status) => return IoStatus::status(status),
        };
        let reply = properties::rt_buffer(data, view.address(), geometry.size);
        if !reply.status.is_success() {
            // Nothing was handed out: take the mapping and the cells back.
            drop(view);
            self.section.free_cells(cells);
            return reply;
        }
        self.section.update_record(
            self.index,
            Event::BufferAllocated {
                offset: cells.first * CELL_SIZE,
                size: geometry.size,
                notification_count: request.notification_count,
            },
        );
        stream.buffers.push((cells, view));
        reply
    }

    /// The address of this endpoint's position register in the calling
    /// process, mapping the register table there if it is not yet.
    fn position_register(&self, stream: &mut Stream, data: &mut [u8]) -> IoStatus {
        let existing = stream
            .register_views
            .iter()
            .position(UserView::is_in_current_process);
        let position = match existing {
            Some(position) => position,
            None => {
                if stream.register_views.try_reserve(1).is_err() {
                    return IoStatus::status(NtStatus::INSUFFICIENT_RESOURCES);
                }
                match self.section.map_registers_into_current_process() {
                    Ok(view) => {
                        stream.register_views.push(view);
                        stream.register_views.len() - 1
                    }
                    Err(status) => return IoStatus::status(status),
                }
            }
        };
        let address = self
            .section
            .position_register_address(&stream.register_views[position], self.index);
        properties::hw_register(
            data,
            address,
            position_accuracy(self.section.layout(), stream.channels),
        )
    }

    /// Queues a client's notification event for the host (section 9).
    fn register_event(&self, owner: &NotifyQueue, call: &PropertyCall<'_>) -> NtStatus {
        let Some(handle) = properties::notification_event_handle(call.request) else {
            return NtStatus::INVALID_PARAMETER;
        };
        // Resolved now, with the client's own rights; the driver keeps the
        // event itself, not the handle value.
        let event = match EventObject::from_handle(handle, call.from_user_mode) {
            Ok(event) => event,
            Err(status) => return status,
        };
        let notification = Notification {
            event,
            generation: self.section.generation(self.index),
            index: self.index,
        };
        match owner.with_state(|queue| queue.push(notification)) {
            Ok(()) => {
                // Wake the host if it is waiting: its oldest wait completes
                // empty, and it asks again to collect the event.
                if let Some(wait) = owner.pop() {
                    wait.complete(IoStatus::SUCCESS);
                }
                NtStatus::SUCCESS
            }
            Err((status, notification)) => {
                // Release the event outside the queue's lock.
                drop(notification);
                status
            }
        }
    }

    /// The standard pin property set on the topology filter.
    pub fn topology_property(&self, call: &PropertyCall<'_>, data: &mut [u8]) -> IoStatus {
        if !same_guid(&call.set, &guids::KSPROPSETID_Pin) {
            return not_found();
        }
        let Some(pin_id) = properties::pin_id(call.request) else {
            return IoStatus::status(NtStatus::INVALID_PARAMETER);
        };
        let state = self.state.lock();
        if state.owner.is_none() {
            return not_found();
        }
        match call.id {
            ids::KSPROPERTY_PIN_NAME if pin_id == pin::TOPOLOGY_JACK => {
                properties::pin_name(data, self.name.as_units())
            }
            ids::KSPROPERTY_PIN_PHYSICALCONNECTION if pin_id == pin::TOPOLOGY_BRIDGE => {
                properties::physical_connection(data, pin::WAVE_BRIDGE, &state.wave_link)
            }
            _ => not_found(),
        }
    }
}

/// Opens (creating as needed) the `MSEP\0` subkey of an interface's
/// parameters key.
fn open_endpoint_key(parameters: &RegistryKey) -> Result<RegistryKey, NtStatus> {
    let access = KEY_SET_VALUE | KEY_CREATE_SUB_KEY;
    let first = WideString::<16>::from_ascii(MSEP_SUBKEY_PATH[0]).ok_or(NtStatus::UNSUCCESSFUL)?;
    let second = WideString::<16>::from_ascii(MSEP_SUBKEY_PATH[1]).ok_or(NtStatus::UNSUCCESSFUL)?;
    parameters
        .create_subkey(first.as_units(), access)?
        .create_subkey(second.as_units(), access)
}
