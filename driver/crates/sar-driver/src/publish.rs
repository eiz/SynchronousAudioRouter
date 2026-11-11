// Copyright (c) 2026 Itty Bitty Binaries, Inc.
// SPDX-License-Identifier: MIT

//! Publishing endpoints on a system worker thread (section 5, "Create
//! endpoint").
//!
//! A create request is answered as pending as soon as the endpoint's filter
//! factories exist. Writing the interfaces' registry values and switching
//! the interfaces on happens here, one endpoint at a time and in the order
//! the requests were made; then the request is completed.
#![forbid(unsafe_code)]

use ks::device::Device;
use ntk::{alloc_ext::Shared, device::DeviceObject, irp::PendingIrp, work::WorkRoutine};
use ntstatus::NtStatus;

use crate::{control::ControlHandle, device::DeviceState, endpoint::Endpoint};

/// One endpoint waiting to be published, and the request to complete when
/// it has been.
pub struct PublishJob {
    pub endpoint: Shared<Endpoint>,
    pub control: Shared<ControlHandle>,
    pub request: PendingIrp,
}

pub struct PublishWork;

impl WorkRoutine for PublishWork {
    type Context = DeviceState;

    /// Publishes the oldest waiting endpoint. One work item is queued per
    /// create request, so each run finds exactly one job; taking the oldest
    /// rather than "its own" keeps completion in request order no matter
    /// how the worker threads are scheduled.
    fn run(device: &DeviceObject, state: &DeviceState) {
        let Some(job) = state.publish_queue.lock().pop_front() else {
            return;
        };
        let status = match Device::of(device) {
            Some(device) => {
                let guard = device.lock();
                let result = job.endpoint.publish(&guard);
                if result.is_err() {
                    // "... or with the failure that stopped it, in which
                    // case no endpoint exists."
                    job.endpoint.withdraw(&guard);
                    job.control.forget_endpoint(&job.endpoint);
                }
                result
            }
            None => Err(NtStatus::UNSUCCESSFUL),
        };
        job.request.complete(match status {
            Ok(()) => NtStatus::SUCCESS,
            Err(status) => status,
        });
    }
}
