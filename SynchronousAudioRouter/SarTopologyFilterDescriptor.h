#pragma once

#include <ntifs.h>
#include <ntddk.h>
#include <ntstrsafe.h>
#include <windef.h>
#include <ks.h>
#include <ksmedia.h>

struct SarTopologyFilterDescriptor
{
    NTSTATUS initTopologyFilter(const struct SarCreateEndpointRequest* request);
    static NTSTATUS getPhysicalConnection(PIRP irp, PKSIDENTIFIER request, PVOID data);

    KSPIN_INTERFACE pinInterface;
    KSDATARANGE analogDataRange;
    PKSDATARANGE dataRangePointers[1];
    KSPIN_DESCRIPTOR_EX pinDesc[2];

    KSNODE_DESCRIPTOR nodeDesc[2];

    KSFILTER_DISPATCH filterDispatch;
    KSPROPERTY_ITEM filterPinProperties[2];
    KSPROPERTY_SET filterPropertySets[1];
    KSAUTOMATION_TABLE filterAutomation;

    GUID categoriesTable[2];
    KSTOPOLOGY_CONNECTION filterConnectionsRender[3];
    KSTOPOLOGY_CONNECTION filterConnectionsCapture[3];
    KSFILTER_DESCRIPTOR filterDesc;


    DECLARE_UNICODE_STRING_SIZE(physicalConnectionSymlink, 256);
};