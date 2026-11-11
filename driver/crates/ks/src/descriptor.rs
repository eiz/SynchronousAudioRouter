// Copyright (c) 2026 Itty Bitty Binaries, Inc.
// SPDX-License-Identifier: MIT

//! Filter descriptions.
//!
//! A driver describes a filter with the plain-data types here
//! ([`FilterSpec`] and its parts). [`Storage`] turns a description into the
//! linked `KSFILTER_DESCRIPTOR` structures AVStream reads, with the
//! handler's trampolines filled in.

use alloc::{boxed::Box, vec::Vec};
use core::mem::{size_of, ManuallyDrop};

use ks_sys as sys;
use ntk::{alloc_ext::try_box, Guid};
use ntstatus::NtStatus;

use crate::filter::{trampolines, FilterHandler};

/// One property of a property set.
#[derive(Clone, Copy, Debug)]
pub struct PropertyItem {
    pub id: u32,
    pub get: bool,
    pub set: bool,
    /// Smallest request AVStream lets through to the handler.
    pub min_property: u32,
    /// Smallest value buffer AVStream lets through (0: any, including none).
    pub min_data: u32,
}

#[derive(Clone, Copy, Debug)]
pub struct PropertySet<'a> {
    pub set: Guid,
    pub items: &'a [PropertyItem],
}

/// One event of an event set. AVStream's default handling is used: callers
/// may subscribe, and the driver may (or may never) raise it.
#[derive(Clone, Copy, Debug)]
pub struct EventItem {
    pub id: u32,
    /// Smallest event data AVStream accepts.
    pub min_data: u32,
}

#[derive(Clone, Copy, Debug)]
pub struct EventSet<'a> {
    pub set: Guid,
    pub items: &'a [EventItem],
}

/// The properties and events of a filter, a pin or a node.
#[derive(Clone, Copy, Debug, Default)]
pub struct Automation<'a> {
    pub properties: &'a [PropertySet<'a>],
    pub events: &'a [EventSet<'a>],
}

impl Automation<'_> {
    pub const NONE: Automation<'static> = Automation {
        properties: &[],
        events: &[],
    };

    fn is_empty(&self) -> bool {
        self.properties.is_empty() && self.events.is_empty()
    }
}

/// A data range of a pin.
#[derive(Clone, Copy, Debug)]
pub enum DataRange {
    /// A bare `KSDATARANGE` (64 bytes).
    Plain {
        major: Guid,
        sub: Guid,
        specifier: Guid,
    },
    /// A `KSDATARANGE_AUDIO` (88 bytes).
    Audio {
        major: Guid,
        sub: Guid,
        specifier: Guid,
        maximum_channels: u32,
        minimum_bits_per_sample: u32,
        maximum_bits_per_sample: u32,
        minimum_sample_frequency: u32,
        maximum_sample_frequency: u32,
    },
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum DataFlow {
    In,
    Out,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Communication {
    None,
    Sink,
}

/// An interface a pin supports: set and id.
#[derive(Clone, Copy, Debug)]
pub struct Interface {
    pub set: Guid,
    pub id: u32,
}

#[derive(Clone, Copy, Debug)]
pub struct PinSpec<'a> {
    /// Whether clients can open the pin as a stream: it then gets the
    /// handler's create, close and state-change callbacks.
    pub streaming: bool,
    pub interfaces: &'a [Interface],
    pub data_ranges: &'a [DataRange],
    pub data_flow: DataFlow,
    pub communication: Communication,
    pub category: Option<Guid>,
    /// `KSPIN_FLAG_*`.
    pub flags: u32,
    pub instances_possible: u32,
    pub instances_necessary: u32,
    /// Whether the handler answers data-range intersection for this pin.
    pub intersect: bool,
    pub automation: Automation<'a>,
}

#[derive(Clone, Copy, Debug)]
pub struct NodeSpec<'a> {
    pub kind: Guid,
    pub automation: Automation<'a>,
}

/// A topology connection. Node `KSFILTER_NODE` is the filter itself, whose
/// "pins" are the filter's pins; a real node's input is pin 1 and its
/// output pin 0.
#[derive(Clone, Copy, Debug)]
pub struct Connection {
    pub from_node: u32,
    pub from_pin: u32,
    pub to_node: u32,
    pub to_pin: u32,
}

#[derive(Clone, Copy, Debug)]
pub struct FilterSpec<'a> {
    /// Device interface categories. The first one's interface is the one
    /// the factory reports as its symbolic link.
    pub categories: &'a [Guid],
    pub pins: &'a [PinSpec<'a>],
    pub nodes: &'a [NodeSpec<'a>],
    pub connections: &'a [Connection],
    pub automation: Automation<'a>,
}

/// Which object an automation table belongs to; selects the trampoline.
#[derive(Clone, Copy)]
enum Scope {
    Filter,
    Pin,
    Node,
}

fn try_vec<T>(capacity: usize) -> Result<Vec<T>, NtStatus> {
    let mut v = Vec::new();
    v.try_reserve_exact(capacity)
        .map_err(|_| NtStatus::INSUFFICIENT_RESOURCES)?;
    Ok(v)
}

fn count(len: usize) -> Result<u32, NtStatus> {
    u32::try_from(len).map_err(|_| NtStatus::INVALID_PARAMETER)
}

/// The memory behind one `KSAUTOMATION_TABLE`. Boxed by its owner, so the
/// table and the vectors' heap blocks never move.
struct AutomationStorage {
    table: sys::KSAUTOMATION_TABLE,
    _set_guids: Vec<Guid>,
    _items: Vec<Vec<sys::KSPROPERTY_ITEM>>,
    _sets: Vec<sys::KSPROPERTY_SET>,
    _event_guids: Vec<Guid>,
    _event_items: Vec<Vec<sys::KSEVENT_ITEM>>,
    _event_sets: Vec<sys::KSEVENT_SET>,
}

impl AutomationStorage {
    fn build<H: FilterHandler>(spec: &Automation<'_>, scope: Scope) -> Result<Option<Box<Self>>, NtStatus> {
        if spec.is_empty() {
            return Ok(None);
        }
        let handler: sys::PFNKSHANDLER = match scope {
            Scope::Filter => Some(trampolines::filter_property::<H>),
            Scope::Pin => Some(trampolines::pin_property::<H>),
            Scope::Node => Some(trampolines::node_property::<H>),
        };

        let mut set_guids = try_vec(spec.properties.len())?;
        let mut items = try_vec(spec.properties.len())?;
        let mut sets = try_vec(spec.properties.len())?;
        for set in spec.properties {
            set_guids.push(set.set);
            let mut set_items = try_vec(set.items.len())?;
            for item in set.items {
                set_items.push(sys::KSPROPERTY_ITEM {
                    PropertyId: item.id,
                    __bindgen_anon_1: sys::KSPROPERTY_ITEM__bindgen_ty_1 {
                        GetPropertyHandler: ManuallyDrop::new(if item.get { handler } else { None }),
                    },
                    MinProperty: item.min_property,
                    MinData: item.min_data,
                    __bindgen_anon_2: sys::KSPROPERTY_ITEM__bindgen_ty_2 {
                        SetPropertyHandler: ManuallyDrop::new(if item.set { handler } else { None }),
                    },
                    Values: core::ptr::null(),
                    RelationsCount: 0,
                    Relations: core::ptr::null(),
                    SupportHandler: None,
                    SerializedSize: 0,
                });
            }
            items.push(set_items);
        }
        // The vectors above are complete; their heap blocks stay put from
        // here on, so pointers into them can be taken.
        for (index, set_items) in items.iter().enumerate() {
            sets.push(sys::KSPROPERTY_SET {
                Set: &set_guids[index],
                PropertiesCount: count(set_items.len())?,
                PropertyItem: set_items.as_ptr(),
                FastIoCount: 0,
                FastIoTable: core::ptr::null(),
            });
        }

        let mut event_guids = try_vec(spec.events.len())?;
        let mut event_items = try_vec(spec.events.len())?;
        let mut event_sets = try_vec(spec.events.len())?;
        for set in spec.events {
            event_guids.push(set.set);
            let mut set_items = try_vec(set.items.len())?;
            for item in set.items {
                set_items.push(sys::KSEVENT_ITEM {
                    EventId: item.id,
                    DataInput: item.min_data,
                    ExtraEntryData: 0,
                    AddHandler: None,
                    RemoveHandler: None,
                    SupportHandler: None,
                });
            }
            event_items.push(set_items);
        }
        for (index, set_items) in event_items.iter().enumerate() {
            event_sets.push(sys::KSEVENT_SET {
                Set: &event_guids[index],
                EventsCount: count(set_items.len())?,
                EventItem: set_items.as_ptr(),
            });
        }

        let table = sys::KSAUTOMATION_TABLE {
            PropertySetsCount: count(sets.len())?,
            PropertyItemSize: size_of::<sys::KSPROPERTY_ITEM>() as u32,
            PropertySets: if sets.is_empty() { core::ptr::null() } else { sets.as_ptr() },
            MethodSetsCount: 0,
            MethodItemSize: size_of::<sys::KSMETHOD_ITEM>() as u32,
            MethodSets: core::ptr::null(),
            EventSetsCount: count(event_sets.len())?,
            EventItemSize: size_of::<sys::KSEVENT_ITEM>() as u32,
            EventSets: if event_sets.is_empty() { core::ptr::null() } else { event_sets.as_ptr() },
        };
        try_box(Self {
            table,
            _set_guids: set_guids,
            _items: items,
            _sets: sets,
            _event_guids: event_guids,
            _event_items: event_items,
            _event_sets: event_sets,
        })
        .map(Some)
    }
}

fn table_ptr(storage: &Option<Box<AutomationStorage>>) -> *const sys::KSAUTOMATION_TABLE {
    storage
        .as_ref()
        .map_or(core::ptr::null(), |boxed| core::ptr::from_ref(&boxed.table))
}

fn data_format(size: usize, major: Guid, sub: Guid, specifier: Guid) -> sys::KSDATARANGE {
    sys::KSDATARANGE {
        __bindgen_anon_1: ManuallyDrop::new(sys::KSDATAFORMAT__bindgen_ty_1 {
            FormatSize: size as u32,
            Flags: 0,
            SampleSize: 0,
            Reserved: 0,
            MajorFormat: major,
            SubFormat: sub,
            Specifier: specifier,
        }),
    }
}

/// The per-pin memory a `KSPIN_DESCRIPTOR_EX` points to.
struct PinStorage {
    _category: Option<Box<Guid>>,
    _interfaces: Vec<sys::KSPIN_INTERFACE>,
    _plain_ranges: Vec<sys::KSDATARANGE>,
    _audio_ranges: Vec<sys::KSDATARANGE_AUDIO>,
    _range_pointers: Vec<sys::PKSDATARANGE>,
    _automation: Option<Box<AutomationStorage>>,
}

struct NodeStorage {
    _kind: Box<Guid>,
    _automation: Option<Box<AutomationStorage>>,
}

/// A complete `KSFILTER_DESCRIPTOR` and everything it points to.
///
/// Always boxed; AVStream keeps pointers into it for as long as a filter
/// factory created from it, or a filter or pin of that factory, exists.
pub(crate) struct Storage {
    descriptor: sys::KSFILTER_DESCRIPTOR,
    _reference_guid: Guid,
    _categories: Vec<Guid>,
    _pins: Vec<sys::KSPIN_DESCRIPTOR_EX>,
    _pin_storage: Vec<PinStorage>,
    _nodes: Vec<sys::KSNODE_DESCRIPTOR>,
    _node_storage: Vec<NodeStorage>,
    _connections: Vec<sys::KSTOPOLOGY_CONNECTION>,
    _automation: Option<Box<AutomationStorage>>,
}

// SAFETY: the structure is immutable after construction and its raw
// pointers refer only to memory it owns (or to constants), so it can be
// shared and released from any thread.
unsafe impl Send for Storage {}
// SAFETY: as above.
unsafe impl Sync for Storage {}

impl Storage {
    /// Builds the descriptor for `spec` with the trampolines of `H`.
    pub(crate) fn build<H: FilterHandler>(spec: &FilterSpec<'_>) -> Result<Box<Self>, NtStatus> {
        let categories = ntk::alloc_ext::try_vec_from_slice(spec.categories)?;

        let mut pins = try_vec(spec.pins.len())?;
        let mut pin_storage = try_vec(spec.pins.len())?;
        for pin in spec.pins {
            let category = match pin.category {
                Some(guid) => Some(try_box(guid)?),
                None => None,
            };
            let mut interfaces = try_vec(pin.interfaces.len())?;
            for interface in pin.interfaces {
                interfaces.push(sys::KSPIN_INTERFACE {
                    __bindgen_anon_1: sys::KSIDENTIFIER__bindgen_ty_1 {
                        __bindgen_anon_1: ManuallyDrop::new(sys::KSIDENTIFIER__bindgen_ty_1__bindgen_ty_1 {
                            Set: interface.set,
                            Id: interface.id,
                            Flags: 0,
                        }),
                    },
                });
            }

            let plain_count = pin
                .data_ranges
                .iter()
                .filter(|r| matches!(r, DataRange::Plain { .. }))
                .count();
            let mut plain_ranges = try_vec(plain_count)?;
            let mut audio_ranges = try_vec(pin.data_ranges.len() - plain_count)?;
            for range in pin.data_ranges {
                match *range {
                    DataRange::Plain { major, sub, specifier } => {
                        plain_ranges.push(data_format(size_of::<sys::KSDATARANGE>(), major, sub, specifier));
                    }
                    DataRange::Audio {
                        major,
                        sub,
                        specifier,
                        maximum_channels,
                        minimum_bits_per_sample,
                        maximum_bits_per_sample,
                        minimum_sample_frequency,
                        maximum_sample_frequency,
                    } => audio_ranges.push(sys::KSDATARANGE_AUDIO {
                        DataRange: data_format(size_of::<sys::KSDATARANGE_AUDIO>(), major, sub, specifier),
                        MaximumChannels: maximum_channels,
                        MinimumBitsPerSample: minimum_bits_per_sample,
                        MaximumBitsPerSample: maximum_bits_per_sample,
                        MinimumSampleFrequency: minimum_sample_frequency,
                        MaximumSampleFrequency: maximum_sample_frequency,
                    }),
                }
            }
            // Both vectors are complete: collect the pointers, in the order
            // the ranges were given.
            let mut range_pointers = try_vec(pin.data_ranges.len())?;
            let (mut next_plain, mut next_audio) = (0, 0);
            for range in pin.data_ranges {
                let pointer: *const sys::KSDATARANGE = match range {
                    DataRange::Plain { .. } => {
                        next_plain += 1;
                        &plain_ranges[next_plain - 1]
                    }
                    DataRange::Audio { .. } => {
                        next_audio += 1;
                        &audio_ranges[next_audio - 1].DataRange
                    }
                };
                range_pointers.push(pointer.cast_mut());
            }

            let automation = AutomationStorage::build::<H>(&pin.automation, Scope::Pin)?;

            pins.push(sys::KSPIN_DESCRIPTOR_EX {
                Dispatch: if pin.streaming {
                    trampolines::pin_dispatch::<H>()
                } else {
                    core::ptr::null()
                },
                AutomationTable: table_ptr(&automation),
                PinDescriptor: sys::KSPIN_DESCRIPTOR {
                    InterfacesCount: count(interfaces.len())?,
                    Interfaces: if interfaces.is_empty() { core::ptr::null() } else { interfaces.as_ptr() },
                    MediumsCount: 0,
                    Mediums: core::ptr::null(),
                    DataRangesCount: count(range_pointers.len())?,
                    DataRanges: range_pointers.as_ptr(),
                    DataFlow: match pin.data_flow {
                        DataFlow::In => sys::KSPIN_DATAFLOW::KSPIN_DATAFLOW_IN,
                        DataFlow::Out => sys::KSPIN_DATAFLOW::KSPIN_DATAFLOW_OUT,
                    },
                    Communication: match pin.communication {
                        Communication::None => sys::KSPIN_COMMUNICATION::KSPIN_COMMUNICATION_NONE,
                        Communication::Sink => sys::KSPIN_COMMUNICATION::KSPIN_COMMUNICATION_SINK,
                    },
                    Category: category
                        .as_ref()
                        .map_or(core::ptr::null(), |boxed| core::ptr::from_ref::<Guid>(boxed)),
                    Name: core::ptr::null(),
                    __bindgen_anon_1: sys::KSPIN_DESCRIPTOR__bindgen_ty_1 {
                        Reserved: ManuallyDrop::new(0),
                    },
                },
                Flags: pin.flags,
                InstancesPossible: pin.instances_possible,
                InstancesNecessary: pin.instances_necessary,
                AllocatorFraming: core::ptr::null(),
                IntersectHandler: if pin.intersect {
                    Some(trampolines::intersect::<H>)
                } else {
                    None
                },
            });
            pin_storage.push(PinStorage {
                _category: category,
                _interfaces: interfaces,
                _plain_ranges: plain_ranges,
                _audio_ranges: audio_ranges,
                _range_pointers: range_pointers,
                _automation: automation,
            });
        }

        let mut nodes = try_vec(spec.nodes.len())?;
        let mut node_storage = try_vec(spec.nodes.len())?;
        for node in spec.nodes {
            let kind = try_box(node.kind)?;
            let automation = AutomationStorage::build::<H>(&node.automation, Scope::Node)?;
            nodes.push(sys::KSNODE_DESCRIPTOR {
                AutomationTable: table_ptr(&automation),
                Type: core::ptr::from_ref::<Guid>(&kind),
                Name: core::ptr::null(),
            });
            node_storage.push(NodeStorage {
                _kind: kind,
                _automation: automation,
            });
        }

        let mut connections = try_vec(spec.connections.len())?;
        for c in spec.connections {
            connections.push(sys::KSTOPOLOGY_CONNECTION {
                FromNode: c.from_node,
                FromNodePin: c.from_pin,
                ToNode: c.to_node,
                ToNodePin: c.to_pin,
            });
        }

        let automation = AutomationStorage::build::<H>(&spec.automation, Scope::Filter)?;

        let descriptor = sys::KSFILTER_DESCRIPTOR {
            Dispatch: trampolines::filter_dispatch::<H>(),
            AutomationTable: table_ptr(&automation),
            Version: sys::KSFILTER_DESCRIPTOR_VERSION,
            Flags: 0,
            // Patched below, once the block has its final address.
            ReferenceGuid: core::ptr::null(),
            PinDescriptorsCount: count(pins.len())?,
            PinDescriptorSize: size_of::<sys::KSPIN_DESCRIPTOR_EX>() as u32,
            PinDescriptors: if pins.is_empty() { core::ptr::null() } else { pins.as_ptr() },
            CategoriesCount: count(categories.len())?,
            Categories: if categories.is_empty() { core::ptr::null() } else { categories.as_ptr() },
            NodeDescriptorsCount: count(nodes.len())?,
            NodeDescriptorSize: size_of::<sys::KSNODE_DESCRIPTOR>() as u32,
            NodeDescriptors: if nodes.is_empty() { core::ptr::null() } else { nodes.as_ptr() },
            ConnectionsCount: count(connections.len())?,
            Connections: if connections.is_empty() { core::ptr::null() } else { connections.as_ptr() },
            ComponentId: core::ptr::null(),
        };

        let mut storage = try_box(Self {
            descriptor,
            // Filters are always created with an explicit reference string;
            // the reference GUID is only AVStream's fallback name.
            _reference_guid: sys::guids::KSNAME_Filter,
            _categories: categories,
            _pins: pins,
            _pin_storage: pin_storage,
            _nodes: nodes,
            _node_storage: node_storage,
            _connections: connections,
            _automation: automation,
        })?;
        storage.descriptor.ReferenceGuid = &storage._reference_guid;
        Ok(storage)
    }

    /// The descriptor to hand to AVStream. Valid for as long as the box is.
    pub(crate) fn descriptor(&self) -> *const sys::KSFILTER_DESCRIPTOR {
        &self.descriptor
    }
}
