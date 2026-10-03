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

#include "common.h"

#include <cstdarg>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <fstream>

namespace SarTest {

bool Args::has(const wchar_t *key) const
{
    return options.find(key) != options.end();
}

std::wstring Args::get(const wchar_t *key, const wchar_t *def) const
{
    auto it = options.find(key);
    return it == options.end() ? std::wstring(def) : it->second;
}

int Args::getInt(const wchar_t *key, int def) const
{
    auto it = options.find(key);

    if (it == options.end() || it->second.empty()) {
        return def;
    }

    return _wtoi(it->second.c_str());
}

double Args::getDouble(const wchar_t *key, double def) const
{
    auto it = options.find(key);

    if (it == options.end() || it->second.empty()) {
        return def;
    }

    return _wtof(it->second.c_str());
}

Args parseArgs(int argc, wchar_t **argv)
{
    Args args;
    int i = 1;

    if (i < argc && wcsncmp(argv[i], L"--", 2) != 0) {
        args.command = argv[i++];
    }

    while (i < argc) {
        std::wstring token = argv[i++];

        if (token.size() < 3 || token.compare(0, 2, L"--") != 0) {
            logf("Ignoring unexpected argument: %s", narrow(token).c_str());
            continue;
        }

        std::wstring key = token.substr(2);
        std::wstring value;
        auto eq = key.find(L'=');

        if (eq != std::wstring::npos) {
            value = key.substr(eq + 1);
            key = key.substr(0, eq);
        } else if (i < argc && wcsncmp(argv[i], L"--", 2) != 0) {
            value = argv[i++];
        }

        args.options[key] = value;
    }

    return args;
}

void logf(const char *fmt, ...)
{
    SYSTEMTIME st;
    char buffer[2048];
    va_list ap;

    GetLocalTime(&st);
    va_start(ap, fmt);
    vsnprintf(buffer, sizeof(buffer), fmt, ap);
    va_end(ap);
    buffer[sizeof(buffer) - 1] = '\0';
    printf("[%02d:%02d:%02d.%03d] %s\n",
        st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, buffer);
    fflush(stdout);
}

std::string narrow(const std::wstring& s)
{
    if (s.empty()) {
        return std::string();
    }

    int len = WideCharToMultiByte(
        CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0, nullptr, nullptr);

    if (len <= 0) {
        return std::string();
    }

    std::string out((size_t)len, '\0');
    WideCharToMultiByte(
        CP_UTF8, 0, s.c_str(), (int)s.size(), &out[0], len, nullptr, nullptr);
    return out;
}

std::wstring widen(const std::string& s)
{
    if (s.empty()) {
        return std::wstring();
    }

    int len = MultiByteToWideChar(
        CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);

    if (len <= 0) {
        return std::wstring();
    }

    std::wstring out((size_t)len, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &out[0], len);
    return out;
}

std::string hresultText(HRESULT hr)
{
    char buf[32];

    sprintf_s(buf, "0x%08X", (unsigned int)hr);
    return buf;
}

std::string lastErrorText(DWORD error)
{
    char *message = nullptr;
    DWORD len = FormatMessageA(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
        FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, error, 0, (LPSTR)&message, 0, nullptr);
    std::string result;

    if (len && message) {
        result.assign(message, len);

        while (!result.empty() &&
               (result.back() == '\r' || result.back() == '\n' ||
                result.back() == ' ')) {
            result.pop_back();
        }
    }

    if (message) {
        LocalFree(message);
    }

    char code[32];
    sprintf_s(code, " (error %lu)", (unsigned long)error);
    return result + code;
}

double nowMs()
{
    LARGE_INTEGER frequency, counter;

    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&counter);
    return (double)counter.QuadPart * 1000.0 / (double)frequency.QuadPart;
}

std::wstring EndpointLayout::playbackName(int pair) const
{
    return prefix + L" Out " + std::to_wstring(pair + 1);
}

std::wstring EndpointLayout::recordingName(int pair) const
{
    return prefix + L" In " + std::to_wstring(pair + 1);
}

std::string EndpointLayout::playbackId(int pair) const
{
    return narrow(prefix) + "-out-" + std::to_string(pair + 1);
}

std::string EndpointLayout::recordingId(int pair) const
{
    return narrow(prefix) + "-in-" + std::to_string(pair + 1);
}

EndpointLayout EndpointLayout::fromArgs(const Args& args)
{
    EndpointLayout layout;

    layout.pairs = args.getInt(L"endpoints", 2);
    layout.channels = args.getInt(L"channels", 2);
    layout.prefix = args.get(L"prefix", L"SarTest");

    if (layout.pairs < 1) {
        layout.pairs = 1;
    }

    if (layout.pairs > 64) {
        layout.pairs = 64;
    }

    if (layout.channels < 1) {
        layout.channels = 1;
    }

    if (layout.channels > 32) {
        layout.channels = 32;
    }

    return layout;
}

std::string jsonString(const std::string& value)
{
    std::string out = "\"";

    for (unsigned char c : value) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20) {
                char buf[8];
                sprintf_s(buf, "\\u%04x", (unsigned int)c);
                out += buf;
            } else {
                out += (char)c;
            }
        }
    }

    out += "\"";
    return out;
}

std::string jsonArray(const std::vector<std::string>& rawItems)
{
    std::string out = "[";

    for (size_t i = 0; i < rawItems.size(); ++i) {
        if (i) {
            out += ", ";
        }

        out += rawItems[i];
    }

    out += "]";
    return out;
}

JsonObject& JsonObject::setString(const std::string& key, const std::string& value)
{
    _members.emplace_back(key, jsonString(value));
    return *this;
}

JsonObject& JsonObject::setInt(const std::string& key, long long value)
{
    _members.emplace_back(key, std::to_string(value));
    return *this;
}

JsonObject& JsonObject::setDouble(const std::string& key, double value)
{
    char buf[64];

    if (std::isfinite(value)) {
        sprintf_s(buf, "%.3f", value);
    } else {
        sprintf_s(buf, "null");
    }

    _members.emplace_back(key, buf);
    return *this;
}

JsonObject& JsonObject::setBool(const std::string& key, bool value)
{
    _members.emplace_back(key, value ? "true" : "false");
    return *this;
}

JsonObject& JsonObject::setRaw(const std::string& key, const std::string& rawJson)
{
    _members.emplace_back(key, rawJson);
    return *this;
}

std::string JsonObject::str() const
{
    std::string out = "{";

    for (size_t i = 0; i < _members.size(); ++i) {
        if (i) {
            out += ", ";
        }

        out += jsonString(_members[i].first);
        out += ": ";
        out += _members[i].second;
    }

    out += "}";
    return out;
}

bool writeTextFile(const std::wstring& path, const std::string& content)
{
    std::ofstream fp(path, std::ios::binary | std::ios::trunc);

    if (!fp) {
        logf("Couldn't write %s", narrow(path).c_str());
        return false;
    }

    fp << content;
    return true;
}

} // namespace SarTest
