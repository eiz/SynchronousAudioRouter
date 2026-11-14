// SynchronousAudioRouter
// Copyright (C) 2026 Mackenzie Straight
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

#ifndef _SAR_TEST_CONTENTION_H
#define _SAR_TEST_CONTENTION_H

#include "common.h"

namespace SarTest {

// Measures how long one host's housekeeping holds up another host's clients.
// A bystander host with one playback and one recording endpoint stays up
// while a second host creates and closes many endpoints, round after round;
// meanwhile a thread opens and closes the bystander's playback filter, as an
// audio application's first step to a stream, and times every open and
// close. Kernel streaming serializes filter opens across the whole device,
// so whatever the driver does under that lock for one host shows up here as
// a wait for the other.
int cmdContention(const Args& args);

} // namespace SarTest
#endif // _SAR_TEST_CONTENTION_H
