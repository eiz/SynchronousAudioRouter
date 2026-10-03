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

#ifndef _SAR_TEST_WATCH_H
#define _SAR_TEST_WATCH_H

#include "common.h"

namespace SarTest {

// Logs, with timestamps, every change in which of the SAR device's KS
// interfaces are enabled and every endpoint state change Windows reports,
// until the duration passes or the stop file appears. For lining up what the
// driver and the audio endpoint builder did with what clients saw.
struct WatchOptions
{
    double durationSeconds = 60.0;
    std::wstring stopFile;
    int pollMs = 20;
    // How often to try opening every enabled SAR filter by its interface
    // path, logging each change in the result; 0 to not probe.
    int probeMs = 0;
};

// Returns the results JSON.
std::string runWatch(const WatchOptions& options);

} // namespace SarTest
#endif // _SAR_TEST_WATCH_H
