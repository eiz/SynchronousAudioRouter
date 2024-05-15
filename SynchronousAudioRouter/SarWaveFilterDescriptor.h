#pragma once

#include <ntifs.h>
#include <ntddk.h>
#include <ntstrsafe.h>
#include <windef.h>
#include <ks.h>
#include <ksmedia.h>

struct SarWaveFilterDescriptor
{
    NTSTATUS initWaveFilter(const struct SarControlContext *controlContext, const struct SarCreateEndpointRequest* request);
    static NTSTATUS getPhysicalConnection(PIRP irp, PKSIDENTIFIER request, PVOID data);


    KSPIN_DISPATCH pinDispatch;
    KSPROPERTY_ITEM pinRtAudioProperties[10];
    KSPROPERTY_SET pinPropertySets[1];
    KSEVENT_ITEM pinEvents[1];
    KSEVENT_SET pinEventSets[1];
    KSAUTOMATION_TABLE pinAutomation;
    KSPIN_INTERFACE pinInterface;
    KSDATARANGE_AUDIO digitalDataRange;
    KSDATARANGE analogDataRange;
    PKSDATARANGE dataRangePointers[2];
    KSPIN_DESCRIPTOR_EX pinDesc[2];

    KSPROPERTY_ITEM nodeProperties[1];
    KSPROPERTY_SET nodePropertySets[1];
    KSAUTOMATION_TABLE nodeAutomation;
    KSNODE_DESCRIPTOR nodeDesc[1];

    KSFILTER_DISPATCH filterDispatch;
    KSPROPERTY_ITEM filterPinProperties[3];
    KSPROPERTY_SET filterPropertySets[1];
    KSEVENT_ITEM filterEvents[1];
    KSEVENT_SET filterEventSets[1];
    KSAUTOMATION_TABLE filterAutomation;

    GUID categoriesTableRender[3];
    GUID categoriesTableCapture[3];
    KSTOPOLOGY_CONNECTION filterConnectionsRender[2];
    KSTOPOLOGY_CONNECTION filterConnectionsCapture[2];
    KSFILTER_DESCRIPTOR filterDesc;

    DECLARE_UNICODE_STRING_SIZE(physicalConnectionSymlink, 256);
};

