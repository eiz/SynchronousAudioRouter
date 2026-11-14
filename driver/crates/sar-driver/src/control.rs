// Copyright (c) 2026 Itty Bitty Binaries, Inc.
// SPDX-License-Identifier: MIT

//! The control interface (section 5, A.5).
#![forbid(unsafe_code)]

use alloc::vec::Vec;
use core::sync::atomic::Ordering;

use ks::device::Device;
use ntk::{
    alloc_ext::Shared,
    csq::IrpQueue,
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
    notify::{Batch, NotificationQueue, WaitDisposition},
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
            notifications: Shared::try_new(IrpQueue::try_new(NotificationQueue::try_new()?)?)?,
        })
    }

    /// The last handle to this open has been closed (section 11).
    pub fn close(&self, request: &Irp) {
        // 1. New control requests fail from here on.
        let (endpoints, host_view) = {
            let mut state = self.state.lock();
            state.closing = true;
            (core::mem::take(&mut state.endpoints), state.host_view.take())
        };

        // 2. Each endpoint is withdrawn, all of them under one hold of the
        // device mutex (section 14). What all of them are told first, which
        // touches memory only, is that they are going: nothing new is begun
        // on them, and a request for an endpoint with the identity of one
        // of them waits for it to go.
        //
        // An endpoint that another thread is still creating or publishing
        // is taken down by that thread. This one waits for it, so that when
        // the close has finished nothing of the handle's is left in kernel
        // streaming; but only after it has withdrawn every endpoint it can
        // withdraw itself and has released the device mutex, because the
        // other thread needs the mutex and may in turn be waiting for one
        // of those to go (`Identities`).
        for endpoint in &endpoints {
            endpoint.announce_withdrawal();
        }
        if let Some(device) = Device::of_request(request) {
            if let Some(device_state) = device.context::<Sar>() {
                {
                    // One hold of the device mutex from the first endpoint
                    // to the last (section 14): Windows sees the host's
                    // endpoints go together, and the close is quick.
                    let guard = device.lock();
                    for endpoint in &endpoints {
                        let _ = endpoint.withdraw(&guard, &device_state.identities);
                    }
                }
                for endpoint in &endpoints {
                    endpoint.wait_withdrawn();
                }
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
pub fn dispatch(control: &Shared<ControlHandle>, irp: Irp) -> DispatchResult {
    if control.state.lock().closing {
        return irp.complete(NtStatus::FILE_CLOSED);
    }
    match Request::decode(irp.control_code()) {
        Some(Request::SetBufferLayout) => set_buffer_layout(control, irp),
        Some(Request::CreateEndpoint) => create_endpoint(control, irp),
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
    let refused = refusal(&control.state.lock());
    if let Some(status) = refused {
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
fn create_endpoint(control: &Shared<ControlHandle>, irp: Irp) -> DispatchResult {
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
    // The device is borrowed from the request: it is not used once the
    // request has been completed or handed on.
    let Some(ks_device) = Device::of_request(&irp) else {
        return irp.complete(NtStatus::INVALID_DEVICE_REQUEST);
    };
    let Some(device_state) = ks_device.context::<Sar>() else {
        return irp.complete(NtStatus::INVALID_DEVICE_REQUEST);
    };

    // The work item is obtained before the endpoint exists, so that queuing
    // it cannot fail once the request has been marked pending.
    let work = match WorkItem::try_new(ks_device.functional_device_object()) {
        Ok(work) => work,
        Err(status) => return irp.complete(status),
    };

    // May the endpoint be created at all? Decided under the control
    // handle's lock, which is then released: creating the filter factories
    // takes tens of milliseconds, and other requests on this handle (a
    // "wait for notification events" above all) are not to be held up by
    // it (section 14).
    //
    // Two endpoints with one index would share a register record. (The
    // specification lists "indexes are not checked for reuse" as a gap.)
    let refusal = |state: &ControlState| {
        if state.closing {
            Some(NtStatus::FILE_CLOSED)
        } else if state.section.is_none() {
            Some(NtStatus::INVALID_STATE_TRANSITION)
        } else if state.endpoints.iter().any(|e| e.index() == request.index) {
            Some(NtStatus::INVALID_PARAMETER)
        } else {
            None
        }
    };
    let section = {
        let state = control.state.lock();
        match refusal(&state) {
            Some(status) => Err(status),
            None => state.section.clone().ok_or(NtStatus::INVALID_STATE_TRANSITION),
        }
    };
    let section = match section {
        Ok(section) => section,
        Err(status) => return irp.complete(status),
    };
    let endpoint = match Endpoint::try_new(&request, section, control.notifications.clone()) {
        Ok(endpoint) => endpoint,
        Err(status) => return irp.complete(status),
    };

    // The endpoint joins the handle's endpoints before its factories exist.
    // That reserves its index, and it lets a close of the handle that comes
    // while the factories are being created see the endpoint and wait for
    // it to be undone.
    let refused = {
        let mut state = control.state.lock();
        let refused = refusal(&state).or_else(|| {
            state
                .endpoints
                .try_reserve(1)
                .is_err()
                .then_some(NtStatus::INSUFFICIENT_RESOURCES)
        });
        if refused.is_none() {
            state.endpoints.push(endpoint.clone());
        }
        refused
    };
    if let Some(status) = refused {
        return irp.complete(status);
    }

    // The factories are created with no lock of the driver's held.
    if let Err(status) = Endpoint::create_factories(&endpoint, ks_device, &device_state.identities) {
        control.forget_endpoint(&endpoint);
        return irp.complete(status);
    }

    // The request is marked pending and queued in one step under the
    // queue's lock, so that requests are published in the order in which
    // they got here.
    let mut queue = device_state.publish_queue.lock();
    if queue.jobs.try_reserve(1).is_err() {
        drop(queue);
        if !endpoint.withdraw(&ks_device.lock(), &device_state.identities) {
            // The control handle is closing and has taken it over.
            endpoint.wait_withdrawn();
        }
        control.forget_endpoint(&endpoint);
        return irp.complete(NtStatus::INSUFFICIENT_RESOURCES);
    }
    let (request, result) = irp.pend();
    queue.jobs.push_back(PublishJob {
        endpoint,
        control: control.clone(),
        request,
    });
    drop(queue);
    work.queue::<PublishWork>(device_state);
    result
}

/// "Wait for notification events": hands the host the events clients have
/// registered, or parks the request until one arrives.
fn wait_notifications(control: &ControlHandle, mut irp: Irp) -> DispatchResult {
    let capacity = notification_capacity(irp.output_length());

    let mut disposition = WaitDisposition::Pend;
    // Events come out of the queue a batch at a time. The batch is on this
    // thread's stack and of a fixed size: the queue's lock is a spin lock
    // the host's real-time thread takes, and nothing is allocated, and no
    // more than a batch copied, while it is held (section 14).
    let mut batch: Batch<EventObject> = Batch::new();
    // Decided under the queue's lock, so that an event registered at the
    // same moment either is seen here or finds this request parked.
    let parked = control.notifications.park_if(irp, |queue| {
        disposition = queue.wait_disposition(capacity);
        if disposition == WaitDisposition::Deliver {
            queue.take(&mut batch, capacity);
        }
        disposition == WaitDisposition::Pend
    });
    irp = match parked {
        Ok(pending) => return pending,
        Err(irp) => irp,
    };

    match disposition {
        WaitDisposition::BufferTooSmall => {
            irp.complete(IoStatus::new(NtStatus::BUFFER_TOO_SMALL, NOTIFICATION_RECORD_SIZE))
        }
        WaitDisposition::Cancelled => irp.complete(NtStatus::CANCELLED),
        // `Pend` cannot come back here: the queue parks the request then.
        WaitDisposition::Pend => irp.complete(NtStatus::UNSUCCESSFUL),
        WaitDisposition::Deliver => {
            let output = irp.buffered_output();
            let mut taken = 0;
            let mut filled = 0;
            loop {
                taken += batch.len();
                // The lock is not held here: handles are created, and the
                // chunk of the queue the batch emptied, if any, is freed.
                for notification in batch.drain() {
                    // A handle is only ever created on a thread of the
                    // process that will own it: this one. An event that
                    // cannot be turned into a handle is dropped; the others
                    // are still delivered.
                    let Ok(handle) = notification.event.open_handle_in_current_process() else {
                        continue;
                    };
                    let record = encode_notification(handle, notification.generation, notification.index);
                    let start = filled * NOTIFICATION_RECORD_SIZE;
                    // Cannot fail: at most `capacity` events are taken.
                    if let Some(slot) = output.get_mut(start..start + NOTIFICATION_RECORD_SIZE) {
                        slot.copy_from_slice(&record);
                        filled += 1;
                    }
                }
                // "As many as fit": while the caller's buffer has room and
                // events are waiting, take another batch, under a hold of
                // the lock of its own.
                if taken >= capacity {
                    break;
                }
                control
                    .notifications
                    .with_state(|queue| queue.take(&mut batch, capacity - taken));
                if batch.is_empty() {
                    break;
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
    // Registering the callback is a call into the registry, and reading the
    // caller's user opens its token: neither is made under a lock (section
    // 14). What keeps the filter from being started twice is a claim that
    // the first request stakes and keeps if it succeeds. A request that
    // arrives while another is still registering the callback is answered
    // like one that arrives afterwards.
    if driver.registry_filter_claimed.swap(true, Ordering::AcqRel) {
        return NtStatus::RESOURCE_IN_USE;
    }
    let started = UserSid::of_current_process().and_then(|user| {
        let altitude = WideString::<16>::from_ascii(FILTER_ALTITUDE).ok_or(NtStatus::UNSUCCESSFUL)?;
        RegistryCallback::register(driver.handle, altitude.as_units(), RoutingFilter::new(user))
    });
    match started {
        Ok(callback) => {
            let previous = driver.registry_filter.lock().replace(callback);
            drop(previous);
            NtStatus::SUCCESS
        }
        Err(status) => {
            // Nothing was started: the next request may try.
            driver.registry_filter_claimed.store(false, Ordering::Release);
            status
        }
    }
}
