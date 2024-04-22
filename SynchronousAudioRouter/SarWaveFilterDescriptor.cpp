#include "SarWaveFilterDescriptor.h"
#include "sar.h"

#define DEFINE_KSPROPERTY_GETTER(id, handler, intype, outtype) \
    DEFINE_KSPROPERTY_ITEM((id), (handler), sizeof(intype), sizeof(outtype), \
    nullptr, nullptr, 0, nullptr, nullptr, 0)


NTSTATUS SarWaveFilterDescriptor::initWaveFilter(const SarControlContext *controlContext, const SarCreateEndpointRequest * request) {
    NTSTATUS result = STATUS_SUCCESS;

    const SarWaveFilterDescriptor self = {
        /* Pins Descriptors */
        /*********************/
        { // pinDispatch
            SarKsPinCreate, // Create
            SarKsPinClose, // Close
            SarKsPinProcess, // Process
            SarKsPinReset, // Reset
            SarKsPinSetDataFormat, // SetDataFormat
            SarKsPinSetDeviceState, // SetDeviceState
            SarKsPinConnect, // Connect
            SarKsPinDisconnect, // Disconnect
            nullptr, // Clock
            nullptr, // Allocator
        },
        { // pinRtAudioProperties
            DEFINE_KSPROPERTY_GETTER(
                KSPROPERTY_RTAUDIO_BUFFER, SarKsPinRtGetBuffer,
                KSRTAUDIO_BUFFER_PROPERTY, KSRTAUDIO_BUFFER),
            DEFINE_KSPROPERTY_GETTER(
                KSPROPERTY_RTAUDIO_BUFFER_WITH_NOTIFICATION,
                SarKsPinRtGetBufferWithNotification,
                KSRTAUDIO_BUFFER_PROPERTY, KSRTAUDIO_BUFFER),
            DEFINE_KSPROPERTY_GETTER(
                KSPROPERTY_RTAUDIO_CLOCKREGISTER, SarKsPinRtGetClockRegister,
                KSRTAUDIO_HWREGISTER_PROPERTY, KSRTAUDIO_HWREGISTER),
            DEFINE_KSPROPERTY_GETTER(
                KSPROPERTY_RTAUDIO_HWLATENCY, SarKsPinRtGetHwLatency,
                KSPROPERTY, KSRTAUDIO_HWLATENCY),
            DEFINE_KSPROPERTY_GETTER(
                KSPROPERTY_RTAUDIO_PACKETCOUNT, SarKsPinRtGetPacketCount,
                KSPROPERTY, ULONG),
            DEFINE_KSPROPERTY_GETTER(
                KSPROPERTY_RTAUDIO_POSITIONREGISTER, SarKsPinRtGetPositionRegister,
                KSRTAUDIO_HWREGISTER_PROPERTY, KSRTAUDIO_HWREGISTER),
            DEFINE_KSPROPERTY_GETTER(
                KSPROPERTY_RTAUDIO_PRESENTATION_POSITION,
                SarKsPinRtGetPresentationPosition,
                KSPROPERTY, KSAUDIO_PRESENTATION_POSITION),
            DEFINE_KSPROPERTY_GETTER(
                KSPROPERTY_RTAUDIO_QUERY_NOTIFICATION_SUPPORT,
                SarKsPinRtQueryNotificationSupport, KSPROPERTY, BOOL),
            DEFINE_KSPROPERTY_ITEM(
                KSPROPERTY_RTAUDIO_REGISTER_NOTIFICATION_EVENT,
                SarKsPinRtRegisterNotificationEvent,
                sizeof(KSRTAUDIO_NOTIFICATION_EVENT_PROPERTY), 0,
                SarKsPinRtRegisterNotificationEvent,
                nullptr, 0, nullptr, nullptr, 0),
            DEFINE_KSPROPERTY_ITEM(
                KSPROPERTY_RTAUDIO_UNREGISTER_NOTIFICATION_EVENT,
                SarKsPinRtUnregisterNotificationEvent,
                sizeof(KSRTAUDIO_NOTIFICATION_EVENT_PROPERTY), 0,
                SarKsPinRtUnregisterNotificationEvent,
                nullptr, 0, nullptr, nullptr, 0),
        },
        { // pinPropertySets
            DEFINE_KSPROPERTY_SET(
                &KSPROPSETID_RtAudio,
                SIZEOF_ARRAY(pinRtAudioProperties),
                pinRtAudioProperties,
                0,
                nullptr)
        },
        { // pinEvents
            DEFINE_KSEVENT_ITEM(
                KSEVENT_PINCAPS_FORMATCHANGE,
                sizeof(KSEVENTDATA), 0,
                nullptr, nullptr, nullptr)
        },
        { // pinEventSets
            DEFINE_KSEVENT_SET(
                &KSEVENTSETID_PinCapsChange,
                SIZEOF_ARRAY(pinEvents),
                pinEvents)
        },
        { // pinAutomation
            DEFINE_KSAUTOMATION_PROPERTIES(pinPropertySets),
            DEFINE_KSAUTOMATION_METHODS_NULL,
            DEFINE_KSAUTOMATION_EVENTS(pinEventSets)
        },
        { // pinInterface
            (GUID)KSINTERFACESETID_Standard,
            (ULONG)KSINTERFACE_STANDARD_LOOPED_STREAMING,
        },
        { // digitalDataRange
            { // DataRange
                sizeof(digitalDataRange), // FormatSize
                0, // Flags
                0, // SampleSize
                0, // Reserved
                KSDATAFORMAT_TYPE_AUDIO, // MajorFormat
                KSDATAFORMAT_SUBTYPE_PCM, // SubFormat
                KSDATAFORMAT_SPECIFIER_WAVEFORMATEX // Specifier
            },
            request->channelCount, // MaximumChannels
            controlContext->sampleSize * 8, // MinimumBitsPerSample
            controlContext->sampleSize * 8, // MaximumBitsPerSample
            controlContext->sampleRate, // MinimumSampleFrequency
            controlContext->sampleRate, // MaximumSampleFrequency
        },
        { // analogDataRange
            sizeof(analogDataRange), // FormatSize
            0, // Flags
            0, // SampleSize
            0, // Reserved
            KSDATAFORMAT_TYPE_AUDIO, // MajorFormat
            KSDATAFORMAT_SUBTYPE_ANALOG, // SubFormat
            KSDATAFORMAT_SPECIFIER_NONE // Specifier
        },
        { // dataRangePointers
            (PKSDATARANGE) &digitalDataRange,
            &analogDataRange
        },
        { // pinDesc
            { // Pin 0: digital pin
                &pinDispatch, // Dispatch
                &pinAutomation, // AutomationTable
                { // PinDescriptor
                    1u, // InterfacesCount
                    &pinInterface, // Interfaces
                    0u, // MediumsCount
                    nullptr, // Mediums
                    1u, // DataRangesCount
                    &dataRangePointers[0], // DataRanges
                    request->type == SAR_ENDPOINT_TYPE_RECORDING ? KSPIN_DATAFLOW_OUT : KSPIN_DATAFLOW_IN, // DataFlow
                    KSPIN_COMMUNICATION_SINK, // Communication
                    &KSCATEGORY_AUDIO, // Category
                    nullptr, // Name
                },
                KSPIN_FLAG_DO_NOT_INITIATE_PROCESSING |
                    KSPIN_FLAG_FRAMES_NOT_REQUIRED_FOR_PROCESSING |
                    KSPIN_FLAG_PROCESS_IF_ANY_IN_RUN_STATE |
                    KSPIN_FLAG_FIXED_FORMAT |
                    KSPIN_FLAG_DO_NOT_USE_STANDARD_TRANSPORT |
                    (request->type == SAR_ENDPOINT_TYPE_PLAYBACK ? KSPIN_FLAG_RENDERER : 0u), // Flags
                1u, // InstancesPossible
                0u, // InstancesNecessary
                nullptr, // AllocatorFraming
                SarKsPinIntersectHandler, // IntersectHandler
            },
            { // Pin 1: analog pin to topology filter
                nullptr, // Dispatch
                nullptr, // AutomationTable
                { // PinDescriptor
                    0u, // InterfacesCount
                    nullptr, // Interfaces
                    0u, // MediumsCount
                    nullptr, // Mediums
                    1u, // DataRangesCount
                    &dataRangePointers[1], // DataRanges
                    request->type == SAR_ENDPOINT_TYPE_RECORDING ? KSPIN_DATAFLOW_IN : KSPIN_DATAFLOW_OUT, // DataFlow
                    KSPIN_COMMUNICATION_NONE, // Communication
                    &KSCATEGORY_AUDIO, // Category
                    nullptr, // Name
                },
                KSPIN_FLAG_DO_NOT_INITIATE_PROCESSING |
                    KSPIN_FLAG_FRAMES_NOT_REQUIRED_FOR_PROCESSING |
                    KSPIN_FLAG_PROCESS_IF_ANY_IN_RUN_STATE |
                    KSPIN_FLAG_FIXED_FORMAT |
                    KSPIN_FLAG_DO_NOT_USE_STANDARD_TRANSPORT, // Flags
                1u, // InstancesPossible
                0u, // InstancesNecessary
                nullptr, // AllocatorFraming
                nullptr, // IntersectHandler
            },
        },

        // Nodes Descriptors
        //*********************
        { // nodeProperties
            DEFINE_KSPROPERTY_ITEM(
                KSPROPERTY_AUDIO_CHANNEL_CONFIG,
                SarKsNodeGetAudioChannelConfig,
                sizeof(KSNODEPROPERTY), sizeof(KSAUDIO_CHANNEL_CONFIG),
                SarKsNodeSetAudioChannelConfig, nullptr, 0, nullptr, nullptr, 0),
        },
        { // nodePropertySets
            DEFINE_KSPROPERTY_SET(
                &KSPROPSETID_Audio,
                SIZEOF_ARRAY(nodeProperties),
                nodeProperties,
                0,
                nullptr)
        },
        { // nodeAutomation
            DEFINE_KSAUTOMATION_PROPERTIES(nodePropertySets),
            DEFINE_KSAUTOMATION_METHODS_NULL,
            DEFINE_KSAUTOMATION_EVENTS_NULL
        },
        { // nodeDesc
            { // Node 0: ADC/DAC node
                &nodeAutomation,
                request->type == SAR_ENDPOINT_TYPE_RECORDING ? &KSNODETYPE_ADC : &KSNODETYPE_DAC,
                nullptr
            },
        },

        // Filter Descriptor
        //*********************
        { // filterDispatch
            SarKsFilterCreate, // Create
            SarKsFilterClose, // Close
            nullptr, // Process
            nullptr, // Reset
        },
        { // filterPinProperties
            DEFINE_KSPROPERTY_ITEM(
                KSPROPERTY_PIN_GLOBALCINSTANCES,
                SarKsPinGetGlobalInstancesCount,
                sizeof(KSP_PIN), sizeof(KSPIN_CINSTANCES),
                nullptr, nullptr, 0, nullptr, nullptr, 0),
            DEFINE_KSPROPERTY_ITEM(
                KSPROPERTY_PIN_PROPOSEDATAFORMAT,
                SarKsPinGetDefaultDataFormat,
                sizeof(KSP_PIN), 0,
                SarKsPinProposeDataFormat, nullptr, 0, nullptr, nullptr, 0),
                    
            DEFINE_KSPROPERTY_ITEM_PIN_PHYSICALCONNECTION(SarWaveFilterDescriptor::getPhysicalConnection)
        },
        { // filterPropertySets
            DEFINE_KSPROPERTY_SET(
                &KSPROPSETID_Pin,
                SIZEOF_ARRAY(filterPinProperties),
                filterPinProperties,
                0,
                nullptr)
        },
        { // filterEvents
            DEFINE_KSEVENT_ITEM(
                KSEVENT_PINCAPS_FORMATCHANGE,
                sizeof(KSEVENTDATA), 0,
                nullptr, nullptr, nullptr)
        },
        { // filterEventSets
            DEFINE_KSEVENT_SET(
                &KSEVENTSETID_PinCapsChange,
                SIZEOF_ARRAY(filterEvents),
                filterEvents)
        },
        { // filterAutomation
            DEFINE_KSAUTOMATION_PROPERTIES(filterPropertySets),
            DEFINE_KSAUTOMATION_METHODS_NULL,
            DEFINE_KSAUTOMATION_EVENTS(filterEventSets),
        },
        { // categoriesTableRender
            STATICGUIDOF(KSCATEGORY_AUDIO),
            STATICGUIDOF(KSCATEGORY_REALTIME),
            STATICGUIDOF(KSCATEGORY_RENDER),
        },
        { // categoriesTableCapture
            STATICGUIDOF(KSCATEGORY_AUDIO),
            STATICGUIDOF(KSCATEGORY_REALTIME),
            STATICGUIDOF(KSCATEGORY_CAPTURE),
        },
        { // filterConnectionsRender
            { KSFILTER_NODE, 0, 0, KSNODEPIN_STANDARD_IN },
            { 0, KSNODEPIN_STANDARD_OUT, KSFILTER_NODE, 1 }
        },
        { // filterConnectionsCapture
            { KSFILTER_NODE, 1, 0, KSNODEPIN_STANDARD_IN },
            { 0, KSNODEPIN_STANDARD_OUT, KSFILTER_NODE, 0 }
        },
        { // filterDesc
            &filterDispatch, // Dispatch
            &filterAutomation, // AutomationTable
            KSFILTER_DESCRIPTOR_VERSION, // Version
            0, // Flags
            nullptr, // ReferenceGuid
            SIZEOF_ARRAY(pinDesc), // PinDescriptorsCount
            sizeof(pinDesc[0]), // PinDescriptorSize
            pinDesc, // PinDescriptors
            request->type == SAR_ENDPOINT_TYPE_RECORDING ? SIZEOF_ARRAY(categoriesTableCapture) : SIZEOF_ARRAY(categoriesTableRender), // CategoriesCount
            request->type == SAR_ENDPOINT_TYPE_RECORDING ? categoriesTableCapture : categoriesTableRender, // Categories
            SIZEOF_ARRAY(nodeDesc), // NodeDescriptorsCount
            sizeof(nodeDesc[0]), // NodeDescriptorSize
            nodeDesc, // NodeDescriptors
            request->type == SAR_ENDPOINT_TYPE_RECORDING ? SIZEOF_ARRAY(filterConnectionsCapture) : SIZEOF_ARRAY(filterConnectionsRender), // ConnectionsCount
            request->type == SAR_ENDPOINT_TYPE_RECORDING ? filterConnectionsCapture : filterConnectionsRender, // Connections
            nullptr, // ComponentId
        },
    };

    *this = self;

    physicalConnectionSymlink.Buffer = physicalConnectionSymlink_buffer;
    physicalConnectionSymlink.Length = 0;
    physicalConnectionSymlink.MaximumLength = sizeof(physicalConnectionSymlink_buffer);

    return result;
}


NTSTATUS SarWaveFilterDescriptor::getPhysicalConnection(PIRP irp, PKSIDENTIFIER request, PVOID data)
{
    PKSP_PIN pinRequest = (PKSP_PIN)request;
    PKSPIN_PHYSICALCONNECTION pinData = (PKSPIN_PHYSICALCONNECTION)data;
    PUNICODE_STRING symlink;

    if (pinRequest->PinId != 1) {
        return STATUS_NOT_FOUND;
    }

    SarEndpoint *endpoint = SarGetEndpointFromIrp(irp, TRUE);

    if (!endpoint) {
        return STATUS_NOT_FOUND;
    }

    symlink = &endpoint->topologyDescriptor.physicalConnectionSymlink;

    PIO_STACK_LOCATION irpStack = IoGetCurrentIrpStackLocation(irp);
    ULONG outputLength =
        irpStack->Parameters.DeviceIoControl.OutputBufferLength;

    irp->IoStatus.Information = symlink->Length + sizeof(KSPIN_PHYSICALCONNECTION) + sizeof(UNICODE_NULL);

    if (outputLength == 0) {
        SarReleaseEndpointAndContext(endpoint);
        return STATUS_BUFFER_OVERFLOW;
    }

    if (outputLength < irp->IoStatus.Information) {
        SarReleaseEndpointAndContext(endpoint);
        return STATUS_BUFFER_TOO_SMALL;
    }

    pinData->Size = symlink->Length + sizeof(KSPIN_PHYSICALCONNECTION);
    RtlCopyMemory(pinData->SymbolicLinkName, symlink->Buffer, symlink->Length);
    pinData->SymbolicLinkName[symlink->Length / 2];
    pinData->Pin = 0;

    SarReleaseEndpointAndContext(endpoint);
    return STATUS_SUCCESS;
}
