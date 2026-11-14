#!/usr/bin/env bash
# Copyright (c) 2026 Itty Bitty Binaries, Inc.
# SPDX-License-Identifier: MIT
# Prints the numbers quoted in the final report: unsafe usage per crate and
# lines of Rust and C. Counts hand-written sources only (not bindgen output).
set -uo pipefail
cd "$(dirname "$0")/.."
count() { grep -v '^\s*//' "$@" | grep -c -E "$PATTERN"; true; }
printf '%-12s %9s %9s %10s %12s %12s\n' crate rust_loc 'unsafe{}' 'unsafe fn' 'unsafe impl' 'extern fn/{}'
for crate in ntstatus sar-core ks-sys ntx-sys ntk ks sar-driver; do
    all="$(mktemp)"
    find "crates/$crate" -name '*.rs' -not -path '*/target/*' -print0 | sort -z | xargs -0 cat > "$all"
    loc=$(wc -l < "$all")
    blocks=$(grep -v '^\s*//' "$all" | grep -o 'unsafe {' | wc -l)
    fns=$(grep -v '^\s*//' "$all" | grep -c -E '(^|[^a-z_])unsafe (extern "[A-Za-z]+" )?fn ')
    impls=$(grep -v '^\s*//' "$all" | grep -c -E '^\s*unsafe impl')
    externs=$(grep -v '^\s*//' "$all" | grep -c -E 'extern "(C|system)" (fn|\{)')
    printf '%-12s %9s %9s %10s %12s %12s\n' "$crate" "$loc" "$blocks" "$fns" "$impls" "$externs"
    rm -f "$all"
done
echo
echo "C sources:"
wc -l crates/*/shim/*.c
echo
echo "modules with #![forbid(unsafe_code)]:"
grep -l '^#!\[forbid(unsafe_code)\]' -r crates --include='*.rs' | sort
echo
echo "sar-driver modules without it:"
grep -L '^#!\[forbid(unsafe_code)\]' crates/sar-driver/src/*.rs
