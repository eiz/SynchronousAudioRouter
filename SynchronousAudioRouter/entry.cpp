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

#include <initguid.h>
#include "sar.h"

static void SarDeleteRegistryRedirect(PVOID ptr);

DRIVER_UNLOAD SarUnload;
EX_CALLBACK_FUNCTION SarRegistryCallback;
_Dispatch_type_(IRP_MJ_CREATE) DRIVER_DISPATCH SarIrpCreate;
_Dispatch_type_(IRP_MJ_DEVICE_CONTROL) DRIVER_DISPATCH SarIrpDeviceControl;
_Dispatch_type_(IRP_MJ_CLOSE) DRIVER_DISPATCH SarIrpClose;
_Dispatch_type_(IRP_MJ_CLEANUP) DRIVER_DISPATCH SarIrpCleanup;

static KSDEVICE_DISPATCH gDeviceDispatch = {
    SarKsDeviceAdd, // Add
    nullptr, // Start
    SarKsDevicePostStart, // PostStart
    nullptr, // QueryStop
    nullptr, // CancelStop
    nullptr, // Stop
    nullptr, // QueryRemove
    nullptr, // CancelRemove
    nullptr, // Remove
    nullptr, // QueryCapabilities
    nullptr, // SurpriseRemoval
    nullptr, // QueryPower
    nullptr, // SetPower
    nullptr  // QueryInterface
};

static const KSDEVICE_DESCRIPTOR gDeviceDescriptor = {
    &gDeviceDispatch,
    0, nullptr,
    KSDEVICE_DESCRIPTOR_VERSION
};

SarControlContext *SarCreateControlContext(PFILE_OBJECT fileObject)
{
    SarControlContext *controlContext;

    controlContext = (SarControlContext *)ExAllocatePool2(
        POOL_FLAG_NON_PAGED, sizeof(SarControlContext), SAR_TAG);

    if (!controlContext) {
        return nullptr;
    }

    RtlZeroMemory(controlContext, sizeof(SarControlContext));
    controlContext->refs = 1;
    controlContext->fileObject = fileObject;
    ExInitializeFastMutex(&controlContext->mutex);
    InitializeListHead(&controlContext->endpointList);
    InitializeListHead(&controlContext->pendingEndpointList);
    SarInitializeHandleQueue(&controlContext->handleQueue);
    controlContext->workItem = IoAllocateWorkItem(
        controlContext->fileObject->DeviceObject);

    if (!controlContext->workItem) {
        ExFreePoolWithTag(controlContext, SAR_TAG);
        return nullptr;
    }

    SAR_DEBUG("Created controlContext %p", controlContext);

    return controlContext;
}

VOID SarDeleteControlContext(SarControlContext *controlContext)
{
    SAR_DEBUG("Deleting controlContext %p", controlContext);

    while (!IsListEmpty(&controlContext->endpointList)) {
        PLIST_ENTRY entry = controlContext->endpointList.Flink;
        SarEndpoint *endpoint =
            CONTAINING_RECORD(entry, SarEndpoint, listEntry);

        RemoveEntryList(&endpoint->listEntry);
        SarReleaseEndpoint(endpoint);
    }

    while (!IsListEmpty(&controlContext->pendingEndpointList)) {
        PLIST_ENTRY entry = controlContext->pendingEndpointList.Flink;
        SarEndpoint *endpoint =
            CONTAINING_RECORD(entry, SarEndpoint, listEntry);

        RemoveEntryList(&endpoint->listEntry);
        SarReleaseEndpoint(endpoint);
    }

    if (controlContext->workItem) {
        IoFreeWorkItem(controlContext->workItem);
        controlContext->workItem = nullptr;
    }

    if (controlContext->sectionViewBaseAddress) {
        ZwUnmapViewOfSection(ZwCurrentProcess(), controlContext->sectionViewBaseAddress);
        controlContext->sectionViewBaseAddress = nullptr;
    }

    if (controlContext->bufferSection) {
        ZwClose(controlContext->bufferSection);
        controlContext->bufferSection = nullptr;
    }

    if (controlContext->bufferMapStorage) {
        ExFreePoolWithTag(controlContext->bufferMapStorage, SAR_TAG);
        controlContext->bufferMapStorage = nullptr;
    }

    ExFreePoolWithTag(controlContext, SAR_TAG);
}

BOOLEAN SarOrphanControlContext(SarDriverExtension *extension, PIRP irp)
{
    PIO_STACK_LOCATION irpStack = IoGetCurrentIrpStackLocation(irp);
    SarControlContext *controlContext;
    LIST_ENTRY orphanEndpoints;

    ExAcquireFastMutex(&extension->mutex);
    controlContext = (SarControlContext *)SarGetTableEntry(
        &extension->controlContextTable, irpStack->FileObject);

    if (controlContext) {
        SarRemoveTableEntry(
            &extension->controlContextTable, irpStack->FileObject);
    }

    ExReleaseFastMutex(&extension->mutex);

    if (!controlContext) {
        SAR_TRACE("controlContext not found ! FileObject: %p", irpStack->FileObject);
        return FALSE;
    }


    SAR_TRACE("Orphaning controlContext %p, FileObject: %p", controlContext, irpStack->FileObject);

    ExAcquireFastMutex(&controlContext->mutex);
    controlContext->orphan = TRUE;
    InitializeListHead(&orphanEndpoints);

    if (!IsListEmpty(&controlContext->endpointList)) {
        PLIST_ENTRY entry = controlContext->endpointList.Flink;

        RemoveEntryList(&controlContext->endpointList);
        InitializeListHead(&controlContext->endpointList);
        AppendTailList(&orphanEndpoints, entry);
    }

    ExReleaseFastMutex(&controlContext->mutex);

    while (!IsListEmpty(&orphanEndpoints)) {
        SarEndpoint *endpoint =
            CONTAINING_RECORD(orphanEndpoints.Flink, SarEndpoint, listEntry);

        RemoveEntryList(&endpoint->listEntry);
        SarOrphanEndpoint(endpoint);
    }

    SarCancelAllHandleQueueIrps(&controlContext->handleQueue);

    if (SarReleaseControlContext(controlContext) == FALSE) {
        SAR_TRACE("controlContext orphaned but not deleted: %p, refs: %d", controlContext, controlContext->refs);
    }
    return TRUE;
}

template<class Key, class T>
static const char* getKsElementName(Key key, T mapping) {
    for (size_t i = 0; mapping[i].name != NULL; i++) {
        if (mapping[i].key == key)
            return mapping[i].name;
    }
    return nullptr;
}

#ifdef SAR_DEBUG_KS_PROPERTIES
static void SarToHexString(const char* input, int size, char* output) {
    static const char* const hexMapping[] = {
        "00", "01", "02", "03", "04", "05", "06", "07", "08", "09", "0A", "0B", "0C", "0D", "0E", "0F", "10", "11",
        "12", "13", "14", "15", "16", "17", "18", "19", "1A", "1B", "1C", "1D", "1E", "1F", "20", "21", "22", "23",
        "24", "25", "26", "27", "28", "29", "2A", "2B", "2C", "2D", "2E", "2F", "30", "31", "32", "33", "34", "35",
        "36", "37", "38", "39", "3A", "3B", "3C", "3D", "3E", "3F", "40", "41", "42", "43", "44", "45", "46", "47",
        "48", "49", "4A", "4B", "4C", "4D", "4E", "4F", "50", "51", "52", "53", "54", "55", "56", "57", "58", "59",
        "5A", "5B", "5C", "5D", "5E", "5F", "60", "61", "62", "63", "64", "65", "66", "67", "68", "69", "6A", "6B",
        "6C", "6D", "6E", "6F", "70", "71", "72", "73", "74", "75", "76", "77", "78", "79", "7A", "7B", "7C", "7D",
        "7E", "7F", "80", "81", "82", "83", "84", "85", "86", "87", "88", "89", "8A", "8B", "8C", "8D", "8E", "8F",
        "90", "91", "92", "93", "94", "95", "96", "97", "98", "99", "9A", "9B", "9C", "9D", "9E", "9F", "A0", "A1",
        "A2", "A3", "A4", "A5", "A6", "A7", "A8", "A9", "AA", "AB", "AC", "AD", "AE", "AF", "B0", "B1", "B2", "B3",
        "B4", "B5", "B6", "B7", "B8", "B9", "BA", "BB", "BC", "BD", "BE", "BF", "C0", "C1", "C2", "C3", "C4", "C5",
        "C6", "C7", "C8", "C9", "CA", "CB", "CC", "CD", "CE", "CF", "D0", "D1", "D2", "D3", "D4", "D5", "D6", "D7",
        "D8", "D9", "DA", "DB", "DC", "DD", "DE", "DF", "E0", "E1", "E2", "E3", "E4", "E5", "E6", "E7", "E8", "E9",
        "EA", "EB", "EC", "ED", "EE", "EF", "F0", "F1", "F2", "F3", "F4", "F5", "F6", "F7", "F8", "F9", "FA", "FB",
        "FC", "FD", "FE", "FF" };

    for (int i = 0; i < size; i += 16) {
        int maxCharNum = size - i;
        if (maxCharNum > 16)
            maxCharNum = 16;

        *(output++) = hexMapping[(i >> 8) & 0xFF][0];
        *(output++) = hexMapping[(i >> 8) & 0xFF][1];
        *(output++) = hexMapping[(i) & 0xFF][0];
        *(output++) = hexMapping[(i) & 0xFF][1];
        *(output++) = ':';
        *(output++) = ' ';

        for (int row = 0; row < maxCharNum; row++) {
                *(output++) = hexMapping[(unsigned char)input[i + row]][0];
                *(output++) = hexMapping[(unsigned char)input[i + row]][1];
                *(output++) = ' ';
        }
        *(output++) = '\n';
    }
    *(output++) = 0;
}
#endif

static void SarLogKsIrp(PIRP irp) {
    UNREFERENCED_PARAMETER(irp);

#ifdef SAR_DEBUG_KS_PROPERTIES
    PIO_STACK_LOCATION irpStack = IoGetCurrentIrpStackLocation(irp);
    ULONG ioControlCode = irpStack->Parameters.DeviceIoControl.IoControlCode;
    if (ioControlCode == IOCTL_KS_PROPERTY)
    {

        struct SetMapping {
            const GUID key;
            const char* name;
        };
#pragma region Mappings

#define NAMED_GUID(x) {x, #x}
        static const SetMapping knownSets[] = {
            NAMED_GUID(KSPROPSETID_AC3),
            NAMED_GUID(KSPROPSETID_Audio),
            NAMED_GUID(KSPROPSETID_AudioBufferDuration),
            NAMED_GUID(KSPROPSETID_AudioDecoderOut),
            NAMED_GUID(KSPROPSETID_AudioEngine),
            NAMED_GUID(KSPROPSETID_AudioSignalProcessing),
            NAMED_GUID(KSPROPSETID_Bibliographic),
            NAMED_GUID(KSPROPSETID_BtAudio),
            NAMED_GUID(KSPROPSETID_Clock),
            NAMED_GUID(KSPROPSETID_Connection),
            NAMED_GUID(KSPROPSETID_CopyProt),
            NAMED_GUID(KSPROPSETID_Cyclic),
            NAMED_GUID(KSPROPSETID_DirectSound3DBuffer),
            NAMED_GUID(KSPROPSETID_DirectSound3DListener),
            NAMED_GUID(KSPROPSETID_DrmAudioStream),
            NAMED_GUID(KSPROPSETID_DvdSubPic),
            NAMED_GUID(KSPROPSETID_General),
            NAMED_GUID(KSPROPSETID_GM),
            NAMED_GUID(KSPROPSETID_Hrtf3d),
            NAMED_GUID(KSPROPSETID_Itd3d),
            NAMED_GUID(KSPROPSETID_Jack),
            NAMED_GUID(KSPROPSETID_MediaSeeking),
            NAMED_GUID(KSPROPSETID_MemoryTransport),
            NAMED_GUID(KSPROPSETID_Mpeg2Vid),
            NAMED_GUID(KSPROPSETID_MPEG4_MediaType_Attributes),
            NAMED_GUID(KSPROPSETID_OverlayUpdate),
            NAMED_GUID(KSPROPSETID_Pin),
            NAMED_GUID(KSPROPSETID_PinMDLCacheClearProp),
            NAMED_GUID(KSPROPSETID_Quality),
            NAMED_GUID(KSPROPSETID_RtAudio),
            NAMED_GUID(KSPROPSETID_SoundDetector),
            NAMED_GUID(KSPROPSETID_Stream),
            NAMED_GUID(KSPROPSETID_StreamAllocator),
            NAMED_GUID(KSPROPSETID_StreamInterface),
            NAMED_GUID(KSPROPSETID_Topology),
            NAMED_GUID(KSPROPSETID_TopologyNode),
            NAMED_GUID(KSPROPSETID_TSRateChange),
            NAMED_GUID(KSPROPSETID_VBICAP_PROPERTIES),
            NAMED_GUID(KSPROPSETID_VBICodecFiltering),
            NAMED_GUID(KSPROPSETID_VPConfig),
            NAMED_GUID(KSPROPSETID_VPVBIConfig),
            NAMED_GUID(KSPROPSETID_VramCapture),
            NAMED_GUID(KSPROPSETID_Wave),
            NAMED_GUID(PROPSETID_ALLOCATOR_CONTROL),
            NAMED_GUID(PROPSETID_EXT_DEVICE),
            NAMED_GUID(PROPSETID_EXT_TRANSPORT),
            NAMED_GUID(PROPSETID_TIMECODE_READER),
            NAMED_GUID(PROPSETID_TUNER),
            NAMED_GUID(PROPSETID_VIDCAP_CAMERACONTROL),
            NAMED_GUID(PROPSETID_VIDCAP_CROSSBAR),
            NAMED_GUID(PROPSETID_VIDCAP_DROPPEDFRAMES),
            NAMED_GUID(PROPSETID_VIDCAP_SELECTOR),
            NAMED_GUID(PROPSETID_VIDCAP_TVAUDIO),
            NAMED_GUID(PROPSETID_VIDCAP_VIDEOCOMPRESSION),
            NAMED_GUID(PROPSETID_VIDCAP_VIDEOCONTROL),
            NAMED_GUID(PROPSETID_VIDCAP_VIDEODECODER),
            NAMED_GUID(PROPSETID_VIDCAP_VIDEOENCODER),
            NAMED_GUID(PROPSETID_VIDCAP_VIDEOPROCAMP),
            {{0}, NULL}
        };
#undef NAMED_GUID

        struct PropertyNameMapping {
            ULONG key;
            const char* name;
        };

#define NAMED_PROPERTY(x) {x, #x}
        static const PropertyNameMapping pinPropertiesMapping[] = {
            NAMED_PROPERTY(KSPROPERTY_PIN_CINSTANCES),
            NAMED_PROPERTY(KSPROPERTY_PIN_CTYPES),
            NAMED_PROPERTY(KSPROPERTY_PIN_DATAFLOW),
            NAMED_PROPERTY(KSPROPERTY_PIN_DATARANGES),
            NAMED_PROPERTY(KSPROPERTY_PIN_DATAINTERSECTION),
            NAMED_PROPERTY(KSPROPERTY_PIN_INTERFACES),
            NAMED_PROPERTY(KSPROPERTY_PIN_MEDIUMS),
            NAMED_PROPERTY(KSPROPERTY_PIN_COMMUNICATION),
            NAMED_PROPERTY(KSPROPERTY_PIN_GLOBALCINSTANCES),
            NAMED_PROPERTY(KSPROPERTY_PIN_NECESSARYINSTANCES),
            NAMED_PROPERTY(KSPROPERTY_PIN_PHYSICALCONNECTION),
            NAMED_PROPERTY(KSPROPERTY_PIN_CATEGORY),
            NAMED_PROPERTY(KSPROPERTY_PIN_NAME),
            NAMED_PROPERTY(KSPROPERTY_PIN_CONSTRAINEDDATARANGES),
            NAMED_PROPERTY(KSPROPERTY_PIN_PROPOSEDATAFORMAT),
            NAMED_PROPERTY(KSPROPERTY_PIN_PROPOSEDATAFORMAT2),
            {0, NULL}
        };

#define NAMED_PROPERTY(x) {x, #x}
        static const PropertyNameMapping audioPropertiesMapping[] = {
            NAMED_PROPERTY(KSPROPERTY_AUDIO_LATENCY),
            NAMED_PROPERTY(KSPROPERTY_AUDIO_COPY_PROTECTION),
            NAMED_PROPERTY(KSPROPERTY_AUDIO_CHANNEL_CONFIG),
            NAMED_PROPERTY(KSPROPERTY_AUDIO_VOLUMELEVEL),
            NAMED_PROPERTY(KSPROPERTY_AUDIO_POSITION),
            NAMED_PROPERTY(KSPROPERTY_AUDIO_DYNAMIC_RANGE),
            NAMED_PROPERTY(KSPROPERTY_AUDIO_QUALITY),
            NAMED_PROPERTY(KSPROPERTY_AUDIO_SAMPLING_RATE),
            NAMED_PROPERTY(KSPROPERTY_AUDIO_DYNAMIC_SAMPLING_RATE),
            NAMED_PROPERTY(KSPROPERTY_AUDIO_MIX_LEVEL_TABLE),
            NAMED_PROPERTY(KSPROPERTY_AUDIO_MIX_LEVEL_CAPS),
            NAMED_PROPERTY(KSPROPERTY_AUDIO_MUX_SOURCE),
            NAMED_PROPERTY(KSPROPERTY_AUDIO_MUTE),
            NAMED_PROPERTY(KSPROPERTY_AUDIO_BASS),
            NAMED_PROPERTY(KSPROPERTY_AUDIO_MID),
            NAMED_PROPERTY(KSPROPERTY_AUDIO_TREBLE),
            NAMED_PROPERTY(KSPROPERTY_AUDIO_BASS_BOOST),
            NAMED_PROPERTY(KSPROPERTY_AUDIO_EQ_LEVEL),
            NAMED_PROPERTY(KSPROPERTY_AUDIO_NUM_EQ_BANDS),
            NAMED_PROPERTY(KSPROPERTY_AUDIO_EQ_BANDS),
            NAMED_PROPERTY(KSPROPERTY_AUDIO_AGC),
            NAMED_PROPERTY(KSPROPERTY_AUDIO_DELAY),
            NAMED_PROPERTY(KSPROPERTY_AUDIO_LOUDNESS),
            NAMED_PROPERTY(KSPROPERTY_AUDIO_WIDE_MODE),
            NAMED_PROPERTY(KSPROPERTY_AUDIO_WIDENESS),
            NAMED_PROPERTY(KSPROPERTY_AUDIO_REVERB_LEVEL),
            NAMED_PROPERTY(KSPROPERTY_AUDIO_CHORUS_LEVEL),
            NAMED_PROPERTY(KSPROPERTY_AUDIO_DEV_SPECIFIC),
            NAMED_PROPERTY(KSPROPERTY_AUDIO_DEMUX_DEST),
            NAMED_PROPERTY(KSPROPERTY_AUDIO_STEREO_ENHANCE),
            NAMED_PROPERTY(KSPROPERTY_AUDIO_MANUFACTURE_GUID),
            NAMED_PROPERTY(KSPROPERTY_AUDIO_PRODUCT_GUID),
            NAMED_PROPERTY(KSPROPERTY_AUDIO_CPU_RESOURCES),
            NAMED_PROPERTY(KSPROPERTY_AUDIO_STEREO_SPEAKER_GEOMETRY),
            NAMED_PROPERTY(KSPROPERTY_AUDIO_SURROUND_ENCODE),
            NAMED_PROPERTY(KSPROPERTY_AUDIO_3D_INTERFACE),
            NAMED_PROPERTY(KSPROPERTY_AUDIO_PEAKMETER),
            NAMED_PROPERTY(KSPROPERTY_AUDIO_ALGORITHM_INSTANCE),
            NAMED_PROPERTY(KSPROPERTY_AUDIO_FILTER_STATE),
            NAMED_PROPERTY(KSPROPERTY_AUDIO_PREFERRED_STATUS),
            NAMED_PROPERTY(KSPROPERTY_AUDIO_PEQ_MAX_BANDS),
            NAMED_PROPERTY(KSPROPERTY_AUDIO_PEQ_NUM_BANDS),
            NAMED_PROPERTY(KSPROPERTY_AUDIO_PEQ_BAND_CENTER_FREQ),
            NAMED_PROPERTY(KSPROPERTY_AUDIO_PEQ_BAND_Q_FACTOR),
            NAMED_PROPERTY(KSPROPERTY_AUDIO_PEQ_BAND_LEVEL),
            NAMED_PROPERTY(KSPROPERTY_AUDIO_CHORUS_MODULATION_RATE),
            NAMED_PROPERTY(KSPROPERTY_AUDIO_CHORUS_MODULATION_DEPTH),
            NAMED_PROPERTY(KSPROPERTY_AUDIO_REVERB_TIME),
            NAMED_PROPERTY(KSPROPERTY_AUDIO_REVERB_DELAY_FEEDBACK),
            NAMED_PROPERTY(KSPROPERTY_AUDIO_POSITIONEX),
            NAMED_PROPERTY(KSPROPERTY_AUDIO_MIC_ARRAY_GEOMETRY),
            {0, NULL}
        };

        static const PropertyNameMapping rtaudioPropertiesMapping[] = {
            NAMED_PROPERTY(KSPROPERTY_RTAUDIO_GETPOSITIONFUNCTION),
            NAMED_PROPERTY(KSPROPERTY_RTAUDIO_BUFFER),
            NAMED_PROPERTY(KSPROPERTY_RTAUDIO_HWLATENCY),
            NAMED_PROPERTY(KSPROPERTY_RTAUDIO_POSITIONREGISTER),
            NAMED_PROPERTY(KSPROPERTY_RTAUDIO_CLOCKREGISTER),
            NAMED_PROPERTY(KSPROPERTY_RTAUDIO_BUFFER_WITH_NOTIFICATION),
            NAMED_PROPERTY(KSPROPERTY_RTAUDIO_REGISTER_NOTIFICATION_EVENT),
            NAMED_PROPERTY(KSPROPERTY_RTAUDIO_UNREGISTER_NOTIFICATION_EVENT),
            NAMED_PROPERTY(KSPROPERTY_RTAUDIO_QUERY_NOTIFICATION_SUPPORT),
            {9, "KSPROPERTY_RTAUDIO_PACKETCOUNT"},
            {10, "KSPROPERTY_RTAUDIO_PRESENTATION_POSITION"},
            {11, "KSPROPERTY_RTAUDIO_GETREADPACKET"},
            {12, "KSPROPERTY_RTAUDIO_SETWRITEPACKET"},
            {13, "KSPROPERTY_RTAUDIO_PACKETVREGISTER"},
            {0, NULL}
        };

        static const PropertyNameMapping connectionPropertiesMapping[] = {
            NAMED_PROPERTY(KSPROPERTY_CONNECTION_PRIORITY),
            NAMED_PROPERTY(KSPROPERTY_CONNECTION_DATAFORMAT),
            NAMED_PROPERTY(KSPROPERTY_CONNECTION_ALLOCATORFRAMING),
            NAMED_PROPERTY(KSPROPERTY_CONNECTION_PROPOSEDATAFORMAT),
            NAMED_PROPERTY(KSPROPERTY_CONNECTION_ACQUIREORDERING),
            NAMED_PROPERTY(KSPROPERTY_CONNECTION_ALLOCATORFRAMING_EX),
            NAMED_PROPERTY(KSPROPERTY_CONNECTION_STARTAT),
            {0, NULL}
        };


#pragma endregion

        KSPROPERTY property;
        char inputBuffer[1024];
        char inputBufferString[4096];
        NTSTATUS result = SarReadUserBuffer(
            &property, irp, sizeof(property));
        int size = irpStack->Parameters.DeviceIoControl.InputBufferLength;
        if (size > 1024)
            size = 1024;

        SarReadUserBuffer(
            inputBuffer, irp, size);

        if (result == 0) {
            const char* setName = getKsElementName(property.Set, knownSets);
            const char* propertyNameStr = nullptr;
            char propertyName[256];
            if (property.Set == KSPROPSETID_Pin)
                propertyNameStr = getKsElementName(property.Id, pinPropertiesMapping);
            else if (property.Set == KSPROPSETID_Audio)
                propertyNameStr = getKsElementName(property.Id, audioPropertiesMapping);
            else if (property.Set == KSPROPSETID_RtAudio)
                propertyNameStr = getKsElementName(property.Id, rtaudioPropertiesMapping);
            else if (property.Set == KSPROPSETID_Connection)
                propertyNameStr = getKsElementName(property.Id, connectionPropertiesMapping);

            if(propertyNameStr)
                strncpy(propertyName, propertyNameStr, sizeof(propertyName));
            else
                _snprintf(propertyName, sizeof(propertyName), "%d", property.Id);

            SAR_TRACE("IRQ KS_PROPERTY: Set: %s, Property: %s, Flags: 0x%x, inputSize: %d, outputSize: %d",
                setName, propertyName, property.Flags, irpStack->Parameters.DeviceIoControl.InputBufferLength, irpStack->Parameters.DeviceIoControl.OutputBufferLength);
        }
        else {
            SAR_TRACE("IRQ KS_PROPERTY: unknown (size too small: %d), error: 0x%x, inputSize: %d, outputSize: %d", irpStack->Parameters.DeviceIoControl.InputBufferLength, result, irpStack->Parameters.DeviceIoControl.InputBufferLength, irpStack->Parameters.DeviceIoControl.OutputBufferLength);
        }

        if (size < 1024) {
            SarToHexString(inputBuffer, size, inputBufferString);
            SAR_TRACE("Data: %s", inputBufferString);
        }

    }
    else
    {
        SAR_TRACE("IRQ 0x%x", ioControlCode);
    }
#endif
}

BOOL SarIsControlIrp(PIRP irp) {
    UNICODE_STRING referencePath;
    PIO_STACK_LOCATION irpStack = IoGetCurrentIrpStackLocation(irp);

    RtlUnicodeStringInit(&referencePath, SAR_CONTROL_REFERENCE_STRING);

    return RtlCompareUnicodeString(&irpStack->FileObject->FileName, &referencePath, TRUE) == 0;
}

NTSTATUS SarIrpCreate(PDEVICE_OBJECT deviceObject, PIRP irp)
{
    NTSTATUS status = STATUS_SUCCESS;
    PIO_STACK_LOCATION irpStack = IoGetCurrentIrpStackLocation(irp);
    SarDriverExtension *extension =
        (SarDriverExtension *)IoGetDriverObjectExtension(
            deviceObject->DriverObject, DriverEntry);
    SarControlContext *controlContext = nullptr;

    SAR_DEBUG("IRP_MJ_CREATE %wZ", &irpStack->FileObject->FileName);

    if (!SarIsControlIrp(irp)) {
        NTSTATUS result = extension->ksDispatchCreate(deviceObject, irp);
        return result;
    }

    controlContext = SarCreateControlContext(irpStack->FileObject);

    if (!controlContext) {
        status = STATUS_INSUFFICIENT_RESOURCES;
        goto out;
    }

    ExAcquireFastMutex(&extension->mutex);
    status = SarInsertTableEntry(
        &extension->controlContextTable, irpStack->FileObject,
        controlContext);
    ExReleaseFastMutex(&extension->mutex);

    if (!NT_SUCCESS(status)) {
        goto out;
    }

    irpStack->FileObject->FsContext2 = controlContext;

out:
    if (controlContext && !NT_SUCCESS(status)) {
        SarReleaseControlContext(controlContext);
    }

    irp->IoStatus.Status = status;
    IoCompleteRequest(irp, IO_NO_INCREMENT);
    return status;
}

NTSTATUS SarIrpClose(PDEVICE_OBJECT deviceObject, PIRP irp)
{
    NTSTATUS status;
    PIO_STACK_LOCATION irpStack = IoGetCurrentIrpStackLocation(irp);
    SarDriverExtension *extension =
        (SarDriverExtension *)IoGetDriverObjectExtension(
            deviceObject->DriverObject, DriverEntry);

    UNREFERENCED_PARAMETER(irpStack);

    if (!SarIsControlIrp(irp)) {
        SAR_DEBUG("close KS %wZ", &irpStack->FileObject->FileName);
        return extension->ksDispatchClose(deviceObject, irp);
    }

    SAR_DEBUG("IRP_MJ_CLOSE: %wZ", &irpStack->FileObject->FileName);
    SarOrphanControlContext(extension, irp);

    status = STATUS_SUCCESS;
    irp->IoStatus.Status = status;
    IoCompleteRequest(irp, IO_NO_INCREMENT);
    return status;
}

NTSTATUS SarIrpCleanup(PDEVICE_OBJECT deviceObject, PIRP irp)
{
    NTSTATUS status;
    PIO_STACK_LOCATION irpStack = IoGetCurrentIrpStackLocation(irp);
    SarDriverExtension *extension =
        (SarDriverExtension *)IoGetDriverObjectExtension(
            deviceObject->DriverObject, DriverEntry);

    UNREFERENCED_PARAMETER(irpStack);

    if (!SarIsControlIrp(irp)) {
        SAR_DEBUG("cleanup KS %wZ", &irpStack->FileObject->FileName);
        return extension->ksDispatchCleanup(deviceObject, irp);
    }

    SAR_DEBUG("IRP_MJ_CLEANUP: %wZ", &irpStack->FileObject->FileName);
    SarOrphanControlContext(extension, irp);

    status = STATUS_SUCCESS;
    irp->IoStatus.Status = status;
    IoCompleteRequest(irp, IO_NO_INCREMENT);
    return status;
}

NTSTATUS SarIrpDeviceControl(PDEVICE_OBJECT deviceObject, PIRP irp)
{
    PIO_STACK_LOCATION irpStack;
    ULONG ioControlCode;
    NTSTATUS ntStatus = STATUS_SUCCESS;
    SarDriverExtension *extension =
        (SarDriverExtension *)IoGetDriverObjectExtension(
            deviceObject->DriverObject, DriverEntry);

    if (!SarIsControlIrp(irp)) {
        SarLogKsIrp(irp);
        return extension->ksDispatchDeviceControl(deviceObject, irp);
    }

    irpStack = IoGetCurrentIrpStackLocation(irp);
    ioControlCode = irpStack->Parameters.DeviceIoControl.IoControlCode;

    ExAcquireFastMutex(&extension->mutex);
    SarControlContext * controlContext = (SarControlContext *)SarGetTableEntry(
        &extension->controlContextTable, irpStack->FileObject);
    ExReleaseFastMutex(&extension->mutex);

    switch (ioControlCode) {
        case SAR_SET_BUFFER_LAYOUT: {
            SAR_INFO("create audio buffers");

            SarSetBufferLayoutRequest request;
            SarSetBufferLayoutResponse response;

            ntStatus = SarReadUserBuffer(
                &request, irp, sizeof(SarSetBufferLayoutRequest));

            if (!NT_SUCCESS(ntStatus)) {
                break;
            }

            ntStatus = SarSetBufferLayout(controlContext, &request, &response);

            if (!NT_SUCCESS(ntStatus)) {
                break;
            }

            ntStatus = SarWriteUserBuffer(
                &response, irp, sizeof(SarSetBufferLayoutResponse));
            break;
        }
        case SAR_CREATE_ENDPOINT: {
            SAR_INFO("create audio endpoint");
            SarCreateEndpointRequest request;

            ntStatus = SarReadUserBuffer(
                &request, irp, sizeof(SarCreateEndpointRequest));

            if (!NT_SUCCESS(ntStatus)) {
                break;
            }

            ntStatus = SarCreateEndpoint(
                deviceObject, irp, controlContext, &request);
            break;
        }
        case SAR_WAIT_HANDLE_QUEUE: {
            SAR_INFO("wait handle queue");
            ntStatus = SarWaitHandleQueue(&controlContext->handleQueue, irp);
            break;
        }
        case SAR_START_REGISTRY_FILTER: {
            UNICODE_STRING filterAltitude;

            RtlUnicodeStringInit(&filterAltitude, L"360000");
            KeEnterCriticalRegion();
            ExAcquireFastMutexUnsafe(&extension->mutex);

            if (extension->filterCookie.QuadPart) {
                ExReleaseFastMutexUnsafe(&extension->mutex);
                KeLeaveCriticalRegion();
                ntStatus = STATUS_RESOURCE_IN_USE;
                break;
            }

            ntStatus = SarCopyProcessUser(
                PsGetCurrentProcess(), &extension->filterUser);

            if (!NT_SUCCESS(ntStatus)) {
                ExReleaseFastMutexUnsafe(&extension->mutex);
                KeLeaveCriticalRegion();
                break;
            }

            ntStatus = CmRegisterCallbackEx(
                SarRegistryCallback,
                &filterAltitude,
                deviceObject->DriverObject,
                extension,
                &extension->filterCookie,
                nullptr);
            ExReleaseFastMutexUnsafe(&extension->mutex);
            KeLeaveCriticalRegion();
            break;
        }
        case SAR_SEND_FORMAT_CHANGE_EVENT:
            ntStatus = SarSendFormatChangeEvent(deviceObject, extension);
            break;
        default:
            SAR_ERROR("Unknown ioctl %lu", ioControlCode);
            break;
    }

    if (ntStatus != STATUS_PENDING) {
        irp->IoStatus.Status = ntStatus;
        IoCompleteRequest(irp, IO_NO_INCREMENT);
    }

    SAR_INFO("device_control result: 0x%x", ntStatus);

    return ntStatus;
}

VOID SarUnload(PDRIVER_OBJECT driverObject)
{
    SAR_INFO("SAR is unloading");
    KIRQL irql;
    SarDriverExtension *extension =
        (SarDriverExtension *)IoGetDriverObjectExtension(
            driverObject, DriverEntry);

    if (extension->sarInterfaceName.Buffer) {
        RtlFreeUnicodeString(&extension->sarInterfaceName);
    }

    if (extension->filterCookie.QuadPart) {
        CmUnRegisterCallback(extension->filterCookie);
    }

    irql = ExAcquireSpinLockExclusive(&extension->registryRedirectLock);
    SarClearStringTable(
        &extension->registryRedirectTableWow64, SarDeleteRegistryRedirect);
    SarClearStringTable(
        &extension->registryRedirectTable, SarDeleteRegistryRedirect);
    ExReleaseSpinLockExclusive(&extension->registryRedirectLock, irql);

    if (extension->filterUser) {
        // Allocated by SeQueryInformationToken
        ExFreePool(extension->filterUser);
    }
}

BOOL SarFilterMatchesCurrentProcess(SarDriverExtension *extension)
{
    PTOKEN_USER tokenUser = nullptr;
    NTSTATUS status = SarCopyProcessUser(PsGetCurrentProcess(), &tokenUser);

    if (!NT_SUCCESS(status)) {
        return FALSE;
    }

    BOOL isMatch = RtlEqualSid(
        extension->filterUser->User.Sid,
        tokenUser->User.Sid);

    ExFreePool(tokenUser);
    return isMatch;
}

BOOL SarFilterMatchesPath(
    SarDriverExtension *extension,
    PCUNICODE_STRING path,
    PUNICODE_STRING redirectPath)
{
    PRTL_AVL_TABLE table = &extension->registryRedirectTable;
    PVOID entry = nullptr;
    KIRQL irql;
    UNICODE_STRING nonpagedPath;
    NTSTATUS status = STATUS_SUCCESS;

    if (!path) {
        return FALSE;
    }

#ifdef _WIN64
    if (IoIs32bitProcess(nullptr)) {
        table = &extension->registryRedirectTableWow64;
    }
#endif

    // TODO: we get a path that's in paged memory from the configuration manager
    // so we can't access it with IRQL raised to dispatch level by the spinlock.
    // Probably better to just use an ERESOURCE or something here instead.
    status = SarStringDuplicate(&nonpagedPath, path);

    if (!NT_SUCCESS(status)) {
        return FALSE;
    }

    irql = ExAcquireSpinLockShared(&extension->registryRedirectLock);
    entry = SarGetStringTableEntry(table, &nonpagedPath);

    if (entry) {
        *redirectPath = *(PCUNICODE_STRING)entry;
    }

    ExReleaseSpinLockShared(&extension->registryRedirectLock, irql);
    SarStringFree(&nonpagedPath);
    return entry != nullptr;
}

NTSTATUS SarFilterMMDeviceQuery(
    PREG_QUERY_VALUE_KEY_INFORMATION queryInfo,
    PUNICODE_STRING wrapperRegistrationPath)
{
    NTSTATUS status = STATUS_SUCCESS;
    OBJECT_ATTRIBUTES oa;
    HANDLE wrapperKey;
    ULONG resultLength = 0;
    UNICODE_STRING valueName = {};
    PVOID buffer = nullptr;

    InitializeObjectAttributes(
        &oa, wrapperRegistrationPath, OBJ_KERNEL_HANDLE,
        nullptr, nullptr);
    status = ZwOpenKeyEx(&wrapperKey, KEY_ALL_ACCESS, &oa, 0);

    if (!NT_SUCCESS(status)) {
        return STATUS_SUCCESS;
    }

    buffer = ExAllocatePool2(POOL_FLAG_PAGED, queryInfo->Length, SAR_TAG);

    if (!buffer) {
        ZwClose(wrapperKey);
        return STATUS_SUCCESS;
    }

    __try {
        RtlCopyMemory(
            buffer, queryInfo->KeyValueInformation, queryInfo->Length);
        status = ZwQueryValueKey(
            wrapperKey,
            &valueName,
            queryInfo->KeyValueInformationClass,
            buffer,
            queryInfo->Length,
            &resultLength);
        RtlCopyMemory(
            queryInfo->KeyValueInformation, buffer, queryInfo->Length);
        *queryInfo->ResultLength = resultLength;

        if (NT_SUCCESS(status)) {
            status = STATUS_CALLBACK_BYPASS;
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        status = GetExceptionCode();
    }

    ExFreePoolWithTag(buffer, SAR_TAG);
    ZwClose(wrapperKey);
    return status;
}

NTSTATUS SarFilterMMDeviceEnum(
    PREG_ENUMERATE_VALUE_KEY_INFORMATION queryInfo,
    PUNICODE_STRING wrapperRegistrationPath)
{
    NTSTATUS status = STATUS_SUCCESS;
    OBJECT_ATTRIBUTES oa;
    HANDLE wrapperKey;
    ULONG resultLength = 0;
    UNICODE_STRING valueName = {};
    PVOID buffer = nullptr;

    InitializeObjectAttributes(
        &oa, wrapperRegistrationPath, OBJ_KERNEL_HANDLE,
        nullptr, nullptr);
    status = ZwOpenKeyEx(&wrapperKey, KEY_ALL_ACCESS, &oa, 0);

    if (!NT_SUCCESS(status)) {
        return STATUS_SUCCESS;
    }

    buffer = ExAllocatePool2(POOL_FLAG_PAGED, queryInfo->Length, SAR_TAG);

    if (!buffer) {
        ZwClose(wrapperKey);
        return STATUS_SUCCESS;
    }

    __try {
        RtlCopyMemory(
            buffer, queryInfo->KeyValueInformation, queryInfo->Length);
        status = ZwEnumerateValueKey(
            wrapperKey,
            queryInfo->Index,
            queryInfo->KeyValueInformationClass,
            buffer,
            queryInfo->Length,
            &resultLength);
        RtlCopyMemory(
            queryInfo->KeyValueInformation, buffer, queryInfo->Length);
        *queryInfo->ResultLength = resultLength;

        if (NT_SUCCESS(status)) {
            status = STATUS_CALLBACK_BYPASS;
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        status = GetExceptionCode();
    }

    ExFreePoolWithTag(buffer, SAR_TAG);
    ZwClose(wrapperKey);
    return status;
}

#define CLSID_ROOT L"\\REGISTRY\\MACHINE\\SOFTWARE\\Classes\\CLSID\\"
#define WOW64_CLSID_ROOT \
    L"\\REGISTRY\\MACHINE\\SOFTWARE\\Classes\\Wow6432Node\\CLSID\\"

NTSTATUS SarRegistryCallback(PVOID context, PVOID argument1, PVOID argument2)
{
    UNREFERENCED_PARAMETER(argument2);

    UNICODE_STRING redirectPath;
    NTSTATUS status;
    REG_NOTIFY_CLASS notifyClass = (REG_NOTIFY_CLASS)(ULONG_PTR)argument1;
    SarDriverExtension *extension = (SarDriverExtension *)context;

    switch (notifyClass) {
        case RegNtQueryValueKey: {
            PREG_QUERY_VALUE_KEY_INFORMATION queryInfo =
                (PREG_QUERY_VALUE_KEY_INFORMATION)argument2;
            PCUNICODE_STRING objectName;

            // Only filter the default value
            if (!queryInfo || queryInfo->ValueName->Length != 0) {
                break;
            }

            status = CmCallbackGetKeyObjectID(
                &extension->filterCookie, queryInfo->Object,
                nullptr, &objectName);

            if (!NT_SUCCESS(status)) {
                break;
            }

            if (!SarFilterMatchesPath(extension, objectName, &redirectPath)) {
                break;
            }

            if (!SarFilterMatchesCurrentProcess(extension)) {
                break;
            }

            return SarFilterMMDeviceQuery(queryInfo, &redirectPath);
        }
        case RegNtEnumerateValueKey: {
            PREG_ENUMERATE_VALUE_KEY_INFORMATION queryInfo =
                (PREG_ENUMERATE_VALUE_KEY_INFORMATION)argument2;
            PCUNICODE_STRING objectName;

            if (!queryInfo || queryInfo->Index != 0) {
                break;
            }

            status = CmCallbackGetKeyObjectID(
                &extension->filterCookie, queryInfo->Object,
                nullptr, &objectName);

            if (!NT_SUCCESS(status)) {
                break;
            }

            if (!SarFilterMatchesPath(extension, objectName, &redirectPath)) {
                break;
            }

            if (!SarFilterMatchesCurrentProcess(extension)) {
                break;
            }

            return SarFilterMMDeviceEnum(queryInfo, &redirectPath);
        }
    }

    return STATUS_SUCCESS;
}

static void SarDeleteRegistryRedirect(PVOID ptr)
{
    PUNICODE_STRING str = (PUNICODE_STRING)ptr;

    if (str->Buffer) {
        SarStringFree(str);
    }

    ExFreePoolWithTag(str, SAR_TAG);
}

static NTSTATUS SarAddRegistryRedirect(
    PRTL_AVL_TABLE table, NTSTRSAFE_PCWSTR src, NTSTRSAFE_PCWSTR dst)
{
    NTSTATUS status = STATUS_SUCCESS;
    UNICODE_STRING srcLocal = {}, dstLocal = {};
    PUNICODE_STRING dstHeap = nullptr;

    RtlUnicodeStringInit(&srcLocal, src);
    RtlUnicodeStringInit(&dstLocal, dst);

    dstHeap = (PUNICODE_STRING)ExAllocatePool2(
        POOL_FLAG_NON_PAGED, sizeof(UNICODE_STRING), SAR_TAG);

    if (!dstHeap) {
        status = STATUS_INSUFFICIENT_RESOURCES;
        goto err;
    }

    RtlZeroMemory(dstHeap, sizeof(UNICODE_STRING));
    status = SarStringDuplicate(dstHeap, &dstLocal);

    if (!NT_SUCCESS(status)) {
        goto err;
    }

    status = SarInsertStringTableEntry(table, &srcLocal, dstHeap);

    if (!NT_SUCCESS(status)) {
        goto err;
    }

    return STATUS_SUCCESS;

err:
    if (dstHeap) {
        if (dstHeap->Buffer) {
            SarStringFree(dstHeap);
        }

        ExFreePoolWithTag(dstHeap, SAR_TAG);
    }

    return status;
}

#define REDIRECT_INPROC_WOW64(src, dst) \
    do { \
        status = SarAddRegistryRedirect(wow64, \
            WOW64_CLSID_ROOT src L"\\InprocServer32", \
            WOW64_CLSID_ROOT dst L"\\InprocServer32"); \
        if (!NT_SUCCESS(status)) { \
            return status; \
        } \
    } while (0)
#define REDIRECT_INPROC(src, dst) \
    do { \
        status = SarAddRegistryRedirect(table, \
            CLSID_ROOT src L"\\InprocServer32", \
            CLSID_ROOT dst L"\\InprocServer32"); \
        if (!NT_SUCCESS(status)) { \
            return status; \
        } \
    } while (0)
#define REDIRECT(src, dst) \
    do { \
        REDIRECT_INPROC_WOW64(src, dst); \
        REDIRECT_INPROC(src, dst); \
    } while (0)

static NTSTATUS SarAddAllRegistryRedirects(SarDriverExtension *extension)
{
    NTSTATUS status = STATUS_SUCCESS;
    PVOID p;
    PRTL_AVL_TABLE wow64 = &extension->registryRedirectTableWow64;
    PRTL_AVL_TABLE table = &extension->registryRedirectTable;

    // MMDeviceEnumerator
    REDIRECT(
        L"{BCDE0395-E52F-467C-8E3D-C4579291692E}",
        L"{9FB96668-9EDD-4574-AD77-76BD89659D5D}");
    // ActivateAudioInterfaceWorker
    REDIRECT(
        L"{E2F7A62A-862B-40AE-BBC2-5C0CA9A5B7E1}",
        L"{739191CC-CCBE-45D8-8D24-828D8E989E8E}"
    );

    for (p = RtlEnumerateGenericTableAvl(table, TRUE); p;
         p = RtlEnumerateGenericTableAvl(table, FALSE)) {

        SarStringTableEntry *entry = (SarStringTableEntry *)p;
        UNREFERENCED_PARAMETER(entry);

        SAR_DEBUG("Registry mapping: %wZ -> %wZ", &entry->key, entry->value);
    }

    for (p = RtlEnumerateGenericTableAvl(wow64, TRUE); p;
         p = RtlEnumerateGenericTableAvl(wow64, FALSE)) {

        SarStringTableEntry *entry = (SarStringTableEntry *)p;
        UNREFERENCED_PARAMETER(entry);

        SAR_DEBUG("WOW64 Registry mapping: %wZ -> %wZ",
            &entry->key, entry->value);
    }

    return STATUS_SUCCESS;
}

extern "C" NTSTATUS DriverEntry(
    IN PDRIVER_OBJECT driverObject,
    IN PUNICODE_STRING registryPath)
{
    SarDriverExtension *extension;
    NTSTATUS status;

    SAR_INFO("SAR is loading.");

    ExInitializeDriverRuntime(DrvRtPoolNxOptIn);

    status = IoAllocateDriverObjectExtension(
        driverObject, DriverEntry, sizeof(SarDriverExtension),
        (PVOID *)&extension);

    if (!NT_SUCCESS(status)) {
        return status;
    }

    status = KsInitializeDriver(driverObject, registryPath, &gDeviceDescriptor);

    if (!NT_SUCCESS(status)) {
        return status;
    }

    RtlZeroMemory(extension, sizeof(SarDriverExtension));
    ExInitializeFastMutex(&extension->mutex);
    SarInitializeTable(&extension->controlContextTable);
    SarInitializeStringTable(&extension->registryRedirectTableWow64);
    SarInitializeStringTable(&extension->registryRedirectTable);
    status = SarAddAllRegistryRedirects(extension);

    if (!NT_SUCCESS(status)) {
        SarUnload(driverObject);
        return status;
    }

    extension->ksDispatchCreate = driverObject->MajorFunction[IRP_MJ_CREATE];
    extension->ksDispatchClose = driverObject->MajorFunction[IRP_MJ_CLOSE];
    extension->ksDispatchCleanup = driverObject->MajorFunction[IRP_MJ_CLEANUP];
    extension->ksDispatchDeviceControl =
        driverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL];
    extension->sarInterfaceName = {};
    driverObject->DriverUnload = SarUnload;
    driverObject->MajorFunction[IRP_MJ_CREATE] = SarIrpCreate;
    driverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] = SarIrpDeviceControl;
    driverObject->MajorFunction[IRP_MJ_CLOSE] = SarIrpClose;
    driverObject->MajorFunction[IRP_MJ_CLEANUP] = SarIrpCleanup;
    return STATUS_SUCCESS;
}
