// SynchronousAudioRouter
// Copyright (C) 2017 Mackenzie Straight
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

#ifndef _SAR_ASIO_NETWORK_H
#define _SAR_ASIO_NETWORK_H

#include "config.h"
#include "sar.h"

namespace Sar {

#pragma pack(push, 1)
#pragma warning(disable: 4200) // don't warn on 0-length arrays

struct CastPacketHeader
{
    uint32_t length;
    uint32_t tag;
    uint8_t type;
};

struct CastStatusRequestPacket
{
    CastPacketHeader header;
};

struct CastNewEndpointPacket
{
    CastPacketHeader header;
    uint16_t index;
    uint8_t flags;
    uint8_t channels;
    uint32_t sampleRate;
    uint16_t bufferSampleCount;
};

struct CastTickPacket
{
    CastPacketHeader header;
    uint64_t offset;
};

struct CastBufferPacket
{
    CastPacketHeader header;
    uint64_t offset;
    uint16_t channel;
    uint8_t data[0];
};

struct CastAckPacket
{
    CastPacketHeader header;
};

#pragma pack(pop)

struct SarCastMaster: public std::enable_shared_from_this<SarCastMaster>
{
};

struct SarCastSlave: public std::enable_shared_from_this<SarCastSlave>
{
};

}

#endif // _SAR_ASIO_NETWORK_H
