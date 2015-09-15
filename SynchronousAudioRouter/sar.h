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

// TODO: this information is now obsolete. buffers are direct mapped using wavert
// so there's no audio tick or mapping ioctls.

// things the sar ioctl needs to be able to do:
// - create a ks audio endpoint (capture or playback)
// - delete a ks audio endpoint
// - configure audio buffer count/size
// - set mapping between ks pins(?) and audio buffers
// - perform processing tick

// for the sake of keeping things simple let's define an order of operations:
// - all state is scoped to a handle to the device interface (kind of)
// - SarCreateAudioBuffers should be called before any endpoints are created
//   it must be called exactly once
// - SarCreateEndpoint should be called to create all endpoints
// - SarMapAudioBuffer should be called after the buffers and endpoints are set up
// - SarAudioTick should be called finally
// Any configuration changes require the device to be closed and reopened.
// Closing the SynchronousAudioRouter device automatically destroys all endpoints.
// Endpoint names are global.

#define MAX_ENDPOINT_NAME_LENGTH 63

#define SAR_ENDPOINT_TYPE_RECORDING 1
#define SAR_ENDPOINT_TYPE_PLAYBACK 2

#define SAR_REQUEST_SET_BUFFER_LAYOUT CTL_CODE( \
    FILE_DEVICE_UNKNOWN, 1, METHOD_NEITHER, FILE_READ_DATA | FILE_WRITE_DATA)
#define SAR_REQUEST_CREATE_ENDPOINT CTL_CODE( \
    FILE_DEVICE_UNKNOWN, 2, METHOD_NEITHER, FILE_READ_DATA | FILE_WRITE_DATA)

#define SAR_MAX_BUFFER_SIZE 1024 * 1024 * 128
#define SAR_MIN_SAMPLE_SIZE 1
#define SAR_MAX_SAMPLE_SIZE 3
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
    WCHAR name[MAX_ENDPOINT_NAME_LENGTH+1];
} SarCreateEndpointRequest;

typedef struct SarSetBufferLayoutRequest
{
    DWORD bufferSize;
    DWORD sampleRate;
    DWORD sampleSize;
} SarSetBufferLayoutRequest;

typedef struct SarSetBufferLayoutResponse
{
    PVOID virtualAddress;
    DWORD actualSize;
    DWORD registerBase;
} SarSetBufferLayoutResponse;

typedef struct SarEndpointRegisters
{
    LONG generation;
    BOOL isActive;
    DWORD positionRegister;
    DWORD clockRegister;
    DWORD bufferOffset;
    DWORD bufferSize;
} SarEndpointRegisters;

#if defined(KERNEL)
#define SAR_CONTROL_REFERENCE_STRING L"\\{0EB287D4-6C04-4926-AE19-3C066A4C3F3A}"
#define SAR_TAG '1RAS'

#ifdef NO_LOGGING
#define SAR_LOG(...)
#else
#define SAR_LOG(...) \
    DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_WARNING_LEVEL, __VA_ARGS__)
#endif

typedef struct SarDriverExtension
{
    PDRIVER_DISPATCH ksDispatchCreate;
    PDRIVER_DISPATCH ksDispatchClose;
    PDRIVER_DISPATCH ksDispatchCleanup;
    PDRIVER_DISPATCH ksDispatchDeviceControl;
    UNICODE_STRING sarInterfaceName;
    FAST_MUTEX fileContextLock;
    RTL_GENERIC_TABLE fileContextTable;
    LONG nextFilterId;
} SarDriverExtension;

typedef struct SarFileContext
{
    PFILE_OBJECT fileObject;
    FAST_MUTEX mutex;
    PIO_WORKITEM workItem;
    LIST_ENTRY endpointList;
    LIST_ENTRY pendingEndpointList;
    HANDLE bufferSection;
    RTL_BITMAP bufferMap;
    DWORD bufferSize;
    DWORD sampleRate;
    DWORD sampleSize;
} SarFileContext;

#define SarBufferMapEntryCount(fileContext) \
    ((fileContext)->bufferSize / SAR_BUFFER_CELL_SIZE)
#define SarBufferMapSize(fileContext) ( \
    SarBufferMapEntryCount(fileContext) / sizeof(DWORD) + \
    ((SarBufferMapEntryCount(fileContext) % sizeof(DWORD)) != 0) ? 1 : 0)

typedef struct SarEndpoint
{
    LIST_ENTRY listEntry;
    PIRP pendingIrp;
    UNICODE_STRING deviceName;
    SarFileContext *owner;
    // TODO: lock pin instance data
    PKSPIN activePin;
    PEPROCESS activeProcess;
    SarEndpointRegisters *activeRegisterFileUVA;
    PVOID activeBufferVirtualAddress;
    PKSFILTERFACTORY filterFactory;
    PKSFILTER_DESCRIPTOR filterDesc;
    PKSPIN_DESCRIPTOR_EX pinDesc;
    PKSNODE_DESCRIPTOR nodeDesc;
    PKSDATARANGE_AUDIO dataRange;
    PKSDATARANGE_AUDIO analogDataRange;
    PKSALLOCATOR_FRAMING_EX allocatorFraming;
    DWORD type;
    DWORD index;
    DWORD channelCount;
} SarEndpoint;

typedef struct SarFreeBuffer
{
    SarFreeBuffer *next;
    DWORD size;
} SarFreeBuffer;

// Control
NTSTATUS SarReadUserBuffer(PVOID src, PIRP irp, ULONG size);
NTSTATUS SarWriteUserBuffer(PVOID src, PIRP irp, ULONG size);
VOID SarDumpKsIoctl(PIO_STACK_LOCATION irpStack);
NTSTATUS SarSetBufferLayout(
    SarFileContext *fileContext,
    SarSetBufferLayoutRequest *request,
    SarSetBufferLayoutResponse *response);
NTSTATUS SarCreateEndpoint(
    PDEVICE_OBJECT device,
    PIRP irp,
    SarDriverExtension *extension,
    SarFileContext *fileContext,
    SarCreateEndpointRequest *request);

// Device
NTSTATUS SarKsDeviceAdd(IN PKSDEVICE device);
NTSTATUS SarKsDevicePostStart(IN PKSDEVICE device);

// Pin
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

// Init
NTSTATUS SarInitializeFileContext(SarFileContext *fileContext);
BOOLEAN SarDeleteFileContext(SarDriverExtension *extension, PIRP irp);
RTL_GENERIC_COMPARE_RESULTS NTAPI SarCompareFileContext(
    PRTL_GENERIC_TABLE table, PVOID lhs, PVOID rhs);
PVOID NTAPI SarAllocateFileContext(PRTL_GENERIC_TABLE table, CLONG byteSize);
VOID NTAPI SarFreeFileContext(PRTL_GENERIC_TABLE table, PVOID buffer);
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
SarFileContext *SarGetFileContextFromFileObject(
    SarDriverExtension *extension, PFILE_OBJECT fileObject);
SarEndpoint *SarGetEndpointFromIrp(PIRP irp);
NTSTATUS SarReadEndpointRegisters(
    SarEndpointRegisters *regs, SarEndpoint *endpoint);
NTSTATUS SarWriteEndpointRegisters(
    SarEndpointRegisters *regs, SarEndpoint *endpoint);
NTSTATUS SarIncrementEndpointGeneration(SarEndpoint *endpoint);
NTSTATUS SarStringDuplicate(PUNICODE_STRING str, PUNICODE_STRING src);
VOID SarStringFree(PUNICODE_STRING str);

#endif // KERNEL

#pragma warning(pop)

#ifdef __cplusplus
} // extern "C"
#endif

#endif // _SAR_H