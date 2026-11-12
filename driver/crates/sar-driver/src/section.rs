// Copyright (c) 2026 Itty Bitty Binaries, Inc.
// SPDX-License-Identifier: MIT

//! The shared section of one control handle (section 6).
//!
//! The whole section is locked in physical memory for as long as this value
//! exists. The driver has the register table mapped, through a mapping of
//! those locked pages that cannot fault. The audio area is mapped into the
//! host and into clients, never into the kernel: the driver has no way to
//! look at a sample.
#![forbid(unsafe_code)]

use ntk::{
    section::{LockedPages, LockedView, Section, UserView},
    sync::Mutex,
};
use ntstatus::NtStatus;
use sar_core::{
    cells::{CellAllocator, CellRun},
    layout::{Layout, CELL_SIZE},
    registers::{self, Event, Record, RegisterTable},
};

/// The section of one control handle. Shared by the control handle and by
/// every endpoint created on it: it is released when the handle has closed
/// and the last of those endpoints has been released (section 11).
///
/// The lock on the memory belongs to this value and to nothing else. It is
/// taken when the buffer layout is set and given up when the value is
/// dropped; no process's coming or going, the host's included, changes it.
/// So a client that is still streaming after its host has died keeps
/// working on memory that cannot be paged out.
///
/// Fields are dropped in the order they are declared: the driver's mapping
/// and the locks go before the section itself.
pub struct SharedSection {
    layout: Layout,
    /// The register table: its pages locked, and mapped into system address
    /// space. Offset 0 of the mapping is the first register record.
    registers: LockedView,
    /// The audio area's pages, locked. There is no mapping of them in the
    /// kernel.
    _audio: LockedPages,
    /// The cell allocator. Its lock also serialises register updates, so
    /// that two endpoints given the same index cannot interleave the
    /// read-modify-write of one record. Everything done under it is a
    /// memory access that cannot fault (section 14).
    cells: Mutex<CellAllocator>,
    section: Section,
}

/// The register table as the driver sees it.
struct Table<'a> {
    view: &'a LockedView,
}

impl RegisterTable for Table<'_> {
    fn read(&self, offset: u32) -> u32 {
        self.view.read_u32(offset as usize)
    }

    fn write(&self, offset: u32, value: u32) {
        self.view.write_u32(offset as usize, value);
    }

    fn publish(&self, offset: u32, value: u32) {
        self.view.publish_u32(offset as usize, value);
    }
}

impl SharedSection {
    /// Creates the section for `layout`: zeroed, pagefile-backed, unnamed,
    /// and all of it locked in physical memory.
    ///
    /// Fails with `STATUS_INSUFFICIENT_RESOURCES` if the memory cannot be
    /// locked; nothing is left behind then.
    pub fn create(layout: Layout) -> Result<Self, NtStatus> {
        let section = Section::create(u64::from(layout.section_size()))?;
        let cells = Mutex::try_new(CellAllocator::new(layout.usable_cells()))?;

        // Pages are locked through a view, and the only view the driver can
        // lock through is one in system address space. It is needed for no
        // longer than that: the locks are on the pages, and the register
        // table is then mapped a second time, from the locked pages. The
        // temporary view is the one place where the audio area appears in
        // kernel address space, and nothing is read or written through it.
        let unlockable = |_| NtStatus::INSUFFICIENT_RESOURCES;
        let whole = section
            .map_into_system_space(0, layout.section_size() as usize)
            .map_err(unlockable)?;
        let audio_size = layout.register_base() as usize;
        let table_size = (layout.section_size() - layout.register_base()) as usize;
        let audio = whole.lock_pages(0, audio_size).map_err(unlockable)?;
        let registers = whole
            .lock_pages(audio_size, table_size)
            .and_then(LockedPages::map)
            .map_err(unlockable)?;
        drop(whole);

        Ok(Self {
            layout,
            registers,
            _audio: audio,
            cells,
            section,
        })
    }

    pub fn layout(&self) -> &Layout {
        &self.layout
    }

    fn table(&self) -> Table<'_> {
        Table { view: &self.registers }
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
    /// returned. The allocator's lock is not held while the view is mapped.
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
