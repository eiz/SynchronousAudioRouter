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

// The interface between SAR's user-mode software and its kernel driver: the
// control requests and their structures, and the layout of the register
// table. The driver has its own definitions of the same things, in
// driver/crates/sar-core (see driver/docs/behavior.md, appendix A); the two
// have to agree.
#include <windows.h>

#pragma warning(push)
#pragma warning(disable:4200)

// {C16E8D6C-C4CC-4C76-B11C-79B8414EA968}
DEFINE_GUID(GUID_DEVINTERFACE_SYNCHRONOUSAUDIOROUTER,
    0xc16e8d6c, 0xc4cc, 0x4c76, 0xb1, 0x1c, 0x79, 0xb8, 0x41, 0x4e, 0xa9, 0x68);

#define MAX_ENDPOINT_NAME_LENGTH 63

#define SAR_ENDPOINT_TYPE_RECORDING 1
#define SAR_ENDPOINT_TYPE_PLAYBACK 2

// SAR control interface ioctls
#define SAR_SET_BUFFER_LAYOUT CTL_CODE( \
    FILE_DEVICE_UNKNOWN, 1, METHOD_NEITHER, FILE_READ_DATA | FILE_WRITE_DATA)
#define SAR_CREATE_ENDPOINT CTL_CODE( \
    FILE_DEVICE_UNKNOWN, 2, METHOD_NEITHER, FILE_READ_DATA | FILE_WRITE_DATA)
#define SAR_WAIT_HANDLE_QUEUE CTL_CODE( \
    FILE_DEVICE_UNKNOWN, 3, METHOD_BUFFERED, FILE_READ_DATA | FILE_WRITE_DATA)
#define SAR_START_REGISTRY_FILTER CTL_CODE( \
    FILE_DEVICE_UNKNOWN, 4, METHOD_NEITHER, FILE_READ_DATA | FILE_WRITE_DATA)
// 5 was SAR_SEND_FORMAT_CHANGE_EVENT, removed with pre-Windows 10 support.

// SarNdis ioctls
#define SARNDIS_IOCTL_CODE(i) CTL_CODE( \
    FILE_DEVICE_PHYSICAL_NETCARD, i, METHOD_NEITHER, \
    FILE_READ_DATA | FILE_WRITE_DATA)
#define SARNDIS_ENUMERATE SARNDIS_IOCTL_CODE(1)
#define SARNDIS_ENABLE SARNDIS_IOCTL_CODE(2)
#define SARNDIS_SYNC SARNDIS_IOCTL_CODE(3)

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
    DWORD periodSizeBytes;
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
    DWORD reserved; //clockRegister;
    DWORD bufferOffset;
    DWORD bufferSize;
    DWORD notificationCount;
    DWORD activeChannelCount;
} SarEndpointRegisters;

typedef struct SarNdisEnumerateResponseItem
{
    ULONG32 nameOffset;
    ULONG32 nameLength;
} SarNdisEnumerateResponseItem;

typedef struct SarNdisEnumerateResponse
{
    ULONG itemCount;
    SarNdisEnumerateResponseItem items[0];
} SarNdisEnumerateResponse;

#pragma warning(pop)

#ifdef __cplusplus
} // extern "C"
#endif

#endif // _SAR_H