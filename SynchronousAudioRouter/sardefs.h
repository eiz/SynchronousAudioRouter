#ifndef _SAR_DEFS_H
#define _SAR_DEFS_H

#ifdef __cplusplus
extern "C" {
#endif

// things the sar ioctl needs to be able to do:
// - create a ks audio endpoint (capture or playback)
// - delete a ks audio endpoint
// - set asio buffer configuration
// - 

typedef struct SarRequest
{
    int requestType;

} SarRequest;

#ifdef __cplusplus
} // extern "C"
#endif

#endif // _SAR_DEFS_H