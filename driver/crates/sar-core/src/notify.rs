// Copyright (c) 2026 Itty Bitty Binaries, Inc.
// SPDX-License-Identifier: MIT

//! The notification queue of one control handle (sections 5 and 9, A.5).
//!
//! This is the state the driver keeps under the queue's lock: the events
//! clients registered that the host has not collected yet, and whether the
//! control handle has closed. The pending wait requests themselves are I/O
//! request packets and live in the kernel's cancel-safe queue; the driver
//! consults [`NotificationQueue::wait_disposition`] under the same lock to
//! decide whether a wait may be parked.
#![forbid(unsafe_code)]

use alloc::collections::VecDeque;

use ntstatus::NtStatus;

/// Most events that may wait for the host at one time. The specification
/// sets no bound; without one a client could grow kernel memory without
/// limit by registering events the host never collects. Eight per possible
/// endpoint is far above anything a working host leaves uncollected.
pub const MAX_QUEUED_EVENTS: usize = 8 * crate::layout::MAX_ENDPOINTS as usize;

/// One registered event waiting for the host.
#[derive(Debug, PartialEq, Eq)]
pub struct Notification<E> {
    /// The event object (in the driver, a referenced kernel event).
    pub event: E,
    /// The endpoint's `generation` register when the client registered.
    pub generation: u32,
    /// The endpoint's index.
    pub index: u32,
}

/// What to do with a "wait for notification events" request (A.5).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum WaitDisposition {
    /// The output buffer cannot hold one record.
    BufferTooSmall,
    /// Events are queued: complete now with as many as fit.
    Deliver,
    /// Nothing queued and the queue is closed.
    Cancelled,
    /// Nothing queued: park the request.
    Pend,
}

#[derive(Debug)]
pub struct NotificationQueue<E> {
    events: VecDeque<Notification<E>>,
    closed: bool,
}

impl<E> NotificationQueue<E> {
    pub const fn new() -> Self {
        Self {
            events: VecDeque::new(),
            closed: false,
        }
    }

    pub fn is_closed(&self) -> bool {
        self.closed
    }

    pub fn len(&self) -> usize {
        self.events.len()
    }

    pub fn is_empty(&self) -> bool {
        self.events.is_empty()
    }

    /// Queues an event. Fails with `STATUS_CANCELLED` once the control
    /// handle has closed, and with `STATUS_INSUFFICIENT_RESOURCES` if the
    /// queue is full or cannot grow; the notification is handed back so the
    /// caller can release the event outside the lock.
    pub fn push(&mut self, notification: Notification<E>) -> Result<(), (NtStatus, Notification<E>)> {
        if self.closed {
            return Err((NtStatus::CANCELLED, notification));
        }
        if self.events.len() >= MAX_QUEUED_EVENTS || self.events.try_reserve(1).is_err() {
            return Err((NtStatus::INSUFFICIENT_RESOURCES, notification));
        }
        self.events.push_back(notification);
        Ok(())
    }

    /// The oldest queued event.
    pub fn pop(&mut self) -> Option<Notification<E>> {
        self.events.pop_front()
    }

    /// Decides the fate of a wait request whose buffer holds `capacity`
    /// records, in the order of the checks in A.5.
    pub fn wait_disposition(&self, capacity: usize) -> WaitDisposition {
        if capacity == 0 {
            WaitDisposition::BufferTooSmall
        } else if !self.events.is_empty() {
            WaitDisposition::Deliver
        } else if self.closed {
            WaitDisposition::Cancelled
        } else {
            WaitDisposition::Pend
        }
    }

    /// Marks the queue closed and hands back everything nobody collected.
    /// Later pushes fail.
    pub fn close(&mut self) -> VecDeque<Notification<E>> {
        self.closed = true;
        core::mem::take(&mut self.events)
    }
}

impl<E> Default for NotificationQueue<E> {
    fn default() -> Self {
        Self::new()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn n(event: u32) -> Notification<u32> {
        Notification { event, generation: event * 2, index: event }
    }

    #[test]
    fn fifo() {
        let mut q = NotificationQueue::new();
        assert!(q.is_empty());
        for i in 0..5 {
            q.push(n(i)).unwrap();
        }
        assert_eq!(q.len(), 5);
        for i in 0..5 {
            assert_eq!(q.pop(), Some(n(i)));
        }
        assert_eq!(q.pop(), None);
    }

    #[test]
    fn dispositions_follow_a5_order() {
        let mut q = NotificationQueue::new();
        assert_eq!(q.wait_disposition(0), WaitDisposition::BufferTooSmall);
        assert_eq!(q.wait_disposition(32), WaitDisposition::Pend);
        q.push(n(1)).unwrap();
        assert_eq!(q.wait_disposition(0), WaitDisposition::BufferTooSmall);
        assert_eq!(q.wait_disposition(1), WaitDisposition::Deliver);
        let dropped = q.close();
        assert_eq!(dropped.len(), 1);
        assert_eq!(q.wait_disposition(1), WaitDisposition::Cancelled);
        assert_eq!(q.wait_disposition(0), WaitDisposition::BufferTooSmall);
    }

    #[test]
    fn closed_queue_refuses_and_returns_the_event() {
        let mut q = NotificationQueue::new();
        assert!(!q.is_closed());
        q.close();
        assert!(q.is_closed());
        let (status, back) = q.push(n(9)).unwrap_err();
        assert_eq!(status, NtStatus::CANCELLED);
        assert_eq!(back, n(9));
        assert!(q.is_empty());
    }

    #[test]
    fn bounded() {
        let mut q = NotificationQueue::new();
        for i in 0..MAX_QUEUED_EVENTS as u32 {
            q.push(n(i)).unwrap();
        }
        let (status, back) = q.push(n(7)).unwrap_err();
        assert_eq!(status, NtStatus::INSUFFICIENT_RESOURCES);
        assert_eq!(back.event, 7);
        q.pop();
        assert!(q.push(n(8)).is_ok());
    }
}
