// SynchronousAudioRouter
// Copyright (C) 2015 Mackenzie Straight
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with SynchronousAudioRouter.  If not, see <http://www.gnu.org/licenses/>.

#ifndef _SAR_H
#define _SAR_H

#ifdef __cplusplus
extern "C" {
#endif

#if defined(KERNEL)
#include <ntifs.h>
#include <ntddk.h>
#include <ntstrsafe.h>
#include <windef.h>
#include <ks.h>
#define NOBITMAP
#include <mmreg.h>
#include <ksmedia.h>
#include <devpkey.h>
#include <ksproxy.h>
#else
#include <windows.h>
#endif

#ifdef SAR_INIT_GUID
//#include <initguid.h>
#endif

#pragma warning(push)
#pragma warning(disable:4200)

// {C16E8D6C-C4CC-4C76-B11C-79B8414EA968}
DEFINE_GUID(GUID_DEVINTERFACE_SYNCHRONOUSAUDIOROUTER,
    0xc16e8d6c, 0xc4cc, 0x4c76, 0xb1, 0x1c, 0x79, 0xb8, 0x41, 0x4e, 0xa9, 0x68);

#define MAX_ENDPOINT_NAME_LENGTH 63

#define SAR_ENDPOINT_TYPE_RECORDING 1
#define SAR_ENDPOINT_TYPE_PLAYBACK 2

#define SAR_SET_BUFFER_LAYOUT CTL_CODE( \
    FILE_DEVICE_UNKNOWN, 1, METHOD_NEITHER, FILE_READ_DATA | FILE_WRITE_DATA)
#define SAR_CREATE_ENDPOINT CTL_CODE( \
    FILE_DEVICE_UNKNOWN, 2, METHOD_NEITHER, FILE_READ_DATA | FILE_WRITE_DATA)
#define SAR_WAIT_HANDLE_QUEUE CTL_CODE( \
    FILE_DEVICE_UNKNOWN, 3, METHOD_BUFFERED, FILE_READ_DATA | FILE_WRITE_DATA)
#define SAR_START_REGISTRY_FILTER CTL_CODE( \
    FILE_DEVICE_UNKNOWN, 4, METHOD_NEITHER, FILE_READ_DATA | FILE_WRITE_DATA)

#define SAR_MAX_BUFFER_SIZE 1024 * 1024 * 128
#define SAR_MIN_SAMPLE_SIZE 1
#define SAR_MAX_SAMPLE_SIZE 4
#define SAR_MIN_SAMPLE_RATE 8000
#define SAR_MAX_SAMPLE_RATE 192000
#define SAR_MAX_CHANNEL_COUNT 32
#define SAR_BUFFER_CELL_SIZE 65536
#define SAR_MAX_ENDPOINT_COUNT \
    (SAR_BUFFER_CELL_SIZE / sizeof(SarEndpointRegisters))

typedef struct SarCreateEndpointRequest
{
    DWORD type;
    DWORD index;
    DWORD channelCount;
    WCHAR id[MAX_ENDPOINT_NAME_LENGTH+1];
    WCHAR name[MAX_ENDPOINT_NAME_LENGTH+1];
} SarCreateEndpointRequest;

typedef struct SarSetBufferLayoutRequest
{
    DWORD bufferSize;
    DWORD frameSize;
    DWORD sampleRate;
    DWORD sampleSize;
    DWORD minimumFrameCount;
} SarSetBufferLayoutRequest;

typedef struct SarSetBufferLayoutResponse
{
    PVOID64 virtualAddress;
    DWORD actualSize;
    DWORD registerBase;
} SarSetBufferLayoutResponse;

typedef struct SarHandleQueueResponse
{
    PVOID64 handle;
    ULONG64 associatedData;
} SarHandleQueueResponse;

#define GENERATION_IS_ACTIVE(gen) ((gen) & 1)
#define MAKE_GENERATION(gen, active) (((gen) << 1) | (active))
#define GENERATION_NUMBER(gen) ((gen) >> 1)

typedef struct SarEndpointRegisters
{
    ULONG generation;
    DWORD positionRegister;
    DWORD clockRegister;
    DWORD bufferOffset;
    DWORD bufferSize;
    DWORD notificationCount;
} SarEndpointRegisters;

#if defined(KERNEL)

#define SAR_CONTROL_REFERENCE_STRING L"\\{0EB287D4-6C04-4926-AE19-3C066A4C3F3A}"
#define SAR_TAG '1RAS'

#define NO_LOGGING

#ifdef NO_LOGGING
#define SAR_LOG(...)
#else
#define SAR_LOG(fmt, ...) \
    DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_WARNING_LEVEL, fmt "\n", __VA_ARGS__)
#endif

typedef struct SarHandleQueue
{
    KSPIN_LOCK lock;
    LIST_ENTRY pendingItems;
    LIST_ENTRY pendingIrps;
} SarHandleQueue;

typedef struct SarHandleQueueItem
{
    LIST_ENTRY listEntry;
    HANDLE kernelProcessHandle;
    HANDLE userHandle;
    ULONG64 associatedData;
} SarHandleQueueItem;

typedef struct SarHandleQueueIrp
{
    LIST_ENTRY listEntry;
    HANDLE kernelProcessHandle;
    PIRP irp;
} SarHandleQueueIrp;

typedef struct SarTableEntry
{
    PVOID key;
    PVOID value;
} SarTableEntry;

typedef struct SarDriverExtension
{
    PDRIVER_DISPATCH ksDispatchCreate;
    PDRIVER_DISPATCH ksDispatchClose;
    PDRIVER_DISPATCH ksDispatchCleanup;
    PDRIVER_DISPATCH ksDispatchDeviceControl;
    UNICODE_STRING sarInterfaceName;
    FAST_MUTEX mutex;
    RTL_GENERIC_TABLE controlContextTable;
    LARGE_INTEGER filterCookie;
} SarDriverExtension;

typedef struct SarControlContext
{
    LONG refs;

    FAST_MUTEX mutex;
    BOOLEAN orphan;
    PFILE_OBJECT fileObject;
    PIO_WORKITEM workItem;
    LIST_ENTRY endpointList;
    LIST_ENTRY pendingEndpointList;
    HANDLE bufferSection;
    SarHandleQueue handleQueue;
    RTL_BITMAP bufferMap;
    DWORD bufferSize;
    DWORD frameSize;
    DWORD sampleRate;
    DWORD sampleSize;
    DWORD minimumFrameCount;
} SarControlContext;

#define SarBufferMapEntryCount(controlContext) \
    ((controlContext)->bufferSize / SAR_BUFFER_CELL_SIZE)
#define SarBufferMapSize(controlContext) (sizeof(DWORD) * ( \
    SarBufferMapEntryCount(controlContext) / sizeof(DWORD) + \
    (((SarBufferMapEntryCount(controlContext) % sizeof(DWORD)) != 0) ? 1 : 0)))

typedef struct SarEndpointProcessContext
{
    LIST_ENTRY listEntry;
    PEPROCESS process;
    HANDLE processHandle;
    SarEndpointRegisters *registerFileUVA;
    PVOID bufferUVA;
} SarEndpointProcessContext;

typedef struct SarEndpoint
{
    LONG refs;
    LIST_ENTRY listEntry;
    PIRP pendingIrp;
    UNICODE_STRING deviceName;
    UNICODE_STRING deviceId;
    SarControlContext *owner;
    PKSFILTERFACTORY filterFactory;
    PKSFILTER_DESCRIPTOR filterDesc;
    PKSPIN_DESCRIPTOR_EX pinDesc;
    PKSNODE_DESCRIPTOR nodeDesc;
    PKSDATARANGE_AUDIO dataRange;
    PKSDATARANGE_AUDIO analogDataRange;
    DWORD type;
    DWORD index;
    DWORD channelCount;

    FAST_MUTEX mutex;
    BOOLEAN orphan;
    PKSPIN activePin;
    DWORD activeCellIndex;
    SIZE_T activeViewSize;
    ULONG activeBufferSize;
    LIST_ENTRY activeProcessList;
} SarEndpoint;

// Control
NTSTATUS SarReadUserBuffer(PVOID src, PIRP irp, ULONG size);
NTSTATUS SarWriteUserBuffer(PVOID src, PIRP irp, ULONG size);
VOID SarDumpKsIoctl(PIO_STACK_LOCATION irpStack);
NTSTATUS SarSetBufferLayout(
    SarControlContext *controlContext,
    SarSetBufferLayoutRequest *request,
    SarSetBufferLayoutResponse *response);
NTSTATUS SarCreateEndpoint(
    PDEVICE_OBJECT device,
    PIRP irp,
    SarControlContext *controlContext,
    SarCreateEndpointRequest *request);
VOID SarOrphanEndpoint(SarEndpoint *endpoint);
VOID SarDeleteEndpoint(SarEndpoint *endpoint);

FORCEINLINE VOID SarRetainEndpoint(SarEndpoint *endpoint)
{
    InterlockedIncrement(&endpoint->refs);
}

FORCEINLINE BOOLEAN SarReleaseEndpoint(SarEndpoint *endpoint)
{
    if (InterlockedDecrement(&endpoint->refs) == 0) {
        SarDeleteEndpoint(endpoint);
        return TRUE;
    }

    return FALSE;
}

// Device
NTSTATUS SarKsDeviceAdd(IN PKSDEVICE device);
NTSTATUS SarKsDevicePostStart(IN PKSDEVICE device);

// Pin/Filter
NTSTATUS SarKsFilterCreate(PKSFILTER filter, PIRP irp);
NTSTATUS SarKsFilterClose(PKSFILTER filter, PIRP irp);
NTSTATUS SarKsPinCreate(PKSPIN pin, PIRP irp);
NTSTATUS SarKsPinClose(PKSPIN pin, PIRP irp);
VOID SarKsPinReset(PKSPIN pin);
NTSTATUS SarKsPinProcess(PKSPIN pin);
NTSTATUS SarKsPinConnect(PKSPIN pin);
VOID SarKsPinDisconnect(PKSPIN pin);
NTSTATUS SarKsPinSetDataFormat(
    PKSPIN pin,
    PKSDATAFORMAT oldFormat,
    PKSMULTIPLE_ITEM oldAttributeList,
    const KSDATARANGE *dataRange,
    const KSATTRIBUTE_LIST *attributeRange);
NTSTATUS SarKsPinSetDeviceState(PKSPIN pin, KSSTATE toState, KSSTATE fromState);
NTSTATUS SarKsPinIntersectHandler(
    PVOID context, PIRP irp, PKSP_PIN pin,
    PKSDATARANGE dataRange, PKSDATARANGE matchingDataRange, ULONG dataBufferSize,
    PVOID data, PULONG dataSize);
NTSTATUS SarKsPinGetName(
    PIRP irp, PKSIDENTIFIER request, PVOID data);
NTSTATUS SarKsPinGetGlobalInstancesCount(
    PIRP irp, PKSIDENTIFIER request, PVOID data);
NTSTATUS SarKsPinGetDefaultDataFormat(
    PIRP irp, PKSIDENTIFIER request, PVOID data);
NTSTATUS SarKsPinProposeDataFormat(
    PIRP irp, PKSIDENTIFIER request, PVOID data);
NTSTATUS SarKsPinRtGetBuffer(
    PIRP irp, PKSIDENTIFIER request, PVOID data);
NTSTATUS SarKsPinRtGetBufferWithNotification(
    PIRP irp, PKSIDENTIFIER request, PVOID data);
NTSTATUS SarKsPinRtGetClockRegister(
    PIRP irp, PKSIDENTIFIER request, PVOID data);
NTSTATUS SarKsPinRtGetHwLatency(
    PIRP irp, PKSIDENTIFIER request, PVOID data);
NTSTATUS SarKsPinRtGetPacketCount(
    PIRP irp, PKSIDENTIFIER request, PVOID data);
NTSTATUS SarKsPinRtGetPositionRegister(
    PIRP irp, PKSIDENTIFIER request, PVOID data);
NTSTATUS SarKsPinRtGetPresentationPosition(
    PIRP irp, PKSIDENTIFIER request, PVOID data);
NTSTATUS SarKsPinRtQueryNotificationSupport(
    PIRP irp, PKSIDENTIFIER request, PVOID data);
NTSTATUS SarKsPinRtRegisterNotificationEvent(
    PIRP irp, PKSIDENTIFIER request, PVOID data);
NTSTATUS SarKsPinRtUnregisterNotificationEvent(
    PIRP irp, PKSIDENTIFIER request, PVOID data);
NTSTATUS SarGetOrCreateEndpointProcessContext(
    SarEndpoint *endpoint,
    PEPROCESS process,
    SarEndpointProcessContext **outContext);
NTSTATUS SarDeleteEndpointProcessContext(SarEndpointProcessContext *context);

// Entry
SarControlContext *SarCreateControlContext(PFILE_OBJECT fileObject);
VOID SarDeleteControlContext(SarControlContext *controlContext);
BOOLEAN SarOrphanControlContext(SarDriverExtension *extension, PIRP irp);
NTSTATUS SarRegistryCallback(PVOID context, PVOID argument1, PVOID argument2);

FORCEINLINE VOID SarRetainControlContext(SarControlContext *controlContext)
{
    InterlockedIncrement(&controlContext->refs);
}

FORCEINLINE BOOLEAN SarReleaseControlContext(SarControlContext *controlContext)
{
    if (InterlockedDecrement(&controlContext->refs) == 0) {
        SarDeleteControlContext(controlContext);
        return TRUE;
    }

    return FALSE;
}

FORCEINLINE VOID SarReleaseEndpointAndContext(SarEndpoint *endpoint)
{
    SarControlContext *controlContext = endpoint->owner;

    SarReleaseEndpoint(endpoint);
    SarReleaseControlContext(controlContext);
}

NTSTATUS DriverEntry(
    IN PDRIVER_OBJECT driverObject,
    IN PUNICODE_STRING registryPath);

// Utility
#define FOR_EACH_GENERIC(table, objType, obj, restartKey) \
    for (objType *obj = (objType *)RtlEnumerateGenericTableWithoutSplaying( \
        (table), &(restartKey)); \
        obj; \
        obj = (objType *)RtlEnumerateGenericTableWithoutSplaying( \
        (table), &(restartKey)))
#define GUID_FORMAT "{%08lx-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x}"
#define GUID_VALUES(g) (g).Data1, (g).Data2, (g).Data3, \
    (g).Data4[0], \
    (g).Data4[1], \
    (g).Data4[2], \
    (g).Data4[3], \
    (g).Data4[4], \
    (g).Data4[5], \
    (g).Data4[6], \
    (g).Data4[7]
#define ROUND_UP(N, S) ((((N) + (S) - 1) / (S)) * (S))

SarDriverExtension *SarGetDriverExtension(PDRIVER_OBJECT driverObject);
SarDriverExtension *SarGetDriverExtensionFromIrp(PIRP irp);
SarControlContext *SarGetControlContextFromFileObject(
    SarDriverExtension *extension, PFILE_OBJECT fileObject);

// Gets the endpoint associated with an IRP for a KS pin or filter. If retain
// is TRUE, both the endpoint and its control context are retained and must be
// released using SarReleaseEndpointAndContext.
SarEndpoint *SarGetEndpointFromIrp(PIRP irp, BOOLEAN retain);
NTSTATUS SarReadEndpointRegisters(
    SarEndpointRegisters *regs, SarEndpoint *endpoint);
NTSTATUS SarWriteEndpointRegisters(
    SarEndpointRegisters *regs, SarEndpoint *endpoint);

NTSTATUS SarStringDuplicate(PUNICODE_STRING str, PUNICODE_STRING src);

void SarInitializeHandleQueue(SarHandleQueue *queue);
NTSTATUS SarTransferQueuedHandle(
    PIRP irp, HANDLE kernelTargetProcessHandle, ULONG responseIndex,
    HANDLE kernelProcessHandle, HANDLE userHandle, ULONG64 associatedData);
void SarCancelHandleQueueIrp(PDEVICE_OBJECT deviceObject, PIRP irp);
NTSTATUS SarPostHandleQueue(
    SarHandleQueue *queue, HANDLE userHandle, ULONG64 associatedData);
NTSTATUS SarWaitHandleQueue(SarHandleQueue *queue, PIRP irp);
VOID SarStringFree(PUNICODE_STRING str);

RTL_GENERIC_COMPARE_RESULTS NTAPI SarCompareTableEntry(
    PRTL_GENERIC_TABLE table, PVOID lhs, PVOID rhs);
PVOID NTAPI SarAllocateTableEntry(PRTL_GENERIC_TABLE table, CLONG byteSize);
VOID NTAPI SarFreeTableEntry(PRTL_GENERIC_TABLE table, PVOID buffer);
NTSTATUS SarInsertTableEntry(PRTL_GENERIC_TABLE table, PVOID key, PVOID value);
BOOLEAN SarRemoveTableEntry(PRTL_GENERIC_TABLE table, PVOID key);
PVOID SarGetTableEntry(PRTL_GENERIC_TABLE table, PVOID key);
VOID SarInitializeTable(PRTL_GENERIC_TABLE table);

#endif // KERNEL

#pragma warning(pop)

#ifdef __cplusplus
} // extern "C"
#endif

#endif // _SAR_H