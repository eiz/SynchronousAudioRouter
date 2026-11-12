// Copyright (c) 2026 Itty Bitty Binaries, Inc.
// SPDX-License-Identifier: MIT

//! The descriptions of an endpoint's two filters (section 7, A.9), and the
//! kernel streaming callbacks, which all forward to [`Endpoint`].
#![forbid(unsafe_code)]

use ks::{
    descriptor::{
        Automation, Communication, Connection, DataFlow, DataRange, EventItem, EventSet, FilterSpec, Interface,
        NodeSpec, PinSpec, PropertyItem, PropertySet,
    },
    filter::{DataFormat, FilterHandler, Pin, PropertyCall, Target},
    guids, ids,
};
use ntk::{guid_to_bytes, Guid};
use ntstatus::{IoStatus, NtStatus};
use sar_core::{
    endpoint::Direction,
    format::{self, PinAudioRange},
};

use crate::endpoint::Endpoint;

// The format GUIDs sar-core compares against are the ones in ksmedia.h.
const _: () = {
    assert!(bytes_eq(guid_to_bytes(&guids::KSDATAFORMAT_TYPE_AUDIO), format::TYPE_AUDIO));
    assert!(bytes_eq(guid_to_bytes(&guids::KSDATAFORMAT_SUBTYPE_PCM), format::SUBTYPE_PCM));
    assert!(bytes_eq(
        guid_to_bytes(&guids::KSDATAFORMAT_SPECIFIER_WAVEFORMATEX),
        format::SPECIFIER_WAVEFORMATEX
    ));
};

// The minimum sizes A.9 lists are the sizes of the structures in ks.h and
// ksmedia.h for a 64-bit driver.
const _: () = {
    assert!(ids::size::KSPROPERTY == 24);
    assert!(ids::size::KSP_PIN == 32);
    assert!(ids::size::KSNODEPROPERTY == 32);
    assert!(ids::size::KSPIN_CINSTANCES == 8);
    assert!(ids::size::KSAUDIO_CHANNEL_CONFIG == 4);
    assert!(ids::size::KSRTAUDIO_BUFFER_PROPERTY == 40);
    assert!(ids::size::KSRTAUDIO_BUFFER_PROPERTY_WITH_NOTIFICATION == 40);
    assert!(ids::size::KSRTAUDIO_BUFFER == 16);
    assert!(ids::size::KSRTAUDIO_HWREGISTER_PROPERTY == 32);
    assert!(ids::size::KSRTAUDIO_HWREGISTER == 40);
    assert!(ids::size::KSRTAUDIO_HWLATENCY == 12);
    assert!(ids::size::KSRTAUDIO_NOTIFICATION_EVENT_PROPERTY == 32);
    assert!(ids::size::KSAUDIO_PRESENTATION_POSITION == 16);
    assert!(ids::size::KSEVENTDATA == 32);
    assert!(ids::size::BOOL == 4);
};

// Only used by the constant assertions above, which the dead-code lint
// does not count.
#[allow(dead_code)]
const fn bytes_eq(a: [u8; 16], b: [u8; 16]) -> bool {
    let mut i = 0;
    while i < 16 {
        if a[i] != b[i] {
            return false;
        }
        i += 1;
    }
    true
}

/// Whether two GUIDs are equal.
pub fn same_guid(a: &Guid, b: &Guid) -> bool {
    guid_to_bytes(a) == guid_to_bytes(b)
}

const fn get(id: u32, min_property: u32, min_data: u32) -> PropertyItem {
    PropertyItem { id, get: true, set: false, min_property, min_data }
}

const fn get_set(id: u32, min_property: u32, min_data: u32) -> PropertyItem {
    PropertyItem { id, get: true, set: true, min_property, min_data }
}

/// Wave filter, filter level: the standard pin property set (A.9).
const WAVE_FILTER_PIN_PROPERTIES: [PropertyItem; 3] = [
    get(ids::KSPROPERTY_PIN_GLOBALCINSTANCES, ids::size::KSP_PIN, ids::size::KSPIN_CINSTANCES),
    get_set(ids::KSPROPERTY_PIN_PROPOSEDATAFORMAT, ids::size::KSP_PIN, 0),
    // "standard": the sizes of ks.h's DEFINE_KSPROPERTY_ITEM_PIN_PHYSICALCONNECTION.
    get(ids::KSPROPERTY_PIN_PHYSICALCONNECTION, ids::size::KSP_PIN, 0),
];

/// Wave filter, node 0: the standard audio property set.
const WAVE_NODE_AUDIO_PROPERTIES: [PropertyItem; 1] = [get_set(
    ids::KSPROPERTY_AUDIO_CHANNEL_CONFIG,
    ids::size::KSNODEPROPERTY,
    ids::size::KSAUDIO_CHANNEL_CONFIG,
)];

/// Wave filter, pin 0: the WaveRT property set.
const WAVE_PIN_RTAUDIO_PROPERTIES: [PropertyItem; 10] = [
    get(ids::KSPROPERTY_RTAUDIO_BUFFER, ids::size::KSRTAUDIO_BUFFER_PROPERTY, ids::size::KSRTAUDIO_BUFFER),
    get(
        ids::KSPROPERTY_RTAUDIO_BUFFER_WITH_NOTIFICATION,
        ids::size::KSRTAUDIO_BUFFER_PROPERTY_WITH_NOTIFICATION,
        ids::size::KSRTAUDIO_BUFFER,
    ),
    get(
        ids::KSPROPERTY_RTAUDIO_POSITIONREGISTER,
        ids::size::KSRTAUDIO_HWREGISTER_PROPERTY,
        ids::size::KSRTAUDIO_HWREGISTER,
    ),
    get(
        ids::KSPROPERTY_RTAUDIO_CLOCKREGISTER,
        ids::size::KSRTAUDIO_HWREGISTER_PROPERTY,
        ids::size::KSRTAUDIO_HWREGISTER,
    ),
    get(ids::KSPROPERTY_RTAUDIO_HWLATENCY, ids::size::KSPROPERTY, ids::size::KSRTAUDIO_HWLATENCY),
    get(ids::KSPROPERTY_RTAUDIO_QUERY_NOTIFICATION_SUPPORT, ids::size::KSPROPERTY, ids::size::BOOL),
    get_set(
        ids::KSPROPERTY_RTAUDIO_REGISTER_NOTIFICATION_EVENT,
        ids::size::KSRTAUDIO_NOTIFICATION_EVENT_PROPERTY,
        0,
    ),
    get_set(
        ids::KSPROPERTY_RTAUDIO_UNREGISTER_NOTIFICATION_EVENT,
        ids::size::KSRTAUDIO_NOTIFICATION_EVENT_PROPERTY,
        0,
    ),
    get(ids::KSPROPERTY_RTAUDIO_PACKETCOUNT, ids::size::KSPROPERTY, ids::size::ULONG),
    get(
        ids::KSPROPERTY_RTAUDIO_PRESENTATION_POSITION,
        ids::size::KSPROPERTY,
        ids::size::KSAUDIO_PRESENTATION_POSITION,
    ),
];

/// Topology filter, filter level: the standard pin property set.
const TOPOLOGY_FILTER_PIN_PROPERTIES: [PropertyItem; 2] = [
    get_set(ids::KSPROPERTY_PIN_NAME, ids::size::KSP_PIN, 0),
    get(ids::KSPROPERTY_PIN_PHYSICALCONNECTION, ids::size::KSP_PIN, 0),
];

/// The "pin capabilities changed" event, declared on the wave filter and on
/// its streaming pin. The driver never raises it.
const PINCAPS_EVENTS: [EventItem; 1] = [EventItem {
    id: ids::KSEVENT_PINCAPS_FORMATCHANGE,
    min_data: ids::size::KSEVENTDATA,
}];

const ANALOG_RANGE: DataRange = DataRange::Plain {
    major: guids::KSDATAFORMAT_TYPE_AUDIO,
    sub: guids::KSDATAFORMAT_SUBTYPE_ANALOG,
    specifier: guids::KSDATAFORMAT_SPECIFIER_NONE,
};

/// Flags on both pins of the wave filter (A.9).
const WAVE_PIN_FLAGS: u32 = ids::KSPIN_FLAG_DO_NOT_INITIATE_PROCESSING
    | ids::KSPIN_FLAG_FRAMES_NOT_REQUIRED_FOR_PROCESSING
    | ids::KSPIN_FLAG_PROCESS_IF_ANY_IN_RUN_STATE
    | ids::KSPIN_FLAG_FIXED_FORMAT
    | ids::KSPIN_FLAG_DO_NOT_USE_STANDARD_TRANSPORT;

/// Pin and node numbers of the two filters.
pub mod pin {
    /// Wave filter: the streaming pin clients open.
    pub const WAVE_STREAMING: u32 = 0;
    /// Wave filter: the bridge to the topology filter.
    pub const WAVE_BRIDGE: u32 = 1;
    /// Topology filter: the bridge to the wave filter.
    pub const TOPOLOGY_BRIDGE: u32 = 0;
    /// Topology filter: the jack.
    pub const TOPOLOGY_JACK: u32 = 1;
    /// Wave filter: the converter node.
    pub const WAVE_NODE: u32 = 0;
}

/// A node's input and output pin numbers in topology connections.
const NODE_IN: u32 = 1;
const NODE_OUT: u32 = 0;

/// The data flow of the pin clients (or the wave filter) feed for playback.
fn inward(direction: Direction) -> (DataFlow, DataFlow) {
    match direction {
        // (pin 0, pin 1)
        Direction::Playback => (DataFlow::In, DataFlow::Out),
        Direction::Recording => (DataFlow::Out, DataFlow::In),
    }
}

/// Builds the wave filter's description and hands it to `f`.
pub fn with_wave_spec<R>(direction: Direction, range: PinAudioRange, f: impl FnOnce(&FilterSpec<'_>) -> R) -> R {
    let (streaming_flow, bridge_flow) = inward(direction);
    let playback = direction == Direction::Playback;

    let categories = [
        guids::KSCATEGORY_AUDIO,
        guids::KSCATEGORY_REALTIME,
        if playback { guids::KSCATEGORY_RENDER } else { guids::KSCATEGORY_CAPTURE },
    ];
    let interfaces = [Interface {
        set: guids::KSINTERFACESETID_Standard,
        id: ids::KSINTERFACE_STANDARD_LOOPED_STREAMING,
    }];
    let digital = [DataRange::Audio {
        major: guids::KSDATAFORMAT_TYPE_AUDIO,
        sub: guids::KSDATAFORMAT_SUBTYPE_PCM,
        specifier: guids::KSDATAFORMAT_SPECIFIER_WAVEFORMATEX,
        maximum_channels: range.maximum_channels,
        minimum_bits_per_sample: range.minimum_bits_per_sample,
        maximum_bits_per_sample: range.maximum_bits_per_sample,
        minimum_sample_frequency: range.minimum_sample_frequency,
        maximum_sample_frequency: range.maximum_sample_frequency,
    }];
    let analog = [ANALOG_RANGE];
    let rtaudio = [PropertySet { set: guids::KSPROPSETID_RtAudio, items: &WAVE_PIN_RTAUDIO_PROPERTIES }];
    let pincaps = [EventSet { set: guids::KSEVENTSETID_PinCapsChange, items: &PINCAPS_EVENTS }];
    let pins = [
        PinSpec {
            streaming: true,
            interfaces: &interfaces,
            data_ranges: &digital,
            data_flow: streaming_flow,
            communication: Communication::Sink,
            category: Some(guids::KSCATEGORY_AUDIO),
            flags: WAVE_PIN_FLAGS | if playback { ids::KSPIN_FLAG_RENDERER } else { 0 },
            instances_possible: 1,
            instances_necessary: 0,
            intersect: true,
            automation: Automation { properties: &rtaudio, events: &pincaps },
        },
        PinSpec {
            streaming: false,
            interfaces: &[],
            data_ranges: &analog,
            data_flow: bridge_flow,
            communication: Communication::None,
            category: Some(guids::KSCATEGORY_AUDIO),
            flags: WAVE_PIN_FLAGS,
            instances_possible: 1,
            instances_necessary: 0,
            intersect: false,
            automation: Automation::NONE,
        },
    ];
    let node_properties = [PropertySet { set: guids::KSPROPSETID_Audio, items: &WAVE_NODE_AUDIO_PROPERTIES }];
    let nodes = [NodeSpec {
        kind: if playback { guids::KSNODETYPE_DAC } else { guids::KSNODETYPE_ADC },
        automation: Automation { properties: &node_properties, events: &[] },
    }];
    // Playback: pin 0 -> node -> pin 1. Recording: pin 1 -> node -> pin 0.
    let (source, sink) = if playback {
        (pin::WAVE_STREAMING, pin::WAVE_BRIDGE)
    } else {
        (pin::WAVE_BRIDGE, pin::WAVE_STREAMING)
    };
    let connections = [
        Connection { from_node: ids::KSFILTER_NODE, from_pin: source, to_node: 0, to_pin: NODE_IN },
        Connection { from_node: 0, from_pin: NODE_OUT, to_node: ids::KSFILTER_NODE, to_pin: sink },
    ];
    let filter_properties = [PropertySet { set: guids::KSPROPSETID_Pin, items: &WAVE_FILTER_PIN_PROPERTIES }];
    f(&FilterSpec {
        categories: &categories,
        pins: &pins,
        nodes: &nodes,
        connections: &connections,
        automation: Automation { properties: &filter_properties, events: &pincaps },
    })
}

/// Builds the topology filter's description and hands it to `f`.
pub fn with_topology_spec<R>(direction: Direction, f: impl FnOnce(&FilterSpec<'_>) -> R) -> R {
    let (bridge_flow, jack_flow) = inward(direction);
    let playback = direction == Direction::Playback;

    let categories = [guids::KSCATEGORY_AUDIO, guids::KSCATEGORY_TOPOLOGY];
    let analog = [ANALOG_RANGE];
    let bridge_or_jack = |data_flow, category| PinSpec {
        streaming: false,
        interfaces: &[],
        data_ranges: &analog,
        data_flow,
        communication: Communication::None,
        category: Some(category),
        flags: 0,
        instances_possible: 0,
        instances_necessary: 0,
        intersect: false,
        automation: Automation::NONE,
    };
    let pins = [
        bridge_or_jack(bridge_flow, guids::KSCATEGORY_AUDIO),
        bridge_or_jack(
            jack_flow,
            if playback { guids::KSNODETYPE_SPEAKER } else { guids::KSNODETYPE_LINE_CONNECTOR },
        ),
    ];
    // Volume, then mute, in the direction of signal flow. Neither has
    // properties: they describe controls the endpoint does not have.
    let nodes = [
        NodeSpec { kind: guids::KSNODETYPE_VOLUME, automation: Automation::NONE },
        NodeSpec { kind: guids::KSNODETYPE_MUTE, automation: Automation::NONE },
    ];
    let (source, sink) = if playback {
        (pin::TOPOLOGY_BRIDGE, pin::TOPOLOGY_JACK)
    } else {
        (pin::TOPOLOGY_JACK, pin::TOPOLOGY_BRIDGE)
    };
    let connections = [
        Connection { from_node: ids::KSFILTER_NODE, from_pin: source, to_node: 0, to_pin: NODE_IN },
        Connection { from_node: 0, from_pin: NODE_OUT, to_node: 1, to_pin: NODE_IN },
        Connection { from_node: 1, from_pin: NODE_OUT, to_node: ids::KSFILTER_NODE, to_pin: sink },
    ];
    let filter_properties = [PropertySet { set: guids::KSPROPSETID_Pin, items: &TOPOLOGY_FILTER_PIN_PROPERTIES }];
    f(&FilterSpec {
        categories: &categories,
        pins: &pins,
        nodes: &nodes,
        connections: &connections,
        automation: Automation { properties: &filter_properties, events: &[] },
    })
}

/// The device interface categories of the two filters, for writing the
/// per-interface registry values (A.10).
pub fn wave_categories(direction: Direction) -> [Guid; 3] {
    [
        guids::KSCATEGORY_AUDIO,
        guids::KSCATEGORY_REALTIME,
        match direction {
            Direction::Playback => guids::KSCATEGORY_RENDER,
            Direction::Recording => guids::KSCATEGORY_CAPTURE,
        },
    ]
}

pub fn topology_categories() -> [Guid; 2] {
    [guids::KSCATEGORY_AUDIO, guids::KSCATEGORY_TOPOLOGY]
}

/// Kernel streaming callbacks of an endpoint's wave filter.
pub struct WaveFilter;

impl FilterHandler for WaveFilter {
    type Context = Endpoint;

    fn filter_create(endpoint: &Endpoint) -> Result<(), NtStatus> {
        endpoint.check_can_open_filter()
    }

    fn filter_close(_endpoint: &Endpoint) {}

    fn pin_create(endpoint: &Endpoint, pin: &Pin) -> Result<(), NtStatus> {
        endpoint.open_stream(pin)
    }

    fn pin_close(endpoint: &Endpoint, pin: &Pin) {
        endpoint.close_stream(pin);
    }

    fn pin_set_state(endpoint: &Endpoint, pin: &Pin, to: u32, from: u32) -> Result<(), NtStatus> {
        endpoint.set_stream_state(pin, to, from)
    }

    fn property(endpoint: &Endpoint, target: Target<'_>, call: &PropertyCall<'_>, data: &mut [u8]) -> IoStatus {
        endpoint.wave_property(target, call, data)
    }

    fn intersect(endpoint: &Endpoint, pin_id: u32, caller_range: &[u8]) -> Result<DataFormat, NtStatus> {
        endpoint.intersect(pin_id, caller_range)
    }
}

/// Kernel streaming callbacks of an endpoint's topology filter. It has no
/// streaming pin.
pub struct TopologyFilter;

impl FilterHandler for TopologyFilter {
    type Context = Endpoint;

    fn filter_create(endpoint: &Endpoint) -> Result<(), NtStatus> {
        endpoint.check_can_open_filter()
    }

    fn filter_close(_endpoint: &Endpoint) {}

    fn pin_create(_endpoint: &Endpoint, _pin: &Pin) -> Result<(), NtStatus> {
        Err(NtStatus::NOT_SUPPORTED)
    }

    fn pin_close(_endpoint: &Endpoint, _pin: &Pin) {}

    fn pin_set_state(_endpoint: &Endpoint, _pin: &Pin, _to: u32, _from: u32) -> Result<(), NtStatus> {
        Err(NtStatus::NOT_SUPPORTED)
    }

    fn property(endpoint: &Endpoint, _target: Target<'_>, call: &PropertyCall<'_>, data: &mut [u8]) -> IoStatus {
        endpoint.topology_property(call, data)
    }

    fn intersect(_endpoint: &Endpoint, _pin_id: u32, _caller_range: &[u8]) -> Result<DataFormat, NtStatus> {
        Err(NtStatus::NO_MATCH)
    }
}
