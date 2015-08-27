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
#include <ksmedia.h>
#include <devpkey.h>
#include <ksproxy.h>
#define NOBITMAP
#include <mmreg.h>
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
    FILE_DEVICE_UNKNOWN, 1, METHOD_IN_DIRECT, FILE_READ_DATA | FILE_WRITE_DATA)
#define SAR_REQUEST_CREATE_ENDPOINT CTL_CODE( \
    FILE_DEVICE_UNKNOWN, 2, METHOD_IN_DIRECT, FILE_READ_DATA | FILE_WRITE_DATA)
#define SAR_REQUEST_MAP_AUDIO_BUFFER CTL_CODE( \
    FILE_DEVICE_UNKNOWN, 3, METHOD_IN_DIRECT, FILE_READ_DATA | FILE_WRITE_DATA)
#define SAR_REQUEST_AUDIO_TICK CTL_CODE( \
    FILE_DEVICE_UNKNOWN, 4, METHOD_IN_DIRECT, FILE_READ_DATA | FILE_WRITE_DATA)

#define SAR_MAX_CHANNEL_COUNT 256
#define SAR_MAX_BUFFER_COUNT 1024
#define SAR_MAX_BUFFER_SIZE 65536
#define SAR_MIN_SAMPLE_DEPTH 1
#define SAR_MAX_SAMPLE_DEPTH 3
#define SAR_MIN_SAMPLE_RATE 44100
#define SAR_MAX_SAMPLE_RATE 192000

#define SAR_INVALID_BUFFER 0xFFFFFFFF

typedef struct SarCreateEndpointRequest
{
    DWORD type;
    DWORD index;
    DWORD channelCount;
    WCHAR name[MAX_ENDPOINT_NAME_LENGTH+1];
} SarCreateEndpointRequest;

typedef struct SarSetBufferLayoutRequest
{
    DWORD bufferCount;
    DWORD bufferSize;
    DWORD sampleRate;
    DWORD sampleDepth;
} SarSetAudioBuffersRequest;

typedef struct SarMapAudioBufferRequest
{
    DWORD type;
    DWORD index;
    DWORD channel;
    DWORD bufferIndex;
} SarMapAudioBuffersRequest;

#if defined(KERNEL)

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

#define SarEndpointSize(channelCount) \
    (sizeof(SarEndpoint) + sizeof(DWORD) * (channelCount))

typedef struct SarEndpoint
{
    LIST_ENTRY listEntry;
    PIRP pendingIrp;
    UNICODE_STRING pendingDeviceName;
    PKSFILTERFACTORY filterFactory;
    PKSFILTER_DESCRIPTOR filterDesc;
    PKSPIN_DESCRIPTOR_EX pinDesc;
    PKSNODE_DESCRIPTOR nodeDesc;
    PKSDATARANGE_AUDIO dataRange;
    PKSDATARANGE_AUDIO analogDataRange;
    PKSALLOCATOR_FRAMING_EX allocatorFraming;
    DWORD type;
    DWORD channelCount;
    DWORD channelMappings[0];
} SarFilterInfo;

typedef struct SarFileContext
{
    PFILE_OBJECT fileObject;
    FAST_MUTEX mutex;
    PIO_WORKITEM workItem;
    LIST_ENTRY endpointList;
    LIST_ENTRY pendingEndpointList;
    DWORD sampleRate;
    DWORD sampleDepth;
    DWORD bufferCount;
    DWORD bufferSize;
} SarFileContext;

NTSTATUS SarCreateEndpoint(
    PDEVICE_OBJECT device,
    PIRP irp,
    SarDriverExtension *extension,
    SarFileContext *fileContext,
    SarCreateEndpointRequest *request);

NTSTATUS SarInitializeFileContext(SarFileContext *fileContext);
BOOLEAN SarDeleteFileContext(SarDriverExtension *extension, PIRP irp);
RTL_GENERIC_COMPARE_RESULTS NTAPI SarCompareFileContext(
    PRTL_GENERIC_TABLE table, PVOID lhs, PVOID rhs);
PVOID NTAPI SarAllocateFileContext(PRTL_GENERIC_TABLE table, CLONG byteSize);
VOID NTAPI SarFreeFileContext(PRTL_GENERIC_TABLE table, PVOID buffer);

#endif // KERNEL

#pragma warning(pop)

#ifdef __cplusplus
} // extern "C"
#endif

#endif // _SAR_H