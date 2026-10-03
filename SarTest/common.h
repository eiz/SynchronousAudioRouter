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

#ifndef _SAR_TEST_COMMON_H
#define _SAR_TEST_COMMON_H

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace SarTest {

// Command line: SarTest <command> [--key value | --key=value | --flag] ...
struct Args
{
    std::wstring command;
    std::map<std::wstring, std::wstring> options;

    bool has(const wchar_t *key) const;
    std::wstring get(const wchar_t *key, const wchar_t *def = L"") const;
    int getInt(const wchar_t *key, int def) const;
    double getDouble(const wchar_t *key, double def) const;
};

Args parseArgs(int argc, wchar_t **argv);

// Timestamped line on stdout, flushed immediately so the last line before a
// hang is always present in captured output.
void logf(const char *fmt, ...);

std::string narrow(const std::wstring& s);
std::wstring widen(const std::string& s);
std::string hresultText(HRESULT hr);
std::string lastErrorText(DWORD error);
double nowMs();

// Endpoint layout shared by every command: `pairs` playback/recording pairs
// with `channels` channels each. Pair p is "<prefix> Out p+1" (playback) and
// "<prefix> In p+1" (recording). SarAsio exposes playback endpoints as ASIO
// inputs and recording endpoints as ASIO outputs in configuration order, so
// copying ASIO input k to ASIO output k loops every "Out" channel back to the
// matching "In" channel.
struct EndpointLayout
{
    int pairs = 2;
    int channels = 2;
    std::wstring prefix = L"SarTest";

    std::wstring playbackName(int pair) const;
    std::wstring recordingName(int pair) const;
    std::string playbackId(int pair) const;
    std::string recordingId(int pair) const;
    int totalChannels() const { return pairs * channels; }
    int channelId(int pair, int channel) const
    {
        return (pair * channels + channel) & 0x3F;
    }

    static EndpointLayout fromArgs(const Args& args);
};

// Test signal. Each sample encodes (channel id, sequence number) as
// k = (id << 16) | (seq & 0xFFFF), carried as k / 2^23 in float, which stays
// under half scale, is exactly representable in float32, and survives the
// audio engine's float <-> int32 conversions. A capture stream can therefore
// verify which channel it receives and whether frames were dropped or
// repeated.
inline int32_t encodeSample(int channelId, uint32_t seq)
{
    return (int32_t)(((channelId & 0x3F) << 16) | (seq & 0xFFFF));
}

inline float encodeFloat(int channelId, uint32_t seq)
{
    return (float)encodeSample(channelId, seq) / 8388608.0f;
}

inline int32_t decodeFloat(float value)
{
    return (int32_t)lroundf(value * 8388608.0f);
}

inline int decodeChannel(int32_t k) { return (k >> 16) & 0x3F; }
inline uint32_t decodeSeq(int32_t k) { return (uint32_t)(k & 0xFFFF); }

// Minimal JSON emitter for the results file.
class JsonObject
{
public:
    JsonObject& setString(const std::string& key, const std::string& value);
    JsonObject& setInt(const std::string& key, long long value);
    JsonObject& setDouble(const std::string& key, double value);
    JsonObject& setBool(const std::string& key, bool value);
    JsonObject& setRaw(const std::string& key, const std::string& rawJson);
    std::string str() const;

private:
    std::vector<std::pair<std::string, std::string>> _members;
};

std::string jsonString(const std::string& value);
std::string jsonArray(const std::vector<std::string>& rawItems);
bool writeTextFile(const std::wstring& path, const std::string& content);

} // namespace SarTest
#endif // _SAR_TEST_COMMON_H
