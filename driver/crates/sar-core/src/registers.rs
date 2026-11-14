// Copyright (c) 2026 Itty Bitty Binaries, Inc.
// SPDX-License-Identifier: MIT

//! The register record and the driver's update rule (section 6, A.7).
#![forbid(unsafe_code)]

/// Field offsets within a 28-byte register record.
pub mod offset {
    pub const GENERATION: u32 = 0;
    pub const POSITION: u32 = 4;
    // 8: reserved, never written.
    pub const BUFFER_OFFSET: u32 = 12;
    pub const BUFFER_SIZE: u32 = 16;
    pub const NOTIFICATION_COUNT: u32 = 20;
    pub const ACTIVE_CHANNEL_COUNT: u32 = 24;
}

/// Bit 0 of `generation`: the stream is running.
pub const GENERATION_RUNNING: u32 = 1;

/// The stream states of kernel streaming, in their KSSTATE numbering.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum StreamState {
    Stop = 0,
    Acquire = 1,
    Pause = 2,
    Run = 3,
}

impl StreamState {
    pub fn from_ks(value: u32) -> Option<Self> {
        match value {
            0 => Some(Self::Stop),
            1 => Some(Self::Acquire),
            2 => Some(Self::Pause),
            3 => Some(Self::Run),
            _ => None,
        }
    }
}

/// The driver-visible fields of one record (the reserved field is left out
/// because the driver never touches it).
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct Record {
    pub generation: u32,
    pub position: u32,
    pub buffer_offset: u32,
    pub buffer_size: u32,
    pub notification_count: u32,
    pub active_channel_count: u32,
}

/// The moments at which the driver rewrites a record (section 6, "What the
/// driver writes, and when").
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Event {
    StreamOpened { channels: u32 },
    BufferAllocated { offset: u32, size: u32, notification_count: u32 },
    StateChanged { from: StreamState, to: StreamState },
    StreamClosed,
}

impl Record {
    fn bump_counter(generation: u32) -> u32 {
        // generation = counter * 2 + running bit; the counter is 31 bits and
        // wraps. Adding 2 to the whole word and clearing bit 0 does both.
        generation.wrapping_add(2) & !GENERATION_RUNNING
    }

    /// The record after `event`.
    pub fn after(mut self, event: Event) -> Self {
        match event {
            Event::StreamOpened { channels } => {
                self.active_channel_count = channels;
                self.generation = Self::bump_counter(self.generation);
            }
            Event::BufferAllocated { offset, size, notification_count } => {
                self.buffer_offset = offset;
                self.buffer_size = size;
                self.notification_count = notification_count;
            }
            Event::StateChanged { from, to } => {
                if to == StreamState::Run {
                    self.generation |= GENERATION_RUNNING;
                } else if from == StreamState::Run {
                    self.generation &= !GENERATION_RUNNING;
                }
                if (to == StreamState::Stop) != (from == StreamState::Stop) {
                    // Entering or leaving the stopped state.
                    self.position = 0;
                }
            }
            Event::StreamClosed => {
                self.generation = Self::bump_counter(self.generation);
            }
        }
        self
    }
}

/// Access to the 32-bit words of the register table. Implemented over the
/// driver's kernel mapping; a plain array in tests.
pub trait RegisterTable {
    /// Reads the word at byte `offset` of the table.
    fn read(&self, offset: u32) -> u32;
    /// Writes the word at byte `offset` of the table.
    fn write(&self, offset: u32, value: u32);
    /// A full memory barrier followed by one atomic store of the word at
    /// byte `offset`.
    fn publish(&self, offset: u32, value: u32);
}

/// Byte offset of record `index` in the table.
pub fn record_offset(index: u32) -> u32 {
    index * crate::layout::REGISTER_RECORD_SIZE
}

/// Reads record `index`.
pub fn read_record<T: RegisterTable + ?Sized>(table: &T, index: u32) -> Record {
    let base = record_offset(index);
    Record {
        generation: table.read(base + offset::GENERATION),
        position: table.read(base + offset::POSITION),
        buffer_offset: table.read(base + offset::BUFFER_OFFSET),
        buffer_size: table.read(base + offset::BUFFER_SIZE),
        notification_count: table.read(base + offset::NOTIFICATION_COUNT),
        active_channel_count: table.read(base + offset::ACTIVE_CHANNEL_COUNT),
    }
}

/// Applies `event` to record `index` the way A.7 prescribes: read the whole
/// record, write the five other fields, then a barrier and one atomic store
/// of `generation`. Returns the record as written.
///
/// `index` must be below the number of records (2,340); the caller validates
/// it when the endpoint is created.
pub fn update_record<T: RegisterTable + ?Sized>(table: &T, index: u32, event: Event) -> Record {
    let base = record_offset(index);
    let new = read_record(table, index).after(event);
    table.write(base + offset::POSITION, new.position);
    table.write(base + offset::BUFFER_OFFSET, new.buffer_offset);
    table.write(base + offset::BUFFER_SIZE, new.buffer_size);
    table.write(base + offset::NOTIFICATION_COUNT, new.notification_count);
    table.write(base + offset::ACTIVE_CHANNEL_COUNT, new.active_channel_count);
    table.publish(base + offset::GENERATION, new.generation);
    new
}

#[cfg(test)]
mod tests {
    use std::cell::RefCell;

    use super::*;

    #[derive(Default)]
    struct Mock {
        words: RefCell<Vec<u32>>,
        log: RefCell<Vec<(&'static str, u32)>>,
    }

    impl Mock {
        fn new(records: usize) -> Self {
            Self {
                words: RefCell::new(vec![0; records * 7]),
                log: RefCell::new(Vec::new()),
            }
        }
    }

    impl RegisterTable for Mock {
        fn read(&self, offset: u32) -> u32 {
            self.log.borrow_mut().push(("read", offset));
            self.words.borrow()[(offset / 4) as usize]
        }
        fn write(&self, offset: u32, value: u32) {
            self.log.borrow_mut().push(("write", offset));
            self.words.borrow_mut()[(offset / 4) as usize] = value;
        }
        fn publish(&self, offset: u32, value: u32) {
            self.log.borrow_mut().push(("publish", offset));
            self.words.borrow_mut()[(offset / 4) as usize] = value;
        }
    }

    #[test]
    fn lifecycle_matches_section_6() {
        let r = Record::default();
        let r = r.after(Event::StreamOpened { channels: 2 });
        assert_eq!(r.generation, 2);
        assert_eq!(r.active_channel_count, 2);

        let r = r.after(Event::BufferAllocated { offset: 131_072, size: 7680, notification_count: 2 });
        assert_eq!((r.buffer_offset, r.buffer_size, r.notification_count), (131_072, 7680, 2));
        assert_eq!(r.generation, 2);

        let mut r = r;
        r.position = 1234;
        // stop -> acquire: leaving stop resets the position.
        let r = r.after(Event::StateChanged { from: StreamState::Stop, to: StreamState::Acquire });
        assert_eq!(r.position, 0);
        assert_eq!(r.generation, 2);
        let mut r = r.after(Event::StateChanged { from: StreamState::Acquire, to: StreamState::Pause });
        r.position = 96;
        let r = r.after(Event::StateChanged { from: StreamState::Pause, to: StreamState::Run });
        assert_eq!(r.generation, 3);
        assert_eq!(r.position, 96, "pause -> run keeps the position");
        let r = r.after(Event::StateChanged { from: StreamState::Run, to: StreamState::Pause });
        assert_eq!(r.generation, 2);
        assert_eq!(r.position, 96);
        let r = r.after(Event::StateChanged { from: StreamState::Pause, to: StreamState::Acquire });
        let r = r.after(Event::StateChanged { from: StreamState::Acquire, to: StreamState::Stop });
        assert_eq!(r.position, 0, "entering stop resets the position");

        let r = r.after(Event::StreamClosed);
        assert_eq!(r.generation, 4);
        assert_eq!(r.active_channel_count, 2, "closing leaves the other fields");
    }

    #[test]
    fn close_while_running_clears_the_bit_and_bumps_once() {
        let r = Record { generation: 7, ..Record::default() };
        assert_eq!(r.after(Event::StreamClosed).generation, 8);
        assert_eq!(r.after(Event::StreamOpened { channels: 1 }).generation, 8);
    }

    #[test]
    fn counter_wraps() {
        let r = Record { generation: u32::MAX, ..Record::default() };
        assert_eq!(r.after(Event::StreamClosed).generation, 0);
        let r = Record { generation: u32::MAX - 1, ..Record::default() };
        assert_eq!(r.after(Event::StreamClosed).generation, 0);
    }

    #[test]
    fn stop_to_stop_and_run_to_run_are_inert() {
        let r = Record { generation: 4, position: 9, ..Record::default() };
        assert_eq!(r.after(Event::StateChanged { from: StreamState::Stop, to: StreamState::Stop }), r);
        let r = Record { generation: 5, position: 9, ..Record::default() };
        assert_eq!(r.after(Event::StateChanged { from: StreamState::Run, to: StreamState::Run }), r);
    }

    #[test]
    fn update_order_is_read_all_write_five_publish_generation() {
        let table = Mock::new(4);
        // Record 2 starts with a stray reserved word, which must survive.
        table.words.borrow_mut()[2 * 7 + 2] = 0xAAAA_AAAA;
        let written = update_record(&table, 2, Event::StreamOpened { channels: 6 });
        assert_eq!(written.generation, 2);
        let base = 2 * 28;
        let log = table.log.borrow().clone();
        assert_eq!(
            log,
            vec![
                ("read", base),
                ("read", base + 4),
                ("read", base + 12),
                ("read", base + 16),
                ("read", base + 20),
                ("read", base + 24),
                ("write", base + 4),
                ("write", base + 12),
                ("write", base + 16),
                ("write", base + 20),
                ("write", base + 24),
                ("publish", base),
            ]
        );
        let words = table.words.borrow();
        assert_eq!(words[2 * 7], 2);
        assert_eq!(words[2 * 7 + 2], 0xAAAA_AAAA);
        assert_eq!(words[2 * 7 + 6], 6);
        // Other records untouched.
        assert!(words[..14].iter().all(|w| *w == 0));
        assert!(words[21..].iter().all(|w| *w == 0));
    }

    #[test]
    fn last_record_fits_in_the_cell() {
        let last = crate::layout::MAX_ENDPOINTS - 1;
        assert!(record_offset(last) + 28 <= crate::layout::CELL_SIZE);
    }

    #[test]
    fn ks_state_numbers() {
        assert_eq!(StreamState::from_ks(0), Some(StreamState::Stop));
        assert_eq!(StreamState::from_ks(3), Some(StreamState::Run));
        assert_eq!(StreamState::from_ks(4), None);
    }
}
