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


#ifndef _SAR_TEST_CONFORMANCE_H
#define _SAR_TEST_CONFORMANCE_H

#include "common.h"

namespace SarTest {

// Drives the control device directly, without SarAsio, through every row of
// the request tables in driver/docs/behavior.md (appendix A.5): each refusal
// with its status, the byte counts Windows lets a caller see, the layout
// arithmetic of A.6, the register record of A.7 and the notification events
// of A.8, with a real shared-mode stream on a section whose audio area is
// not a multiple of 64 KiB.
//
// --legacy-quirks expects the behaviour section 13 lists as quirks (the
// driver of releases up to 0.13) where the driver in this repository
// deliberately differs.
int cmdConformance(const Args& args);

} // namespace SarTest
#endif // _SAR_TEST_CONFORMANCE_H
