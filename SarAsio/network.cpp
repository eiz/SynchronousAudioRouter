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

#include "stdafx.h"
#include "network.h"

// todo list / thoughts
// - winsock initialization
// - rio initialization
// - message send routines
// - message receive loops
// - master and slave state machines
// - delegate interface
// - threading model: dedicated thread for receivers, sends from any thread
// - SarClient integration
//     - Not sure if it's actually the correct place to do this. SarClient is
//       working on muxed WaveRT buffers which don't really need to exist for
//       Cast -- the slave could run its own SarClient and send the demuxed
//       buffers directly. In that case, SarCastMaster just needs to receive the
//       data and copy it to the correct ASIO framebuffer, bypassing its own
//       SarClient entirely.
// - Remote recording endpoints
//     - In this case, the master needs to send ASIO buffers to the slaves and
//       the slave must copy those frames into the appropriate buffers for its
//       own SarClient. Ordering may be tricky here -- the slave needs to
//       receive all of the buffers prior to the tick event. This isn't
//       guaranteed by the network stack, so SarCastSlave may need to keep track
//       of which buffers with an offset == to a given tick it has received, and
//       defer execution of that tick until all necessary buffers have been
//       received.
//     - I'm not sure how relevant this capability is to my own use cases, but
//       it seems pretty cool and I want it to work right just because there's
//       no reason it shouldn't.
// - Remote playback endpoints
//     - My current assumption here is that the tick packet is going to go out
//       synchronous to the ASIO tick event, which means remote endpoints are
//       going to run 1 frame behind the master? Alternatively, we could defer
//       execution of the work that happens in the ASIO tick until after we
//       receive responses, but on normal 1gbps Ethernet, this most likely is
//       not going to perform fast enough due to minimum round-trip latency.
//       It's worth experimenting on tho.
//       TODO: pick up some 10gbit cards and mess with those, too.
// - Overall goals
//     - Stable network audio over 1 switch hop (using a Mikrotik CRS125 as a
//       test target), both send and receive, with 64 samples @ 96KHz (0.6ms).
//     - Maximum of 1 extra buffer of latency.
//     - Support Windows 8+ (RIO required).
