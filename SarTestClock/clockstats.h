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

#ifndef _SAR_TEST_CLOCK_STATS_H
#define _SAR_TEST_CLOCK_STATS_H

#include <cstdint>

// Shared between SarTestClock.dll and SarTest.exe.
namespace SarTestClock {

// CLSID and ASIO registry name of the software clock driver. The string form
// must match what DllRegisterServer writes, because SarAsio compares it
// textually against the driverClsid in its configuration.
#define SAR_TEST_CLOCK_CLSID_STR L"{7A3C9E12-4B5D-4F6E-8A1B-2C3D4E5F6A7B}"
#define SAR_TEST_CLOCK_NAME L"SAR Test Clock"

// IASIO::future selector that fills a ClockStats. SarAsio forwards unknown
// selectors to its inner driver, so a host reaches the clock through it.
const long kStatsSelector = 0x53415254; // 'SART'

struct ClockStats
{
    uint32_t size;          // sizeof(ClockStats), set by the caller
    uint32_t running;
    uint64_t ticks;
    uint64_t lateTicks;     // ticks delivered more than one period late
    double maxLatenessMs;
    double periodMs;
    double sampleRate;
    uint32_t bufferFrames;
    uint32_t reserved;
};

} // namespace SarTestClock
#endif // _SAR_TEST_CLOCK_STATS_H
