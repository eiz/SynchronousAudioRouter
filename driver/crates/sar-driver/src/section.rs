// Copyright (c) 2026 Itty Bitty Binaries, Inc.
// SPDX-License-Identifier: MIT

//! The shared section of one control handle (section 6).
//!
//! The driver's own view of the section covers the register table only.
//! The audio area is mapped into the host and into clients, never into the
//! kernel: the driver has no way to look at a sample.
#![forbid(unsafe_code)]

use ntk::{
    section::{Section, SystemView, UserView},
    sync::Mutex,
};
use ntstatus::NtStatus;
use sar_core::{
    cells::{CellAllocator, CellRun},
    layout::{Layout, CELL_SIZE},
    registers::{self, Event, Record, RegisterTable},
};

pub struct SharedSection {
    layout: Layout,
    section: Section,
    /// System-space view that starts at the 64 KiB boundary at or below the
    /// register table and runs to the end of the section.
    registers: SystemView,
    /// The cell allocator. Its lock also serialises register updates, so
    /// that two endpoints given the same index cannot interleave the
    /// read-modify-write of one record.
    cells: Mutex<CellAllocator>,
}

/// The register table inside the system view.
struct Table<'a> {
    view: &'a SystemView,
    base: usize,
}

impl RegisterTable for Table<'_> {
    fn read(&self, offset: u32) -> u32 {
        self.view.read_u32(self.base + offset as usize)
    }

    fn write(&self, offset: u32, value: u32) {
        self.view.write_u32(self.base + offset as usize, value);
    }

    fn publish(&self, offset: u32, value: u32) {
        self.view.publish_u32(self.base + offset as usize, value);
    }
}

impl SharedSection {
    /// Creates the section for `layout`: zeroed, pagefile-backed, unnamed.
    pub fn create(layout: Layout) -> Result<Self, NtStatus> {
        let section = Section::create(u64::from(layout.section_size()))?;
        let registers = section.map_into_system_space(
            u64::from(layout.register_view_offset()),
            layout.register_view_size() as usize,
        )?;
        Ok(Self {
            layout,
            section,
            registers,
            cells: Mutex::try_new(CellAllocator::new(layout.usable_cells()))?,
        })
    }

    pub fn layout(&self) -> &Layout {
        &self.layout
    }

    fn table(&self) -> Table<'_> {
        Table {
            view: &self.registers,
            base: self.layout.register_view_delta() as usize,
        }
    }

    /// Maps the whole section into the calling process (the host).
    pub fn map_whole_into_current_process(&self) -> Result<UserView, NtStatus> {
        self.section
            .map_into_current_process(0, self.layout.section_size() as usize)
    }

    /// Maps the register table into the calling process (a client). Views
    /// start on 64 KiB boundaries, so when the table does not, the view
    /// begins a little before it; see [`position_register_address`](Self::position_register_address).
    pub fn map_registers_into_current_process(&self) -> Result<UserView, NtStatus> {
        self.section.map_into_current_process(
            u64::from(self.layout.register_view_offset()),
            self.layout.register_view_size() as usize,
        )
    }

    /// Address of record `index`'s position register in a client's register
    /// view.
    pub fn position_register_address(&self, view: &UserView, index: u32) -> u64 {
        view.address()
            + u64::from(self.layout.register_view_delta())
            + u64::from(registers::record_offset(index))
            + u64::from(registers::offset::POSITION)
    }

    /// Applies `event` to record `index` (A.7).
    pub fn update_record(&self, index: u32, event: Event) -> Record {
        let _serialise = self.cells.lock();
        registers::update_record(&self.table(), index, event)
    }

    /// The `generation` word of record `index` as it is right now.
    pub fn generation(&self, index: u32) -> u32 {
        self.table()
            .read(registers::record_offset(index) + registers::offset::GENERATION)
    }

    /// Takes the first run of `cells` free cells and maps exactly those
    /// cells into the calling process. If mapping fails the cells are
    /// returned.
    pub fn allocate_buffer(&self, cells: u32) -> Result<(CellRun, UserView), NtStatus> {
        let run = self
            .cells
            .lock()
            .allocate(cells)
            .ok_or(NtStatus::INSUFFICIENT_RESOURCES)?;
        let mapped = self.section.map_into_current_process(
            u64::from(self.layout.cell_offset(run.first)),
            run.count as usize * CELL_SIZE as usize,
        );
        match mapped {
            Ok(view) => Ok((run, view)),
            Err(status) => {
                self.cells.lock().free(run);
                Err(status)
            }
        }
    }

    /// Returns cells handed out by [`allocate_buffer`](Self::allocate_buffer).
    pub fn free_cells(&self, run: CellRun) {
        self.cells.lock().free(run);
    }
}
