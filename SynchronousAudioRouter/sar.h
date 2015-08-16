#ifndef _SAR_H
#define _SAR_H

#ifdef __cplusplus
extern "C" {
#endif

#if defined(KERNEL)
#include <wdm.h>
#include <windef.h>
#include <ks.h>
#else
#include <windows.h>
#endif

// things the sar ioctl needs to be able to do:
// - create a ks audio endpoint (capture or playback)
// - delete a ks audio endpoint
// - configure audio buffer count/size
// - set mapping between ks pins(?) and audio buffers
// - perform processing tick

// for the sake of keeping things simple let's define an order of operations:
// - all state is scoped to a handle to \\.\SynchronousAudioRouter
// - SarCreateAudioBuffers should be called before any endpoints are created
//   it must be called exactly once
// - SarCreateEndpoint should be called to create all endpoints
// - SarMapAudioBuffer should be called after the buffers and endpoints are set up
// - SarAudioTick should be called finally
// Any configuration changes require the device to be closed and reopened.
// Device is exclusive access.
// Closing the SynchronousAudioRouter device automatically destroys all endpoints.

#define MAX_ENDPOINT_NAME_LENGTH 63

#define SAR_ENDPOINT_TYPE_CAPTURE 1
#define SAR_ENDPOINT_TYPE_PLAYBACK 2

#define SAR_REQUEST_CREATE_AUDIO_BUFFERS CTL_CODE( \
    FILE_DEVICE_SOUND, 1, METHOD_IN_DIRECT, FILE_READ_DATA | FILE_WRITE_DATA)
#define SAR_REQUEST_CREATE_ENDPOINT CTL_CODE( \
    FILE_DEVICE_SOUND, 2, METHOD_IN_DIRECT, FILE_READ_DATA | FILE_WRITE_DATA)
#define SAR_REQUEST_MAP_AUDIO_BUFFER CTL_CODE( \
    FILE_DEVICE_SOUND, 3, METHOD_IN_DIRECT, FILE_READ_DATA | FILE_WRITE_DATA)
#define SAR_REQUEST_AUDIO_TICK CTL_CODE( \
    FILE_DEVICE_SOUND, 4, METHOD_IN_DIRECT, FILE_READ_DATA | FILE_WRITE_DATA)

typedef struct SarCreateEndpointRequest
{
    DWORD type;
    DWORD index;
    DWORD channelCount;
    WCHAR name[MAX_ENDPOINT_NAME_LENGTH+1];
} SarCreateEndpointRequest;

typedef struct SarCreateAudioBuffersRequest
{
    DWORD bufferCount;
    DWORD bufferSize;
    DWORD sampleRate;
} SarSetAudioBuffersRequest;

typedef struct SarMapAudioBufferRequest
{
    DWORD type;
    DWORD index;
    DWORD channel;
    DWORD bufferIndex;
} SarMapAudioBuffersRequest;

#ifdef __cplusplus
} // extern "C"
#endif

#endif // _SAR_H