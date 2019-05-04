#include "SarTopologyFilterDescriptor.h"
#include "sar.h"


NTSTATUS SarTopologyFilterDescriptor::initTopologyFilter(const SarCreateEndpointRequest * request) {
    NTSTATUS result = STATUS_SUCCESS;

    const SarTopologyFilterDescriptor self = {
        /* Pins Descriptors */
        /*********************/
        { // pinInterface
            (GUID)KSINTERFACESETID_Standard,
            (ULONG)KSINTERFACE_STANDARD_STREAMING,
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
            &analogDataRange
        },
        { // pinDesc
            { // Pin 0: digital pin
                nullptr, // Dispatch
                nullptr, // AutomationTable
                { // PinDescriptor
                    0u, // InterfacesCount
                    nullptr, // Interfaces
                    0u, // MediumsCount
                    nullptr, // Mediums
                    1u, // DataRangesCount
                    &dataRangePointers[0], // DataRanges
                    request->type == SAR_ENDPOINT_TYPE_RECORDING ? KSPIN_DATAFLOW_OUT : KSPIN_DATAFLOW_IN, // DataFlow
                    KSPIN_COMMUNICATION_NONE, // Communication
                    &KSCATEGORY_AUDIO, // Category
                    nullptr, // Name
                },
                0, // Flags
                0u, // InstancesPossible
                0u, // InstancesNecessary
                nullptr, // AllocatorFraming
                nullptr, // IntersectHandler
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
                    &dataRangePointers[0], // DataRanges
                    request->type == SAR_ENDPOINT_TYPE_RECORDING ? KSPIN_DATAFLOW_IN : KSPIN_DATAFLOW_OUT, // DataFlow
                    KSPIN_COMMUNICATION_NONE, // Communication
                    request->type == SAR_ENDPOINT_TYPE_RECORDING ? &KSNODETYPE_LINE_CONNECTOR : &KSNODETYPE_SPEAKER, // Category
                    nullptr, // Name
                },
                0, // Flags
                0u, // InstancesPossible
                0u, // InstancesNecessary
                nullptr, // AllocatorFraming
                nullptr, // IntersectHandler
            },
        },

        // Nodes Descriptors
        //*********************
        { // nodeDesc
            { // Node 0: Volume
                nullptr,
                &KSNODETYPE_VOLUME,
                nullptr
            },
            { // Node 0: Mute
                nullptr,
                &KSNODETYPE_MUTE,
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
                KSPROPERTY_PIN_NAME,
                SarKsPinGetName,
                sizeof(KSP_PIN), 0,
                SarKsPinGetName, nullptr, 0, nullptr, nullptr, 0),
            DEFINE_KSPROPERTY_ITEM_PIN_PHYSICALCONNECTION(SarTopologyFilterDescriptor::getPhysicalConnection)
        },
        { // filterPropertySets
            DEFINE_KSPROPERTY_SET(
                &KSPROPSETID_Pin,
                SIZEOF_ARRAY(filterPinProperties),
                filterPinProperties,
                0,
                nullptr)
        },
        { // filterAutomation
            DEFINE_KSAUTOMATION_PROPERTIES(filterPropertySets),
            DEFINE_KSAUTOMATION_METHODS_NULL,
            DEFINE_KSAUTOMATION_EVENTS_NULL,
        },
        { // categoriesTable
            STATICGUIDOF(KSCATEGORY_AUDIO),
            STATICGUIDOF(KSCATEGORY_TOPOLOGY),
        },
        { // filterConnectionsRender
            { KSFILTER_NODE, 0,                      0,             KSNODEPIN_STANDARD_IN },
            { 0,             KSNODEPIN_STANDARD_OUT, 1,             KSNODEPIN_STANDARD_IN },
            { 1,             KSNODEPIN_STANDARD_OUT, KSFILTER_NODE, 1 }
        },
        { // filterConnectionsCapture
            { KSFILTER_NODE, 1,                      0,             KSNODEPIN_STANDARD_IN },
            { 0,             KSNODEPIN_STANDARD_OUT, 1,             KSNODEPIN_STANDARD_IN },
            { 1,             KSNODEPIN_STANDARD_OUT, KSFILTER_NODE, 0 }
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
            SIZEOF_ARRAY(categoriesTable), // CategoriesCount
            categoriesTable, // Categories
            SIZEOF_ARRAY(nodeDesc), // NodeDescriptorsCount
            sizeof(nodeDesc[0]), // NodeDescriptorSize
            nodeDesc, // NodeDescriptors
            request->type == SAR_ENDPOINT_TYPE_RECORDING ? SIZEOF_ARRAY(filterConnectionsCapture) : SIZEOF_ARRAY(filterConnectionsRender), // ConnectionsCount
            request->type == SAR_ENDPOINT_TYPE_RECORDING ? filterConnectionsCapture : filterConnectionsRender, // Connections
            nullptr, // ComponentId
            // PhysicalConnection
        },
    };

    *this = self;

    physicalConnectionSymlink.Buffer = physicalConnectionSymlink_buffer;
    physicalConnectionSymlink.Length = 0;
    physicalConnectionSymlink.MaximumLength = sizeof(physicalConnectionSymlink_buffer);

    return result;
}

NTSTATUS SarTopologyFilterDescriptor::getPhysicalConnection(PIRP irp, PKSIDENTIFIER request, PVOID data)
{
    PKSP_PIN pinRequest = (PKSP_PIN)request;
    PKSPIN_PHYSICALCONNECTION pinData = (PKSPIN_PHYSICALCONNECTION)data;
    PUNICODE_STRING symlink;

    if (pinRequest->PinId != 0) {
        return STATUS_NOT_FOUND;
    }

    SarEndpoint *endpoint = SarGetEndpointFromIrp(irp, TRUE);

    if (!endpoint) {
        return STATUS_NOT_FOUND;
    }

    symlink = &endpoint->filterDescriptor.physicalConnectionSymlink;

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
    pinData->SymbolicLinkName[symlink->Length/2];
    pinData->Pin = 1;

    SarReleaseEndpointAndContext(endpoint);
    return STATUS_SUCCESS;
}
