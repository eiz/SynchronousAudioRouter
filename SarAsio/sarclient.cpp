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

#include "stdafx.h"
#include "sarclient.h"

namespace Sar {

SarClient::SarClient(
    const DriverConfig& driverConfig,
    const BufferConfig& bufferConfig)
    : _driverConfig(driverConfig), _bufferConfig(bufferConfig)
{
}

void SarClient::tick(long bufferIndex)
{
    // for each endpoint
    // read isActive, generation and buffer offset/size
    //   if offset/size invalid, skip endpoint (fill asio buffers with 0)
    // if playback device:
    //   consume frameSampleCount * channelCount samples, demux to asio frames
    // if recording device:
    //   mux from asio frames
    // read isActive, generation
    //   if conflicted, skip endpoint and fill asio frames with 0
    // else increment position register
}

bool SarClient::start()
{
    return true;
}

void SarClient::stop()
{
}

} // namespace Sar