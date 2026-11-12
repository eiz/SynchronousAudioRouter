// Copyright (c) 2026 Itty Bitty Binaries, Inc.
// SPDX-License-Identifier: MIT

//! One endpoint: its two filter factories, its stream, and the property
//! requests Windows makes on it (sections 7, 8 and 9).
//!
//! # Locks (section 14)
//!
//! An endpoint's lock guards its own memory and is never held across a call
//! that can wait on another part of the system. Three things follow.
//!
//! * A view of the section is mapped, and unmapped, with the lock released.
//!   Whoever mapped one takes the lock again to hand it over, and if the
//!   endpoint has changed meanwhile, removes it.
//! * The two filter factories are not used under the lock. A thread that
//!   has kernel streaming calls to make on them takes them out of the
//!   endpoint's state, makes the calls, and puts them back or deletes them.
//!   While they are out, nobody else can touch them; somebody who wants the
//!   endpoint withdrawn meanwhile takes its owner away and waits for the
//!   thread that has the factories to notice and delete them.
//! * A request that is refused for what it says, not for the state of the
//!   endpoint, is refused before the lock is taken.
#![forbid(unsafe_code)]

use alloc::vec::Vec;
use core::sync::atomic::{AtomicBool, AtomicU32, Ordering};

use ks::{
    device::Device,
    filter::{DataFormat, FilterFactory, Pin, PropertyCall, Target},
    guids, ids,
};
use ntk::{
    alloc_ext::Shared,
    interface::DeviceInterface,
    object::EventObject,
    registry::{RegistryKey, KEY_CREATE_SUB_KEY, KEY_SET_VALUE},
    section::UserView,
    sync::{Completion, Mutex},
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
    notify::{Notification, NotificationQueue, Pushed},
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

/// An endpoint's two filter factories.
struct Factories {
    wave: FilterFactory,
    topology: FilterFactory,
}

impl Factories {
    /// Switches the interfaces of both off. Made without the device mutex
    /// (section 14).
    fn switch_off(&mut self) {
        let _ = self.wave.set_enabled(false);
        let _ = self.topology.set_enabled(false);
    }

    /// Deletes both. The device mutex is taken inside, for each deletion
    /// and for nothing else.
    fn delete(self, device: &Device) {
        self.wave.delete(device);
        self.topology.delete(device);
    }
}

struct EndpointState {
    /// The owning control handle's notification queue; `None` once the
    /// endpoint has been withdrawn ("marked as having no owner").
    owner: Option<Shared<NotifyQueue>>,
    /// Set once both factories exist and their interface paths are known.
    /// No filter can be opened before.
    ready: bool,
    /// The factories, while no thread has kernel streaming calls to make on
    /// them (module documentation). `None` before they exist, while a
    /// thread has them, and once they have been deleted.
    factories: Option<Factories>,
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

/// One endpoint.
///
/// It is shared by the control handle that created it and by its two filter
/// factories, each of which keeps a reference for as long as the factory or
/// a filter or pin opened from it exists. So it is released, and with it its
/// hold on the section, once it has been withdrawn (the control handle lets
/// go, the factories are deleted) and the last filter and pin a client had
/// open on it has been closed (section 11, "What is released, and when").
pub struct Endpoint {
    direction: Direction,
    index: u32,
    channel_count: u32,
    id: NameString,
    name: NameString,
    wave_reference: ReferenceString,
    topology_reference: ReferenceString,
    section: Shared<SharedSection>,
    /// The endpoint's entry among the identities in use on the device.
    identity: Shared<Identity>,
    /// The speaker mask: per endpoint, zero ("direct out") to begin with.
    speaker_mask: AtomicU32,
    state: Mutex<EndpointState>,
}

fn not_found() -> IoStatus {
    IoStatus::status(NtStatus::NOT_FOUND)
}

/// An endpoint's identity (section 7) as the rest of the device sees it:
/// the reference string its filter factories are created under, and whether
/// those factories still stand in the way of a new endpoint with the same
/// one.
pub struct Identity {
    /// The wave filter's reference string. The topology filter's is derived
    /// from it, so one comparison covers both.
    reference: ReferenceString,
    /// Set when the endpoint has lost its owner: its factories are about to
    /// be deleted.
    withdrawing: AtomicBool,
    /// Set when the endpoint's factories are gone (or it turned out that
    /// there would never be any).
    gone: Completion,
}

impl Identity {
    /// Whether a factory created under `self`'s reference string could
    /// collide with one created under `other`'s. Reference strings are
    /// names, and Windows compares names without regard to case; only ASCII
    /// letters are folded here. Erring towards "same" costs a short wait,
    /// erring towards "different" a refused request.
    fn collides_with(&self, other: &Self) -> bool {
        let fold = |unit: &u16| match u8::try_from(*unit) {
            Ok(byte) => u16::from(byte.to_ascii_lowercase()),
            Err(_) => *unit,
        };
        self.reference
            .as_units()
            .iter()
            .map(fold)
            .eq(other.reference.as_units().iter().map(fold))
    }
}

/// The identities of every endpoint of the device whose filter factories
/// exist, are being created or are being deleted.
///
/// It exists for one case. A control handle's endpoints are withdrawn one
/// at a time, and the device mutex is released in between (section 14), so
/// a host that starts while its predecessor's handle is still closing can
/// ask for an endpoint whose old factories have not been deleted yet.
/// Kernel streaming would refuse the new factories. Instead the new
/// request waits here, holding no lock, until the old ones are gone, which
/// takes the time of withdrawing the endpoints ahead of them at most.
pub struct Identities {
    list: Mutex<Vec<Shared<Identity>>>,
}

impl Identities {
    pub fn try_new() -> Result<Self, NtStatus> {
        Ok(Self {
            list: Mutex::try_new(Vec::new())?,
        })
    }

    /// Enters `identity`, first waiting for every endpoint of the same
    /// identity that is being withdrawn to lose its factories. An endpoint
    /// of the same identity that is not being withdrawn is not waited for:
    /// kernel streaming refuses the second set of factories, as it always
    /// has.
    fn claim(&self, identity: &Shared<Identity>) -> Result<(), NtStatus> {
        loop {
            let in_the_way = {
                let mut list = self.list.lock();
                let in_the_way = list
                    .iter()
                    .find(|other| other.withdrawing.load(Ordering::Acquire) && other.collides_with(identity))
                    .cloned();
                if in_the_way.is_none() {
                    list.try_reserve(1)
                        .map_err(|_| NtStatus::INSUFFICIENT_RESOURCES)?;
                    list.push(identity.clone());
                }
                in_the_way
            };
            match in_the_way {
                Some(other) => other.gone.wait(),
                None => return Ok(()),
            }
        }
    }

    /// The factories of `identity`'s endpoint are gone: takes it out and
    /// lets go whoever waits for that. Harmless if it was never entered.
    fn release(&self, identity: &Shared<Identity>) {
        let removed = {
            let mut list = self.list.lock();
            list.iter()
                .position(|candidate| Shared::ptr_eq(candidate, identity))
                .map(|position| list.swap_remove(position))
        };
        drop(removed);
        // After the removal, so that a request that wakes up does not find
        // the entry still there.
        identity.gone.set();
    }
}

impl Endpoint {
    /// Allocates an endpoint. Nothing of it exists in kernel streaming yet;
    /// see [`create_factories`](Self::create_factories).
    pub fn try_new(
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
        let identity = Shared::try_new(Identity {
            reference: wave_reference.clone(),
            withdrawing: AtomicBool::new(false),
            gone: Completion::try_new()?,
        })?;

        Shared::try_new(Self {
            direction: request.direction,
            index: request.index,
            channel_count: request.channel_count,
            id,
            name,
            wave_reference,
            topology_reference,
            section,
            identity,
            speaker_mask: AtomicU32::new(0),
            state: Mutex::try_new(EndpointState {
                owner: Some(owner),
                ready: false,
                factories: None,
                wave_link: Vec::new(),
                topology_link: Vec::new(),
                stream: None,
            })?,
        })
    }

    /// Creates the endpoint's two filter factories. Nothing is visible to
    /// Windows yet: the device interfaces are off until
    /// [`publish`](Self::publish).
    ///
    /// Called once, by the thread that handles the create request, with no
    /// lock held. On failure nothing of the endpoint is left in kernel
    /// streaming; that includes the case that the endpoint lost its owner
    /// meanwhile (`STATUS_FILE_CLOSED`).
    pub fn create_factories(endpoint: &Shared<Self>, device: &Device, identities: &Identities) -> Result<(), NtStatus> {
        let created = if endpoint.identity.withdrawing.load(Ordering::Acquire) {
            // The control handle has closed already: nothing is created.
            Err(NtStatus::FILE_CLOSED)
        } else {
            identities
                .claim(&endpoint.identity)
                .and_then(|()| Self::build_factories(endpoint, device))
        };
        let (status, factories) = match created {
            Ok((factories, wave_link, topology_link)) => {
                let mut state = endpoint.state.lock();
                if state.owner.is_some() {
                    state.wave_link = wave_link;
                    state.topology_link = topology_link;
                    state.factories = Some(factories);
                    state.ready = true;
                    return Ok(());
                }
                // The control handle closed meanwhile, and its close is
                // waiting for this thread to take the factories away.
                (NtStatus::FILE_CLOSED, Some(factories))
            }
            Err(status) => (status, None),
        };
        endpoint.disown();
        endpoint.take_down(device, identities, factories);
        Err(status)
    }

    /// The kernel streaming part of [`create_factories`](Self::create_factories).
    /// The device mutex is taken inside, once for each factory and only
    /// around its creation (section 14).
    fn build_factories(endpoint: &Shared<Self>, device: &Device) -> Result<(Factories, Vec<u16>, Vec<u16>), NtStatus> {
        let range = PinAudioRange::of(&endpoint.format());
        let wave = filters::with_wave_spec(endpoint.direction, range, |spec| {
            FilterFactory::create::<WaveFilter>(
                device,
                spec,
                endpoint.wave_reference.as_units_with_nul(),
                endpoint.clone(),
            )
        })?;
        let topology = filters::with_topology_spec(endpoint.direction, |spec| {
            FilterFactory::create::<TopologyFilter>(
                device,
                spec,
                endpoint.topology_reference.as_units_with_nul(),
                endpoint.clone(),
            )
        });
        let topology = match topology {
            Ok(topology) => topology,
            Err(status) => {
                wave.delete(device);
                return Err(status);
            }
        };
        let mut factories = Factories { wave, topology };
        // "Cached filter data" (A.9): kernel streaming writes each factory's
        // `FilterData` value and medium cache from the factory's own
        // descriptor, the wave factory first, before any interface is
        // switched on. The results are not checked: a failure here does not
        // fail the request. These are registry writes, and are made without
        // the device mutex.
        let _ = factories.wave.update_cache_data();
        let _ = factories.topology.update_cache_data();
        let links = factories
            .wave
            .symbolic_link()
            .and_then(|wave_link| Ok((wave_link, factories.topology.symbolic_link()?)));
        match links {
            Ok((wave_link, topology_link)) => Ok((factories, wave_link, topology_link)),
            Err(status) => {
                factories.delete(device);
                Err(status)
            }
        }
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

    /// `STATUS_NOT_FOUND` once the endpoint has been withdrawn: its
    /// property requests fail as if it were not found.
    fn check_not_withdrawn(&self) -> Result<(), NtStatus> {
        if self.state.lock().owner.is_some() {
            Ok(())
        } else {
            Err(NtStatus::NOT_FOUND)
        }
    }

    /// Whether a filter may be opened: not before the endpoint's factories
    /// are both in place, and not once it has been withdrawn ("its filters
    /// cannot be opened again").
    pub fn check_can_open_filter(&self) -> Result<(), NtStatus> {
        let state = self.state.lock();
        if state.ready && state.owner.is_some() {
            Ok(())
        } else {
            Err(NtStatus::NOT_FOUND)
        }
    }

    /// Writes the registry values of every interface instance of both
    /// filters, then switches the interfaces on (section 7, A.10). Runs on
    /// a system worker thread, with no lock held and without the device
    /// mutex.
    ///
    /// On failure the endpoint no longer exists: its factories have been
    /// deleted. `STATUS_FILE_CLOSED` if it lost its owner before or while
    /// it was published.
    pub fn publish(&self, device: &Device, identities: &Identities) -> Result<(), NtStatus> {
        let taken = {
            let mut state = self.state.lock();
            if state.owner.is_some() && !self.identity.withdrawing.load(Ordering::Acquire) {
                state.factories.take()
            } else {
                None
            }
        };
        let Some(mut factories) = taken else {
            // The control handle closed while the request was queued, and
            // has withdrawn the endpoint or is about to.
            return Err(NtStatus::FILE_CLOSED);
        };

        // The topology filter is switched on first, so that when the wave
        // filter (the one with the streaming pin) arrives, the whole path
        // to the jack can be followed.
        let published = self
            .write_all_interface_values(device.physical_device_object())
            .and_then(|()| factories.topology.set_enabled(true))
            .and_then(|()| factories.wave.set_enabled(true));

        let (status, owner) = {
            let mut state = self.state.lock();
            match published {
                Ok(()) if state.owner.is_some() => {
                    state.factories = Some(factories);
                    return Ok(());
                }
                // The control handle closed meanwhile, and its close is
                // waiting for this thread to take the factories away.
                Ok(()) => (NtStatus::FILE_CLOSED, None),
                // "... or with the failure that stopped it, in which case
                // no endpoint exists."
                Err(status) => (status, state.owner.take()),
            }
        };
        drop(owner);
        self.identity.withdrawing.store(true, Ordering::Release);
        self.take_down(device, identities, Some(factories));
        Err(status)
    }

    fn write_all_interface_values(&self, pdo: &ntk::device::DeviceObject) -> Result<(), NtStatus> {
        for category in filters::wave_categories(self.direction) {
            self.write_interface_values(pdo, &category, self.wave_reference.as_units())?;
        }
        for category in filters::topology_categories() {
            self.write_interface_values(pdo, &category, self.topology_reference.as_units())?;
        }
        Ok(())
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

    /// Says that the endpoint is about to be withdrawn. From here on a new
    /// endpoint of the same identity waits for this one's factories to be
    /// deleted instead of colliding with them, and nothing new is begun on
    /// this one. The endpoint itself goes on working until it is withdrawn.
    ///
    /// Only memory is touched, so a closing control handle does this for
    /// all its endpoints before it withdraws the first.
    pub fn announce_withdrawal(&self) {
        self.identity.withdrawing.store(true, Ordering::Release);
    }

    /// Marks the endpoint as having no owner (section 11): its filters
    /// cannot be opened any more and its property requests fail.
    fn disown(&self) {
        self.announce_withdrawal();
        let owner = self.state.lock().owner.take();
        // The reference is released with the lock released.
        drop(owner);
    }

    /// Withdraws the endpoint (section 11): its interfaces are switched
    /// off, so Windows removes the endpoint; it is marked as having no
    /// owner; and its factories are deleted so that the same identity can
    /// be created again. Streams and filters that are still open keep
    /// working on the section until their clients close them.
    ///
    /// In that order: an endpoint that has lost its owner refuses whatever
    /// Windows asks of it, and Windows should find it gone by then, not
    /// present and broken. With the device mutex no longer held for the
    /// duration (section 14), Windows does ask in between.
    ///
    /// Returns `true` when that has been done. If another thread has the
    /// factories (it is creating or publishing the endpoint), the endpoint
    /// only loses its owner here and the result is `false`: that thread
    /// switches off and deletes what it has when it finds the endpoint
    /// without an owner, and [`wait_withdrawn`](Self::wait_withdrawn) waits
    /// for it.
    pub fn withdraw(&self, device: &Device, identities: &Identities) -> bool {
        self.announce_withdrawal();
        let factories = self.state.lock().factories.take();
        match factories {
            Some(mut factories) => {
                factories.switch_off();
                self.disown();
                factories.delete(device);
                identities.release(&self.identity);
                true
            }
            None => {
                self.disown();
                false
            }
        }
    }

    /// Returns when the endpoint's factories are gone, whoever deletes
    /// them. Waits holding nothing, and only for an endpoint that has lost
    /// its owner: otherwise nobody may ever delete them.
    pub fn wait_withdrawn(&self) {
        self.identity.gone.wait();
    }

    /// Ends the existence in kernel streaming of an endpoint that has no
    /// owner, on the thread that has its factories: switches their
    /// interfaces off and deletes them, then lets go whoever waits for
    /// that. With no factories, only the last.
    fn take_down(&self, device: &Device, identities: &Identities, factories: Option<Factories>) {
        if let Some(mut factories) = factories {
            factories.switch_off();
            factories.delete(device);
        }
        identities.release(&self.identity);
    }

    // ---- Streams (section 8) -------------------------------------------

    pub fn open_stream(&self, pin: &Pin) -> Result<(), NtStatus> {
        let channels =
            format::accepted_channels(pin.connection_format(), &self.format()).ok_or(NtStatus::NO_MATCH)?;
        // Whether the stream can be had at all. Looked at twice: before the
        // register table is mapped, to spare a doomed request the mapping,
        // and again when the stream is taken.
        let refusal = |state: &EndpointState| {
            if state.owner.is_none() {
                Some(NtStatus::NOT_FOUND)
            } else if state.stream.is_some() {
                Some(NtStatus::RESOURCE_IN_USE)
            } else {
                None
            }
        };
        if let Some(status) = refusal(&self.state.lock()) {
            return Err(status);
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
        // The register table goes into the opening process, with the lock
        // released: a request on this endpoint does not wait for the
        // mapping.
        stream
            .register_views
            .push(self.section.map_registers_into_current_process()?);

        let mut state = self.state.lock();
        if let Some(status) = refusal(&state) {
            // Another open got the stream, or the endpoint was withdrawn,
            // meanwhile. The mapping made for this one is removed again,
            // with the lock released.
            drop(state);
            drop(stream);
            return Err(status);
        }
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
        if pin_id != pin::WAVE_STREAMING {
            return Err(NtStatus::NO_MATCH);
        }
        // Intersection is a property request like the others: on a
        // withdrawn endpoint it fails as if the endpoint were not found.
        self.check_not_withdrawn()?;
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
        match call.id {
            ids::KSPROPERTY_PIN_GLOBALCINSTANCES => {
                let state = self.state.lock();
                if state.owner.is_none() {
                    not_found()
                } else if pin_id == pin::WAVE_STREAMING {
                    properties::instance_counts(data, 1, u32::from(state.stream.is_some()))
                } else {
                    properties::instance_counts(data, 0, 0)
                }
            }
            ids::KSPROPERTY_PIN_PROPOSEDATAFORMAT if pin_id == pin::WAVE_STREAMING => {
                // Nothing of the endpoint's changeable state goes into the
                // answer, apart from whether it still has an owner.
                if self.check_not_withdrawn().is_err() {
                    return not_found();
                }
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
                let state = self.state.lock();
                if state.owner.is_none() {
                    not_found()
                } else {
                    properties::physical_connection(data, pin::TOPOLOGY_BRIDGE, &state.topology_link)
                }
            }
            // Not a property of that pin: refused for what the request
            // says, without a look at the endpoint (section 14).
            _ => not_found(),
        }
    }

    /// The channel configuration of the wave filter's converter node.
    fn wave_node(&self, call: &PropertyCall<'_>, data: &mut [u8]) -> IoStatus {
        if call.id != ids::KSPROPERTY_AUDIO_CHANNEL_CONFIG || properties::node_id(call.request) != Some(pin::WAVE_NODE)
        {
            return not_found();
        }
        if self.check_not_withdrawn().is_err() {
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
        match call.id {
            ids::KSPROPERTY_RTAUDIO_BUFFER => self.allocate_buffer(pin, call, false, data),
            ids::KSPROPERTY_RTAUDIO_BUFFER_WITH_NOTIFICATION => self.allocate_buffer(pin, call, true, data),
            ids::KSPROPERTY_RTAUDIO_POSITIONREGISTER => self.position_register(pin, data),
            ids::KSPROPERTY_RTAUDIO_HWLATENCY => match self.stream_of(pin) {
                Ok(_) => properties::hw_latency(data),
                Err(refusal) => refusal,
            },
            ids::KSPROPERTY_RTAUDIO_QUERY_NOTIFICATION_SUPPORT => match self.stream_of(pin) {
                Ok(_) => properties::boolean(data, true),
                Err(refusal) => refusal,
            },
            ids::KSPROPERTY_RTAUDIO_REGISTER_NOTIFICATION_EVENT => IoStatus::status(self.register_event(pin, call)),
            // Unregistering, the clock register, packet counts and the
            // presentation position are not implemented (section 8). The
            // refusal has nothing to do with the state of the endpoint, so
            // it is made without the endpoint's lock (section 14): Windows
            // may ask for the presentation position again and again while
            // a stream runs.
            _ => IoStatus::status(NtStatus::NOT_IMPLEMENTED),
        }
    }

    /// What a WaveRT request has to know of the stream it was made on: the
    /// queue of the control handle that owns the endpoint, and the stream's
    /// channel count. `STATUS_NOT_FOUND` if the endpoint has been withdrawn
    /// or `pin` does not have its stream.
    fn stream_of(&self, pin: &Pin) -> Result<(Shared<NotifyQueue>, u32), IoStatus> {
        let state = self.state.lock();
        let owner = state.owner.clone().ok_or_else(not_found)?;
        let stream = state
            .stream
            .as_ref()
            .filter(|stream| stream.pin_key == pin.key())
            .ok_or_else(not_found)?;
        Ok((owner, stream.channels))
    }

    /// Allocates a buffer for the stream (section 8, "Allocating the
    /// buffer").
    ///
    /// A stream is meant to have one buffer. If a client asks again, the
    /// new buffer becomes the one described by the register record, and the
    /// earlier ones stay mapped and reserved until the stream closes, when
    /// all of them are released (the specification's section 13 notes that
    /// the earlier allocation used to be forgotten and never released).
    fn allocate_buffer(&self, pin: &Pin, call: &PropertyCall<'_>, with_notification: bool, data: &mut [u8]) -> IoStatus {
        // What the request itself is refused for comes first, without a
        // look at the endpoint.
        let Some(request) = BufferRequest::parse(call.request, with_notification) else {
            return IoStatus::status(NtStatus::INVALID_PARAMETER);
        };
        if request.base_address != 0 {
            // The client may not ask for a particular address.
            return IoStatus::status(NtStatus::NOT_IMPLEMENTED);
        }
        let channels = match self.stream_of(pin) {
            Ok((_, channels)) => channels,
            Err(refusal) => return refusal,
        };
        let geometry = match stream_buffer_geometry(self.section.layout(), channels, request.requested_size) {
            Ok(geometry) => geometry,
            Err(status) => return IoStatus::status(status),
        };

        // The cells are taken and mapped with the endpoint's lock released.
        let (cells, view) = match self.section.allocate_buffer(geometry.cells) {
            Ok(allocation) => allocation,
            Err(status) => return IoStatus::status(status),
        };
        let reply = properties::rt_buffer(data, view.address(), geometry.size);
        let mut allocation = Some((cells, view));
        let mut failure = None;
        if reply.status.is_success() {
            // The buffer becomes the stream's, if the stream is still
            // there. (It is: a stream is not closed while one of its own
            // requests is in progress.)
            let mut state = self.state.lock();
            match state.stream.as_mut().filter(|stream| stream.pin_key == pin.key()) {
                Some(stream) => {
                    if stream.buffers.try_reserve(1).is_ok() {
                        self.section.update_record(
                            self.index,
                            Event::BufferAllocated {
                                offset: cells.first * CELL_SIZE,
                                size: geometry.size,
                                notification_count: request.notification_count,
                            },
                        );
                        if let Some(buffer) = allocation.take() {
                            stream.buffers.push(buffer);
                        }
                    } else {
                        failure = Some(NtStatus::INSUFFICIENT_RESOURCES);
                    }
                }
                None => failure = Some(NtStatus::NOT_FOUND),
            }
        }
        // Nothing was handed out: the mapping and the cells are taken back
        // before the failure is reported, with the lock released.
        if let Some((cells, view)) = allocation {
            drop(view);
            self.section.free_cells(cells);
        }
        match failure {
            Some(status) => IoStatus::status(status),
            None => reply,
        }
    }

    /// The address of this endpoint's position register in the calling
    /// process, mapping the register table there if it is not yet.
    fn position_register(&self, pin: &Pin, data: &mut [u8]) -> IoStatus {
        // A view mapped by this request, until the stream has taken it.
        let mut mapped: Option<UserView> = None;
        let found = loop {
            {
                let mut state = self.state.lock();
                if state.owner.is_none() {
                    break Err(not_found());
                }
                let Some(stream) = state.stream.as_mut().filter(|stream| stream.pin_key == pin.key()) else {
                    break Err(not_found());
                };
                let existing = stream
                    .register_views
                    .iter()
                    .find(|view| view.is_in_current_process());
                if let Some(view) = existing {
                    break Ok((self.section.position_register_address(view, self.index), stream.channels));
                }
                if let Some(view) = mapped.take() {
                    if stream.register_views.try_reserve(1).is_err() {
                        mapped = Some(view);
                        break Err(IoStatus::status(NtStatus::INSUFFICIENT_RESOURCES));
                    }
                    let address = self.section.position_register_address(&view, self.index);
                    stream.register_views.push(view);
                    break Ok((address, stream.channels));
                }
            }
            // The table is not mapped in this process: map it with the lock
            // released, then look again.
            match self.section.map_registers_into_current_process() {
                Ok(view) => mapped = Some(view),
                Err(status) => break Err(IoStatus::status(status)),
            }
        };
        // A view that was not needed after all (another request of the
        // same process mapped the table meanwhile, or the stream went) is
        // removed here, with the lock released.
        drop(mapped);
        match found {
            Ok((address, channels)) => {
                properties::hw_register(data, address, position_accuracy(self.section.layout(), channels))
            }
            Err(refusal) => refusal,
        }
    }

    /// Queues a client's notification event for the host (section 9).
    fn register_event(&self, pin: &Pin, call: &PropertyCall<'_>) -> NtStatus {
        let Some(handle) = properties::notification_event_handle(call.request) else {
            return NtStatus::INVALID_PARAMETER;
        };
        let owner = match self.stream_of(pin) {
            Ok((owner, _)) => owner,
            Err(refusal) => return refusal.status,
        };
        // Resolved now, as a handle of the requesting process with that
        // process's own rights, whatever the request says about its origin
        // (A.8). The driver keeps the event itself, not the handle value.
        let event = match EventObject::from_user_handle(handle) {
            Ok(event) => event,
            Err(status) => return status,
        };
        let notification = Notification {
            event,
            generation: self.section.generation(self.index),
            index: self.index,
        };
        // The queue's lock is a spin lock the host's real-time thread
        // takes: nothing is allocated or freed under it (section 14). If
        // the queue needs room, it says so, the room is allocated here with
        // the lock released, and the event is offered again.
        let mut offered = owner.with_state(|queue| queue.push(notification, None));
        if let Pushed::NeedsChunk(notification) = offered {
            offered = match NotificationQueue::new_chunk() {
                Ok(chunk) => owner.with_state(|queue| queue.push(notification, Some(chunk))),
                Err(status) => Pushed::Refused {
                    status,
                    notification,
                    unused: None,
                },
            };
        }
        match offered {
            Pushed::Queued { unused } => {
                drop(unused);
                // Wake the host if it is waiting: its oldest wait completes
                // empty, and it asks again to collect the event.
                if let Some(wait) = owner.pop() {
                    wait.complete(IoStatus::SUCCESS);
                }
                NtStatus::SUCCESS
            }
            Pushed::Refused {
                status,
                notification,
                unused,
            } => {
                // The event and the room are released here, outside the
                // queue's lock.
                drop(notification);
                drop(unused);
                status
            }
            // Cannot happen: the event was offered with room.
            Pushed::NeedsChunk(notification) => {
                drop(notification);
                NtStatus::INSUFFICIENT_RESOURCES
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
        match call.id {
            ids::KSPROPERTY_PIN_NAME if pin_id == pin::TOPOLOGY_JACK => {
                if self.check_not_withdrawn().is_err() {
                    return not_found();
                }
                properties::pin_name(data, self.name.as_units())
            }
            ids::KSPROPERTY_PIN_PHYSICALCONNECTION if pin_id == pin::TOPOLOGY_BRIDGE => {
                let state = self.state.lock();
                if state.owner.is_none() {
                    not_found()
                } else {
                    properties::physical_connection(data, pin::WAVE_BRIDGE, &state.wave_link)
                }
            }
            // Not a property of that pin: refused without a look at the
            // endpoint.
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
