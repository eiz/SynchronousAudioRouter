// Copyright (c) 2026 Itty Bitty Binaries, Inc.
// SPDX-License-Identifier: MIT

//! Publishing endpoints on a system worker thread (section 5, "Create
//! endpoint").
//!
//! A create request is answered as pending as soon as the endpoint's filter
//! factories exist. Writing the interfaces' registry values and switching
//! the interfaces on happens here, one endpoint at a time and in the order
//! the requests were made; then the request is completed.
//!
//! None of that is done under a lock, of the driver's or of kernel
//! streaming's (section 14). What keeps the endpoints in order is that only
//! one worker thread publishes at a time.
#![forbid(unsafe_code)]

use alloc::collections::VecDeque;

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

/// The endpoints waiting to be published.
pub struct PublishQueue {
    /// Oldest first.
    pub jobs: VecDeque<PublishJob>,
    /// A worker thread is publishing. It goes on until the queue is empty,
    /// so a work item that finds this set has nothing to do.
    pub draining: bool,
}

pub struct PublishWork;

impl WorkRoutine for PublishWork {
    type Context = DeviceState;

    /// Publishes waiting endpoints, oldest first, until none is left. One
    /// work item is queued per create request. Whichever runs while no
    /// other is publishing takes the jobs, its own and those that arrive
    /// meanwhile, one after the other; that keeps completion in request
    /// order no matter how the worker threads are scheduled, without a
    /// lock being held while an endpoint is published.
    fn run(_device: &DeviceObject, state: &DeviceState) {
        loop {
            let job = {
                let mut queue = state.publish_queue.lock();
                if queue.draining {
                    return;
                }
                let Some(job) = queue.jobs.pop_front() else {
                    return;
                };
                queue.draining = true;
                job
            };
            // The device is borrowed from the job's request, which is not
            // completed until the endpoint has been published.
            let status = match Device::of_request(&job.request) {
                Some(device) => {
                    let result = job.endpoint.publish(device, &state.identities);
                    if result.is_err() {
                        // "... or with the failure that stopped it, in which
                        // case no endpoint exists."
                        job.control.forget_endpoint(&job.endpoint);
                    }
                    result
                }
                None => Err(NtStatus::UNSUCCESSFUL),
            };
            // The request is completed before the next endpoint is taken.
            let PublishJob { endpoint, control, request } = job;
            request.complete(match status {
                Ok(()) => NtStatus::SUCCESS,
                Err(status) => status,
            });
            drop(endpoint);
            drop(control);
            state.publish_queue.lock().draining = false;
        }
    }
}
