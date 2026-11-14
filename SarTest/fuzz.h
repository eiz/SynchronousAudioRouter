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


#ifndef _SAR_TEST_FUZZ_H
#define _SAR_TEST_FUZZ_H

#include "common.h"

namespace SarTest {

// Throws malformed and badly ordered requests at the driver from several
// threads at once: control requests with random codes, lengths, contents and
// buffer placements on handles in every state, and kernel streaming
// property, event, stream and pin-creation requests on the filters of
// endpoints that keep coming and going underneath. Nothing it sends is meant
// to succeed; what is required is that the machine survives, that no request
// is left hanging, and that the driver still works afterwards (which the
// scenarios that follow it check).
int cmdFuzz(const Args& args);

} // namespace SarTest
#endif // _SAR_TEST_FUZZ_H
