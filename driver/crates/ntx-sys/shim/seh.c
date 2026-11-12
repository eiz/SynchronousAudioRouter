// Copyright (c) 2026 Itty Bitty Binaries, Inc.
// SPDX-License-Identifier: MIT

/*
 * Structured exception handling shim.
 *
 * Why this is C: touching a user-mode address from kernel mode can fault at
 * any time (the page can be unmapped or made read-only by another thread of
 * the process), and ProbeForRead/ProbeForWrite report a bad address by
 * raising an exception. So does MmProbeAndLockPages when it cannot lock the
 * pages it is given. The only way to survive any of these is a structured
 * exception handler (__try/__except) around the call. Rust has no SEH
 * construct, so each function below is the smallest possible __try block and
 * does nothing else; all decisions are made by the Rust callers.
 *
 * Every function returns STATUS_SUCCESS, or the exception code if the access
 * raised (STATUS_ACCESS_VIOLATION, STATUS_DATATYPE_MISALIGNMENT, ...).
 */
#include <ntddk.h>

/*
 * Copies Length bytes from a user-mode address into kernel memory after
 * checking that the range lies in user space and is aligned to Alignment.
 */
NTSTATUS
SarShimProbeAndRead(
    _Out_writes_bytes_(Length) VOID *Destination,
    _In_reads_bytes_(Length) const VOID *UserSource,
    _In_ SIZE_T Length,
    _In_ ULONG Alignment
    )
{
    __try {
        ProbeForRead((PVOID)UserSource, Length, Alignment);
        RtlCopyMemory(Destination, UserSource, Length);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return GetExceptionCode();
    }
    return STATUS_SUCCESS;
}

/*
 * Checks that a user-mode range is in user space, aligned and writable.
 * Writes nothing the caller can observe (ProbeForWrite rewrites one byte per
 * page with its own value).
 */
NTSTATUS
SarShimProbeForWrite(
    _In_ VOID *UserDestination,
    _In_ SIZE_T Length,
    _In_ ULONG Alignment
    )
{
    __try {
        ProbeForWrite(UserDestination, Length, Alignment);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return GetExceptionCode();
    }
    return STATUS_SUCCESS;
}

/*
 * Copies Length bytes from kernel memory to a user-mode address after
 * checking that the range lies in user space, is aligned and is writable.
 */
NTSTATUS
SarShimProbeAndWrite(
    _Out_writes_bytes_(Length) VOID *UserDestination,
    _In_reads_bytes_(Length) const VOID *Source,
    _In_ SIZE_T Length,
    _In_ ULONG Alignment
    )
{
    __try {
        ProbeForWrite(UserDestination, Length, Alignment);
        RtlCopyMemory(UserDestination, Source, Length);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return GetExceptionCode();
    }
    return STATUS_SUCCESS;
}

/*
 * Copies Length bytes to a buffer that the kernel has already probed but not
 * captured (the output buffers handed to a registry callback). No probe is
 * made here; only a fault during the copy is caught.
 */
NTSTATUS
SarShimGuardedWrite(
    _Out_writes_bytes_(Length) VOID *Destination,
    _In_reads_bytes_(Length) const VOID *Source,
    _In_ SIZE_T Length
    )
{
    __try {
        RtlCopyMemory(Destination, Source, Length);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return GetExceptionCode();
    }
    return STATUS_SUCCESS;
}

/*
 * Makes the pages an MDL describes resident and locks them in physical
 * memory. MmProbeAndLockPages reports failure by raising an exception, which
 * is the only reason this is here.
 */
NTSTATUS
SarShimProbeAndLockPages(
    _Inout_ MDL *MemoryDescriptorList,
    _In_ KPROCESSOR_MODE AccessMode,
    _In_ LOCK_OPERATION Operation
    )
{
    __try {
        MmProbeAndLockPages(MemoryDescriptorList, AccessMode, Operation);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return GetExceptionCode();
    }
    return STATUS_SUCCESS;
}
