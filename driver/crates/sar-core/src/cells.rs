// Copyright (c) 2026 Itty Bitty Binaries, Inc.
// SPDX-License-Identifier: MIT

//! The cell allocator of a section's audio area (section 6, section 8
//! "Allocating the buffer").
#![forbid(unsafe_code)]

use crate::layout::MAX_CELLS;

const WORDS: usize = (MAX_CELLS as usize) / 64;

/// Tracks which cells of one section are handed out. First fit, as the
/// specification requires: "the first run of free cells that is long enough".
#[derive(Clone, Debug)]
pub struct CellAllocator {
    used: [u64; WORDS],
    count: u32,
}

/// A run of consecutive cells.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct CellRun {
    pub first: u32,
    pub count: u32,
}

impl CellAllocator {
    /// An allocator over `count` cells, all free. `count` is capped at the
    /// largest number of cells a section can have.
    pub fn new(count: u32) -> Self {
        Self {
            used: [0; WORDS],
            count: count.min(MAX_CELLS),
        }
    }

    pub fn cell_count(&self) -> u32 {
        self.count
    }

    fn is_used(&self, cell: u32) -> bool {
        self.used[(cell / 64) as usize] & (1u64 << (cell % 64)) != 0
    }

    fn set(&mut self, cell: u32, used: bool) {
        let word = &mut self.used[(cell / 64) as usize];
        let bit = 1u64 << (cell % 64);
        if used {
            *word |= bit;
        } else {
            *word &= !bit;
        }
    }

    /// Takes the first run of `cells` free cells, or `None` if there is none
    /// (or `cells` is zero).
    pub fn allocate(&mut self, cells: u32) -> Option<CellRun> {
        if cells == 0 || cells > self.count {
            return None;
        }
        let mut run_start = 0u32;
        let mut run_len = 0u32;
        for cell in 0..self.count {
            if self.is_used(cell) {
                run_len = 0;
                run_start = cell + 1;
                continue;
            }
            run_len += 1;
            if run_len == cells {
                for c in run_start..run_start + cells {
                    self.set(c, true);
                }
                return Some(CellRun {
                    first: run_start,
                    count: cells,
                });
            }
        }
        None
    }

    /// Returns a run obtained from [`allocate`](Self::allocate). Cells
    /// outside the allocator, or already free, are ignored.
    pub fn free(&mut self, run: CellRun) {
        let end = run.first.saturating_add(run.count).min(self.count);
        for cell in run.first..end {
            self.set(cell, false);
        }
    }

    /// Number of cells currently handed out.
    pub fn used_cells(&self) -> u32 {
        self.used.iter().map(|w| w.count_ones()).sum()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn first_fit() {
        let mut a = CellAllocator::new(8);
        assert_eq!(a.allocate(2), Some(CellRun { first: 0, count: 2 }));
        assert_eq!(a.allocate(3), Some(CellRun { first: 2, count: 3 }));
        assert_eq!(a.allocate(4), None);
        assert_eq!(a.allocate(3), Some(CellRun { first: 5, count: 3 }));
        assert_eq!(a.allocate(1), None);
        a.free(CellRun { first: 2, count: 3 });
        assert_eq!(a.used_cells(), 5);
        // The hole of three is found before anything later.
        assert_eq!(a.allocate(1), Some(CellRun { first: 2, count: 1 }));
        assert_eq!(a.allocate(3), None);
        assert_eq!(a.allocate(2), Some(CellRun { first: 3, count: 2 }));
    }

    #[test]
    fn zero_and_oversize_requests_fail() {
        let mut a = CellAllocator::new(4);
        assert_eq!(a.allocate(0), None);
        assert_eq!(a.allocate(5), None);
        assert_eq!(a.allocate(u32::MAX), None);
        assert_eq!(a.allocate(4), Some(CellRun { first: 0, count: 4 }));
        assert_eq!(CellAllocator::new(0).allocate(1), None);
    }

    #[test]
    fn full_size_section() {
        let mut a = CellAllocator::new(MAX_CELLS);
        assert_eq!(a.cell_count(), 2048);
        assert_eq!(a.allocate(2048), Some(CellRun { first: 0, count: 2048 }));
        assert_eq!(a.allocate(1), None);
        a.free(CellRun { first: 2047, count: 1 });
        assert_eq!(a.allocate(1), Some(CellRun { first: 2047, count: 1 }));
        // The count is capped; a larger request cannot index out of bounds.
        assert_eq!(CellAllocator::new(u32::MAX).cell_count(), 2048);
    }

    #[test]
    fn free_is_tolerant() {
        let mut a = CellAllocator::new(4);
        a.free(CellRun { first: 2, count: u32::MAX });
        a.free(CellRun { first: 100, count: 1 });
        assert_eq!(a.used_cells(), 0);
        let r = a.allocate(2).unwrap();
        a.free(r);
        a.free(r);
        assert_eq!(a.used_cells(), 0);
    }

    #[test]
    fn runs_never_overlap() {
        // Deterministic pseudo-random allocate/free churn.
        let mut a = CellAllocator::new(64);
        let mut live: Vec<CellRun> = Vec::new();
        let mut seed = 0x1234_5678u32;
        for _ in 0..5000 {
            seed = seed.wrapping_mul(1_664_525).wrapping_add(1_013_904_223);
            if seed & 0x8000 != 0 || live.is_empty() {
                let n = (seed >> 20) % 9 + 1;
                if let Some(run) = a.allocate(n) {
                    for other in &live {
                        let disjoint = run.first + run.count <= other.first
                            || other.first + other.count <= run.first;
                        assert!(disjoint, "{run:?} overlaps {other:?}");
                    }
                    assert!(run.first + run.count <= 64);
                    live.push(run);
                }
            } else {
                let i = (seed as usize >> 8) % live.len();
                a.free(live.swap_remove(i));
            }
            assert_eq!(a.used_cells(), live.iter().map(|r| r.count).sum::<u32>());
        }
    }
}
