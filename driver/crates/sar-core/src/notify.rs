// Copyright (c) 2026 Itty Bitty Binaries, Inc.
// SPDX-License-Identifier: MIT

//! The notification queue of one control handle (sections 5, 9 and 14,
//! A.5).
//!
//! This is the state the driver keeps under the queue's lock: the events
//! clients registered that the host has not collected yet, and whether the
//! control handle has closed. The pending wait requests themselves are I/O
//! request packets and live in the kernel's cancel-safe queue; the driver
//! consults [`NotificationQueue::wait_disposition`] under the same lock to
//! decide whether a wait may be parked.
//!
//! The lock is a spin lock that the host's real-time thread takes, so
//! nothing here that runs under it allocates or frees memory, and nothing
//! copies more than a fixed, small amount, however many events are waiting
//! (section 14). That shapes the queue:
//!
//! * Events are kept in chunks of [`CHUNK_EVENTS`]. The list of chunks has
//!   room for the fullest queue from the start, so adding a chunk never
//!   allocates. A chunk itself is allocated by whoever needs one, before
//!   taking the lock ([`NotificationQueue::new_chunk`]), and handed in.
//! * Events are taken out at most [`TAKE_LIMIT`] at a time, into a
//!   [`Batch`] on the caller's stack. A caller that wants more takes the
//!   lock again.
//! * A chunk that has been emptied leaves with the batch, and the whole
//!   list leaves with [`NotificationQueue::close`], to be freed by the
//!   caller once the lock is released.
#![forbid(unsafe_code)]

use alloc::collections::VecDeque;

use ntstatus::NtStatus;

/// Most events that may wait for the host at one time. The specification
/// sets no bound; without one a client could grow kernel memory without
/// limit by registering events the host never collects. Eight per possible
/// endpoint is far above anything a working host leaves uncollected.
pub const MAX_QUEUED_EVENTS: usize = 8 * crate::layout::MAX_ENDPOINTS as usize;

/// Events per chunk.
pub const CHUNK_EVENTS: usize = 128;

/// Most events handed out under one hold of the lock.
pub const TAKE_LIMIT: usize = 32;

/// Chunks a full queue can consist of: its events, and one more because the
/// oldest chunk may be partly emptied while the newest is partly filled.
const MAX_CHUNKS: usize = MAX_QUEUED_EVENTS.div_ceil(CHUNK_EVENTS) + 1;

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

/// Room for [`CHUNK_EVENTS`] events, allocated outside the queue's lock.
#[derive(Debug)]
pub struct Chunk<E> {
    events: VecDeque<Notification<E>>,
}

impl<E> Chunk<E> {
    fn is_full(&self) -> bool {
        self.events.len() >= CHUNK_EVENTS
    }
}

/// What became of an event offered to [`NotificationQueue::push`].
#[derive(Debug)]
pub enum Pushed<E> {
    /// It is queued. A chunk that was handed in and not needed comes back,
    /// to be dropped outside the lock.
    Queued { unused: Option<Chunk<E>> },
    /// There is no room in the newest chunk: allocate one
    /// ([`NotificationQueue::new_chunk`]) outside the lock and offer the
    /// event again with it.
    NeedsChunk(Notification<E>),
    /// Refused: `STATUS_CANCELLED` once the control handle has closed,
    /// `STATUS_INSUFFICIENT_RESOURCES` if the queue is full. The event and
    /// any chunk come back so that the caller can release them outside the
    /// lock.
    Refused {
        status: NtStatus,
        notification: Notification<E>,
        unused: Option<Chunk<E>>,
    },
}

/// Events taken from the queue under one hold of the lock, and what has to
/// be freed once the lock is released. Lives on the taker's stack.
#[derive(Debug)]
pub struct Batch<E> {
    events: [Option<Notification<E>>; TAKE_LIMIT],
    len: usize,
    /// A chunk the take emptied.
    retired: Option<Chunk<E>>,
}

impl<E> Batch<E> {
    pub const fn new() -> Self {
        Self {
            events: [const { None }; TAKE_LIMIT],
            len: 0,
            retired: None,
        }
    }

    /// Number of events in the batch.
    pub fn len(&self) -> usize {
        self.len
    }

    pub fn is_empty(&self) -> bool {
        self.len == 0
    }

    /// Takes the events out, oldest first, leaving the batch empty. The
    /// emptied chunk, if any, is dropped here: call this outside the lock.
    pub fn drain(&mut self) -> impl Iterator<Item = Notification<E>> + '_ {
        self.retired = None;
        let len = core::mem::take(&mut self.len);
        self.events[..len].iter_mut().filter_map(Option::take)
    }
}

impl<E> Default for Batch<E> {
    fn default() -> Self {
        Self::new()
    }
}

#[derive(Debug)]
pub struct NotificationQueue<E> {
    /// Oldest chunk first. Only the newest can have room, and only the
    /// oldest can have been partly emptied.
    chunks: VecDeque<Chunk<E>>,
    len: usize,
    closed: bool,
}

impl<E> NotificationQueue<E> {
    /// An empty queue. The list of chunks is given room for the fullest
    /// queue here, so that it never grows under the lock.
    pub fn try_new() -> Result<Self, NtStatus> {
        let mut chunks = VecDeque::new();
        chunks
            .try_reserve_exact(MAX_CHUNKS)
            .map_err(|_| NtStatus::INSUFFICIENT_RESOURCES)?;
        Ok(Self {
            chunks,
            len: 0,
            closed: false,
        })
    }

    /// A chunk to hand to [`push`](Self::push). Allocates: call it outside
    /// the lock.
    pub fn new_chunk() -> Result<Chunk<E>, NtStatus> {
        let mut events = VecDeque::new();
        events
            .try_reserve_exact(CHUNK_EVENTS)
            .map_err(|_| NtStatus::INSUFFICIENT_RESOURCES)?;
        Ok(Chunk { events })
    }

    pub fn is_closed(&self) -> bool {
        self.closed
    }

    pub fn len(&self) -> usize {
        self.len
    }

    pub fn is_empty(&self) -> bool {
        self.len == 0
    }

    /// Queues an event, without allocating. `spare` is a chunk the caller
    /// brought in case the newest one is full.
    pub fn push(&mut self, notification: Notification<E>, spare: Option<Chunk<E>>) -> Pushed<E> {
        if self.closed {
            return Pushed::Refused {
                status: NtStatus::CANCELLED,
                notification,
                unused: spare,
            };
        }
        if self.len >= MAX_QUEUED_EVENTS {
            return Pushed::Refused {
                status: NtStatus::INSUFFICIENT_RESOURCES,
                notification,
                unused: spare,
            };
        }
        let mut unused = spare;
        if self.chunks.back().is_none_or(Chunk::is_full) {
            // The list has room for every chunk a queue within its bound
            // can have (MAX_CHUNKS), so this does not allocate. The check
            // keeps that true whatever else goes wrong.
            match unused.take() {
                Some(chunk) if self.chunks.len() < MAX_CHUNKS => self.chunks.push_back(chunk),
                Some(chunk) => {
                    return Pushed::Refused {
                        status: NtStatus::INSUFFICIENT_RESOURCES,
                        notification,
                        unused: Some(chunk),
                    }
                }
                None => return Pushed::NeedsChunk(notification),
            }
        }
        match self.chunks.back_mut() {
            Some(chunk) => {
                // The chunk was allocated with room for CHUNK_EVENTS and
                // holds fewer, so this does not allocate either.
                chunk.events.push_back(notification);
                self.len += 1;
                Pushed::Queued { unused }
            }
            // Cannot happen: a chunk with room was found or added above.
            None => Pushed::NeedsChunk(notification),
        }
    }

    /// Moves up to `limit` of the oldest events, and never more than
    /// [`TAKE_LIMIT`], into `batch`, which must be empty. Does not allocate
    /// or free: a chunk this empties leaves in the batch.
    pub fn take(&mut self, batch: &mut Batch<E>, limit: usize) {
        let limit = limit.min(TAKE_LIMIT);
        while batch.len < limit {
            let Some(oldest) = self.chunks.front_mut() else {
                break;
            };
            match oldest.events.pop_front() {
                Some(notification) => {
                    batch.events[batch.len] = Some(notification);
                    batch.len += 1;
                    self.len -= 1;
                }
                None => {
                    // An emptied chunk is kept if it is the only one (the
                    // next event goes into it), and otherwise leaves with
                    // the batch, which has room for one.
                    if self.chunks.len() == 1 || batch.retired.is_some() {
                        break;
                    }
                    batch.retired = self.chunks.pop_front();
                }
            }
        }
    }

    /// Decides the fate of a wait request whose buffer holds `capacity`
    /// records, in the order of the checks in A.5.
    pub fn wait_disposition(&self, capacity: usize) -> WaitDisposition {
        if capacity == 0 {
            WaitDisposition::BufferTooSmall
        } else if self.len != 0 {
            WaitDisposition::Deliver
        } else if self.closed {
            WaitDisposition::Cancelled
        } else {
            WaitDisposition::Pend
        }
    }

    /// Marks the queue closed and hands back everything nobody collected,
    /// to be dropped outside the lock. Later pushes fail.
    pub fn close(&mut self) -> Uncollected<E> {
        self.closed = true;
        self.len = 0;
        Uncollected {
            chunks: core::mem::take(&mut self.chunks),
        }
    }
}

/// What was still queued when the queue closed. Dropping it releases the
/// events and the memory.
#[derive(Debug)]
pub struct Uncollected<E> {
    chunks: VecDeque<Chunk<E>>,
}

impl<E> Uncollected<E> {
    /// Number of events nobody collected.
    pub fn len(&self) -> usize {
        self.chunks.iter().map(|chunk| chunk.events.len()).sum()
    }

    pub fn is_empty(&self) -> bool {
        self.len() == 0
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    type Queue = NotificationQueue<u32>;

    fn n(event: u32) -> Notification<u32> {
        Notification { event, generation: event * 2, index: event }
    }

    /// Queues an event the way the driver does: without a chunk first, and
    /// with one if the queue asks.
    fn push(q: &mut Queue, notification: Notification<u32>) -> Result<(), (NtStatus, Notification<u32>)> {
        match q.push(notification, None) {
            Pushed::Queued { unused } => {
                assert!(unused.is_none());
                Ok(())
            }
            Pushed::NeedsChunk(notification) => match q.push(notification, Some(Queue::new_chunk().unwrap())) {
                Pushed::Queued { unused } => {
                    assert!(unused.is_none(), "a chunk that was asked for is used");
                    Ok(())
                }
                Pushed::NeedsChunk(_) => panic!("asked for a chunk twice"),
                Pushed::Refused { status, notification, .. } => Err((status, notification)),
            },
            Pushed::Refused { status, notification, unused } => {
                assert!(unused.is_none());
                Err((status, notification))
            }
        }
    }

    fn take_all(q: &mut Queue, limit: usize) -> Vec<u32> {
        let mut batch = Batch::new();
        q.take(&mut batch, limit);
        batch.drain().map(|n| n.event).collect()
    }

    #[test]
    fn fifo() {
        let mut q = Queue::try_new().unwrap();
        assert!(q.is_empty());
        for i in 0..5 {
            push(&mut q, n(i)).unwrap();
        }
        assert_eq!(q.len(), 5);
        assert_eq!(take_all(&mut q, 3), [0, 1, 2]);
        assert_eq!(take_all(&mut q, 32), [3, 4]);
        assert_eq!(take_all(&mut q, 32), [] as [u32; 0]);
        assert!(q.is_empty());
    }

    #[test]
    fn dispositions_follow_a5_order() {
        let mut q = Queue::try_new().unwrap();
        assert_eq!(q.wait_disposition(0), WaitDisposition::BufferTooSmall);
        assert_eq!(q.wait_disposition(32), WaitDisposition::Pend);
        push(&mut q, n(1)).unwrap();
        assert_eq!(q.wait_disposition(0), WaitDisposition::BufferTooSmall);
        assert_eq!(q.wait_disposition(1), WaitDisposition::Deliver);
        let dropped = q.close();
        assert_eq!(dropped.len(), 1);
        assert_eq!(q.wait_disposition(1), WaitDisposition::Cancelled);
        assert_eq!(q.wait_disposition(0), WaitDisposition::BufferTooSmall);
    }

    #[test]
    fn closed_queue_refuses_and_returns_the_event() {
        let mut q = Queue::try_new().unwrap();
        assert!(!q.is_closed());
        assert!(q.close().is_empty());
        assert!(q.is_closed());
        let (status, back) = push(&mut q, n(9)).unwrap_err();
        assert_eq!(status, NtStatus::CANCELLED);
        assert_eq!(back, n(9));
        assert!(q.is_empty());
        // A chunk brought along comes back too.
        match q.push(n(9), Some(Queue::new_chunk().unwrap())) {
            Pushed::Refused { status, unused, .. } => {
                assert_eq!(status, NtStatus::CANCELLED);
                assert!(unused.is_some());
            }
            other => panic!("{other:?}"),
        }
    }

    #[test]
    fn bounded() {
        let mut q = Queue::try_new().unwrap();
        for i in 0..MAX_QUEUED_EVENTS as u32 {
            push(&mut q, n(i)).unwrap();
        }
        let (status, back) = push(&mut q, n(7)).unwrap_err();
        assert_eq!(status, NtStatus::INSUFFICIENT_RESOURCES);
        assert_eq!(back.event, 7);
        assert_eq!(take_all(&mut q, 1), [0]);
        assert!(push(&mut q, n(8)).is_ok());
    }

    #[test]
    fn a_take_never_exceeds_the_limit() {
        let mut q = Queue::try_new().unwrap();
        for i in 0..1000 {
            push(&mut q, n(i)).unwrap();
        }
        let taken = take_all(&mut q, usize::MAX);
        assert_eq!(taken.len(), TAKE_LIMIT);
        assert_eq!(taken, (0..TAKE_LIMIT as u32).collect::<Vec<_>>());
        assert_eq!(q.len(), 1000 - TAKE_LIMIT);
    }

    #[test]
    fn order_holds_across_chunks_and_the_list_never_grows() {
        let mut q = Queue::try_new().unwrap();
        let room = q.chunks.capacity();
        assert!(room >= MAX_CHUNKS);
        // Fill to the bound with the oldest chunk partly emptied, which is
        // the most chunks the queue can have.
        for i in 0..MAX_QUEUED_EVENTS as u32 {
            push(&mut q, n(i)).unwrap();
        }
        let mut expected = 0u32;
        for event in take_all(&mut q, 5) {
            assert_eq!(event, expected);
            expected += 1;
        }
        let mut next = MAX_QUEUED_EVENTS as u32;
        for _ in 0..5 {
            push(&mut q, n(next)).unwrap();
            next += 1;
        }
        assert!(q.chunks.len() <= MAX_CHUNKS);
        assert_eq!(q.chunks.capacity(), room, "the list of chunks was reallocated");
        // Drain everything, pushing a little now and then.
        while !q.is_empty() {
            for event in take_all(&mut q, 7) {
                assert_eq!(event, expected);
                expected += 1;
            }
            if expected.is_multiple_of(500) {
                push(&mut q, n(next)).unwrap();
                next += 1;
            }
        }
        assert_eq!(expected, next);
        assert_eq!(q.chunks.capacity(), room);
        assert!(q.chunks.len() <= 1, "an empty queue keeps at most one chunk");
    }

    #[test]
    fn a_chunk_holds_its_events_without_growing() {
        let mut q = Queue::try_new().unwrap();
        push(&mut q, n(0)).unwrap();
        let room = q.chunks[0].events.capacity();
        assert!(room >= CHUNK_EVENTS);
        for i in 1..CHUNK_EVENTS as u32 {
            // No chunk is asked for while the first has room.
            assert!(matches!(q.push(n(i), None), Pushed::Queued { unused: None }));
        }
        assert_eq!(q.chunks.len(), 1);
        assert_eq!(q.chunks[0].events.capacity(), room, "a chunk was reallocated");
        // The next one needs a chunk.
        assert!(matches!(q.push(n(999), None), Pushed::NeedsChunk(_)));
    }

    #[test]
    fn an_emptied_chunk_leaves_with_the_batch_unless_it_is_the_last() {
        let mut q = Queue::try_new().unwrap();
        for i in 0..(CHUNK_EVENTS as u32 + 3) {
            push(&mut q, n(i)).unwrap();
        }
        assert_eq!(q.chunks.len(), 2);
        // Empty the first chunk exactly.
        let mut taken = 0;
        while taken < CHUNK_EVENTS {
            let mut batch = Batch::new();
            q.take(&mut batch, CHUNK_EVENTS - taken);
            taken += batch.len();
            assert!(batch.retired.is_none(), "a chunk is retired by the take that finds it empty");
            let _ = batch.drain().count();
        }
        // The next take finds it empty, retires it, and goes on.
        let mut batch = Batch::new();
        q.take(&mut batch, TAKE_LIMIT);
        assert!(batch.retired.is_some());
        assert_eq!(batch.len(), 3);
        assert_eq!(q.chunks.len(), 1);
        let _ = batch.drain().count();
        assert!(batch.retired.is_none());
        // The last chunk stays, empty, for the next event.
        assert!(q.is_empty());
        assert_eq!(q.chunks.len(), 1);
        assert!(matches!(q.push(n(1), None), Pushed::Queued { unused: None }));
    }

    #[test]
    fn a_spare_chunk_that_is_not_needed_comes_back() {
        let mut q = Queue::try_new().unwrap();
        push(&mut q, n(0)).unwrap();
        match q.push(n(1), Some(Queue::new_chunk().unwrap())) {
            Pushed::Queued { unused } => assert!(unused.is_some()),
            other => panic!("{other:?}"),
        }
        assert_eq!(q.len(), 2);
        assert_eq!(q.chunks.len(), 1);
    }
}
