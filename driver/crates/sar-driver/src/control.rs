// Copyright (c) 2026 Itty Bitty Binaries, Inc.
// SPDX-License-Identifier: MIT

//! The control interface (section 5, A.5).
#![forbid(unsafe_code)]

use alloc::vec::Vec;

use ks::device::Device;
use ntk::{
    alloc_ext::Shared,
    csq::IrpQueue,
    device::DeviceObject,
    irp::{DispatchResult, Irp},
    object::EventObject,
    registry::RegistryCallback,
    section::UserView,
    security::UserSid,
    sync::Mutex,
    work::WorkItem,
};
use ntstatus::{IoStatus, NtStatus};
use sar_core::{
    control::{
        check_input_length, check_output_length, encode_notification, notification_capacity, Request,
        NOTIFICATION_RECORD_SIZE,
    },
    endpoint::{EndpointRequest, WideString},
    layout::{LayoutReply, LayoutRequest},
    notify::{Notification, NotificationQueue, WaitDisposition},
    registry::FILTER_ALTITUDE,
};

use crate::{
    device::{Sar, DRIVER},
    endpoint::Endpoint,
    publish::{PublishJob, PublishWork},
    regfilter::RoutingFilter,
    section::SharedSection,
};

/// Alignment the "neither"-method requests require of the caller's
/// pointers (A.3).
const USER_BUFFER_ALIGNMENT: u32 = 4;

/// A control handle's notification queue: the events clients registered,
/// and the host's parked wait requests, under one lock.
pub type NotifyQueue = IrpQueue<NotificationQueue<EventObject>>;

struct ControlState {
    /// Set when the last handle has been closed.
    closing: bool,
    /// The section, once the buffer layout has been set.
    section: Option<Shared<SharedSection>>,
    /// The host's mapping of the whole section.
    host_view: Option<UserView>,
    endpoints: Vec<Shared<Endpoint>>,
}

/// One open of the control interface.
pub struct ControlHandle {
    state: Mutex<ControlState>,
    notifications: Shared<NotifyQueue>,
}

impl ControlHandle {
    pub fn try_new() -> Result<Shared<Self>, NtStatus> {
        Shared::try_new(Self {
            state: Mutex::try_new(ControlState {
                closing: false,
                section: None,
                host_view: None,
                endpoints: Vec::new(),
            })?,
            notifications: Shared::try_new(IrpQueue::try_new(NotificationQueue::new())?)?,
        })
    }

    /// The last handle to this open has been closed (section 11).
    pub fn close(&self, device: &DeviceObject) {
        // 1. New control requests fail from here on.
        let (endpoints, host_view) = {
            let mut state = self.state.lock();
            state.closing = true;
            (core::mem::take(&mut state.endpoints), state.host_view.take())
        };

        // 2. Each endpoint is withdrawn.
        if let Some(device) = Device::of(device) {
            let guard = device.lock();
            for endpoint in &endpoints {
                endpoint.withdraw(&guard);
            }
        }
        drop(endpoints);

        // 3. Pending waits are cancelled and uncollected events dropped.
        // Closing the queue first means no wait can be parked afterwards.
        let uncollected = self.notifications.with_state(NotificationQueue::close);
        while let Some(wait) = self.notifications.pop() {
            wait.complete(NtStatus::CANCELLED);
        }
        drop(uncollected);

        // 4. The section is unmapped from the host, but only if this is the
        // process that mapped it; otherwise the mapping goes away with its
        // process.
        if let Some(view) = host_view {
            if view.is_in_current_process() {
                drop(view);
            } else {
                view.leave_mapped();
            }
        }
    }

    /// Drops an endpoint whose publication failed.
    pub fn forget_endpoint(&self, endpoint: &Shared<Endpoint>) {
        let removed = {
            let mut state = self.state.lock();
            state
                .endpoints
                .iter()
                .position(|candidate| Shared::ptr_eq(candidate, endpoint))
                .map(|position| state.endpoints.swap_remove(position))
        };
        drop(removed);
    }
}

/// A device control request on a control handle.
pub fn dispatch(device: &DeviceObject, control: &Shared<ControlHandle>, irp: Irp) -> DispatchResult {
    if control.state.lock().closing {
        return irp.complete(NtStatus::FILE_CLOSED);
    }
    match Request::decode(irp.control_code()) {
        Some(Request::SetBufferLayout) => set_buffer_layout(control, irp),
        Some(Request::CreateEndpoint) => create_endpoint(device, control, irp),
        Some(Request::WaitNotifications) => wait_notifications(control, irp),
        Some(Request::StartRegistryFilter) => {
            let status = start_registry_filter();
            irp.complete(status)
        }
        // Unknown requests are refused. (The specification lists "unknown
        // control requests succeed" as a gap that should be closed.)
        None => irp.complete(NtStatus::INVALID_DEVICE_REQUEST),
    }
}

/// "Set buffer layout": creates the section and fixes the audio format.
fn set_buffer_layout(control: &ControlHandle, irp: Irp) -> DispatchResult {
    if let Err(refusal) = check_input_length(irp.input_length(), LayoutRequest::SIZE) {
        return irp.complete(refusal);
    }
    let raw = match irp.read_user_input::<{ LayoutRequest::SIZE }>(USER_BUFFER_ALIGNMENT) {
        Ok(raw) => raw,
        Err(status) => return irp.complete(status),
    };
    let layout = match LayoutRequest::parse(&raw).validate() {
        Ok(layout) => layout,
        Err(status) => return irp.complete(status),
    };

    // May the layout be set at all? Checked again when it is committed; the
    // lock is not held in between, because the steps there touch the
    // caller's memory and may take a while.
    let refusal = |state: &ControlState| {
        if state.closing {
            Some(NtStatus::FILE_CLOSED)
        } else if state.section.is_some() {
            Some(NtStatus::INVALID_STATE_TRANSITION)
        } else {
            None
        }
    };
    if let Some(status) = refusal(&control.state.lock()) {
        return irp.complete(status);
    }

    // The reply buffer is checked before anything is created, and a reply
    // that cannot be delivered undoes the layout: the request can then be
    // repeated. (The specification lists "a failed reply leaves the layout
    // set" as a gap.)
    if let Err(refusal) = check_output_length(irp.output_length(), LayoutReply::SIZE) {
        return irp.complete(refusal);
    }
    if let Err(status) = irp.probe_user_output(LayoutReply::SIZE, USER_BUFFER_ALIGNMENT) {
        return irp.complete(IoStatus::new(status, LayoutReply::SIZE));
    }

    let section = match SharedSection::create(layout).and_then(Shared::try_new) {
        Ok(section) => section,
        Err(status) => return irp.complete(status),
    };
    let view = match section.map_whole_into_current_process() {
        Ok(view) => view,
        Err(status) => return irp.complete(status),
    };
    let reply = LayoutReply::new(&layout, view.address()).encode();
    if let Err(status) = irp.write_user_output(&reply, USER_BUFFER_ALIGNMENT) {
        // Dropping the view unmaps it; dropping the section frees it.
        drop(view);
        return irp.complete(IoStatus::new(status, LayoutReply::SIZE));
    }

    let mut state = control.state.lock();
    if let Some(status) = refusal(&state) {
        // The handle closed, or another request set the layout, meanwhile.
        drop(state);
        drop(view);
        return irp.complete(status);
    }
    state.section = Some(section);
    state.host_view = Some(view);
    drop(state);
    irp.complete(IoStatus::new(NtStatus::SUCCESS, LayoutReply::SIZE))
}

/// "Create endpoint": builds the endpoint's filters now and queues its
/// publication; the request completes when that is done.
fn create_endpoint(device: &DeviceObject, control: &Shared<ControlHandle>, irp: Irp) -> DispatchResult {
    if let Err(refusal) = check_input_length(irp.input_length(), EndpointRequest::SIZE) {
        return irp.complete(refusal);
    }
    let raw = match irp.read_user_input::<{ EndpointRequest::SIZE }>(USER_BUFFER_ALIGNMENT) {
        Ok(raw) => raw,
        Err(status) => return irp.complete(status),
    };
    let request = match EndpointRequest::parse(&raw) {
        Ok(request) => request,
        Err(status) => return irp.complete(status),
    };
    let Some(ks_device) = Device::of(device) else {
        return irp.complete(NtStatus::INVALID_DEVICE_REQUEST);
    };
    let Some(device_state) = ks_device.context::<Sar>() else {
        return irp.complete(NtStatus::INVALID_DEVICE_REQUEST);
    };

    // Everything that can fail for lack of memory is obtained before the
    // endpoint exists, so that nothing has to be undone after the request
    // has been marked pending.
    let work = match WorkItem::try_new(ks_device.functional_device_object()) {
        Ok(work) => work,
        Err(status) => return irp.complete(status),
    };

    // Lock order: device mutex, then the control handle, then the queue.
    let guard = ks_device.lock();
    let mut state = control.state.lock();
    if state.closing {
        return irp.complete(NtStatus::FILE_CLOSED);
    }
    let Some(section) = state.section.clone() else {
        return irp.complete(NtStatus::INVALID_STATE_TRANSITION);
    };
    // Two endpoints with one index would share a register record. (The
    // specification lists "indexes are not checked for reuse" as a gap.)
    if state.endpoints.iter().any(|e| e.index() == request.index) {
        return irp.complete(NtStatus::INVALID_PARAMETER);
    }
    let mut queue = device_state.publish_queue.lock();
    if state.endpoints.try_reserve(1).is_err() || queue.try_reserve(1).is_err() {
        return irp.complete(NtStatus::INSUFFICIENT_RESOURCES);
    }

    let endpoint = match Endpoint::create(&guard, &request, section, control.notifications.clone()) {
        Ok(endpoint) => endpoint,
        Err(status) => return irp.complete(status),
    };
    state.endpoints.push(endpoint.clone());
    let (request, result) = irp.pend();
    queue.push_back(PublishJob {
        endpoint,
        control: control.clone(),
        request,
    });
    drop(queue);
    drop(state);
    drop(guard);
    work.queue::<PublishWork>(device_state);
    result
}

/// "Wait for notification events": hands the host the events clients have
/// registered, or parks the request until one arrives.
fn wait_notifications(control: &ControlHandle, mut irp: Irp) -> DispatchResult {
    let capacity = notification_capacity(irp.output_length());

    let mut disposition = WaitDisposition::Pend;
    let mut batch: Vec<Notification<EventObject>> = Vec::new();
    let mut out_of_memory = false;
    // Decided under the queue's lock, so that an event registered at the
    // same moment either is seen here or finds this request parked.
    let parked = control.notifications.park_if(irp, |queue| {
        disposition = queue.wait_disposition(capacity);
        if disposition == WaitDisposition::Deliver {
            let take = capacity.min(queue.len());
            if batch.try_reserve_exact(take).is_err() {
                out_of_memory = true;
            } else {
                while batch.len() < take {
                    match queue.pop() {
                        Some(notification) => batch.push(notification),
                        None => break,
                    }
                }
            }
        }
        disposition == WaitDisposition::Pend
    });
    irp = match parked {
        Ok(pending) => return pending,
        Err(irp) => irp,
    };

    if out_of_memory {
        return irp.complete(NtStatus::INSUFFICIENT_RESOURCES);
    }
    match disposition {
        WaitDisposition::BufferTooSmall => {
            irp.complete(IoStatus::new(NtStatus::BUFFER_TOO_SMALL, NOTIFICATION_RECORD_SIZE))
        }
        WaitDisposition::Cancelled => irp.complete(NtStatus::CANCELLED),
        // `Pend` cannot come back here: the queue parks the request then.
        WaitDisposition::Pend => irp.complete(NtStatus::UNSUCCESSFUL),
        WaitDisposition::Deliver => {
            let output = irp.buffered_output();
            let mut filled = 0;
            for notification in batch {
                // A handle is only ever created on a thread of the process
                // that will own it: this one. An event that cannot be turned
                // into a handle is dropped; the others are still delivered.
                let Ok(handle) = notification.event.open_handle_in_current_process() else {
                    continue;
                };
                let record = encode_notification(handle, notification.generation, notification.index);
                let start = filled * NOTIFICATION_RECORD_SIZE;
                match output.get_mut(start..start + NOTIFICATION_RECORD_SIZE) {
                    Some(slot) => {
                        slot.copy_from_slice(&record);
                        filled += 1;
                    }
                    // Cannot happen: at most `capacity` events were taken.
                    None => break,
                }
            }
            irp.complete(IoStatus::new(NtStatus::SUCCESS, filled * NOTIFICATION_RECORD_SIZE))
        }
    }
}

/// "Start registry filter": turns on application routing support for the
/// user the calling process runs as. At most once per driver load.
fn start_registry_filter() -> NtStatus {
    let Some(driver) = DRIVER.get() else {
        return NtStatus::UNSUCCESSFUL;
    };
    let mut slot = driver.registry_filter.lock();
    if slot.is_some() {
        return NtStatus::RESOURCE_IN_USE;
    }
    let user = match UserSid::of_current_process() {
        Ok(user) => user,
        Err(status) => return status,
    };
    let Some(altitude) = WideString::<16>::from_ascii(FILTER_ALTITUDE) else {
        return NtStatus::UNSUCCESSFUL;
    };
    match RegistryCallback::register(driver.handle, altitude.as_units(), RoutingFilter::new(user)) {
        Ok(callback) => {
            *slot = Some(callback);
            NtStatus::SUCCESS
        }
        Err(status) => status,
    }
}
