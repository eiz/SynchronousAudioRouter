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

#include "sar.h"

SarDriverExtension *SarGetDriverExtension(PDRIVER_OBJECT driverObject)
{
    return (SarDriverExtension *)IoGetDriverObjectExtension(
        driverObject, DriverEntry);
}

SarDriverExtension *SarGetDriverExtensionFromIrp(PIRP irp)
{
    PIO_STACK_LOCATION irpStack = IoGetCurrentIrpStackLocation(irp);

    return SarGetDriverExtension(irpStack->DeviceObject->DriverObject);
}

SarFileContext *SarGetFileContextFromFileObject(
    SarDriverExtension *extension, PFILE_OBJECT fileObject)
{
    SarFileContext *fileContext;
    SarFileContext fileContextTemplate;

    fileContextTemplate.fileObject = fileObject;
    ExAcquireFastMutex(&extension->fileContextLock);
    fileContext = (SarFileContext *)RtlLookupElementGenericTable(
        &extension->fileContextTable, (PVOID)&fileContextTemplate);
    ExReleaseFastMutex(&extension->fileContextLock);

    return fileContext;
}

// TODO: gotta go fast
SarEndpoint *SarGetEndpointFromIrp(PIRP irp)
{
    PKSFILTER filter = KsGetFilterFromIrp(irp);
    PKSFILTERFACTORY factory = KsFilterGetParentFilterFactory(filter);
    SarDriverExtension *extension = SarGetDriverExtensionFromIrp(irp);
    PVOID restartKey = nullptr;
    SarEndpoint *found = nullptr;

    ExAcquireFastMutex(&extension->fileContextLock);

    SAR_LOG("Looking for %p", factory);

    FOR_EACH_GENERIC(
        &extension->fileContextTable, SarFileContext, fileContext, restartKey) {
        ExAcquireFastMutex(&fileContext->mutex);

        PLIST_ENTRY entry = fileContext->endpointList.Flink;

        while (entry != &fileContext->endpointList) {
            SarEndpoint *endpoint =
                CONTAINING_RECORD(entry, SarEndpoint, listEntry);

            SAR_LOG("Checking %p", endpoint->filterFactory);
            if (endpoint->filterFactory == factory) {
                found = endpoint;
                break;
            }
        }

        ExReleaseFastMutex(&fileContext->mutex);

        if (found) {
            break;
        }
    }

    ExReleaseFastMutex(&extension->fileContextLock);
    return found;
}