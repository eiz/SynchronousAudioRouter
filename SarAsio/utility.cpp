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
#include "utility.h"

namespace Sar {

std::string TCHARToUTF8(const TCHAR *ptr)
{
    std::wstring_convert<std::codecvt_utf8<wchar_t>> converter;

    return converter.to_bytes(ptr);
}

std::wstring UTF8ToWide(const std::string& str)
{
    std::wstring_convert<std::codecvt_utf8<wchar_t>> converter;

    return converter.from_bytes(str);
}

std::string ConfigurationPath(const std::string& name)
{
    TCHAR path[MAX_PATH];

    if (SUCCEEDED(SHGetFolderPath(
        nullptr, CSIDL_APPDATA, nullptr, 0, path))) {

        PathAppend(path, TEXT("\\SynchronousAudioRouter\\"));
        CreateDirectory(path, nullptr);
        PathAppend(path, UTF8ToWide(name).c_str());
    }

    return TCHARToUTF8(path);
}

static std::string getProductName(const std::string& path)
{
    auto wpath = UTF8ToWide(path);
    auto verLength = GetFileVersionInfoSize(wpath.c_str(), nullptr);

    if (!verLength) {
        return "";
    }

    auto buffer = malloc(verLength);

    if (!GetFileVersionInfo(wpath.c_str(), 0, verLength, buffer)) {
        free(buffer);
        return "";
    }

    struct LANGANDCODEPAGE
    {
        WORD language;
        WORD codePage;
    } *translations;
    UINT translationsLength, nameLength;
    WCHAR subBlockName[50];
    WCHAR *neutralSubBlock = L"\\StringFileInfo\\00000000\\FileDescription";
    WCHAR *name;

    if (!VerQueryValue(buffer, L"\\VarFileInfo\\Translation",
        (void **)&translations, &translationsLength)) {

        free(buffer);
        return "";
    }

    if (translationsLength < sizeof(LANGANDCODEPAGE)) {
        free(buffer);
        return "";
    }

    // TODO: try current locale/codepage
    if (!VerQueryValue(buffer, neutralSubBlock, (void **)&name, &nameLength)) {
        _snwprintf_s(subBlockName, _TRUNCATE,
            L"\\StringFileInfo\\%04x%04x\\FileDescription",
            translations[0].language, translations[0].codePage);

        if (!VerQueryValue(buffer, subBlockName, (void **)&name, &nameLength)) {
            free(buffer);
            return "";
        }
    }

    std::string result = TCHARToUTF8(name);

    free(buffer);
    return result;
}

std::vector<RunningApplication> RunningApplications()
{
    std::unordered_map<std::string, std::vector<std::string>> processMap;
    using MapType = decltype(processMap);

    EnumWindows([](HWND hwnd, LPARAM lparam) {
        auto processMapPtr = (MapType *)lparam;
        auto owner = GetWindowOwner(hwnd);
        auto exstyle = GetWindowLong(hwnd, GWL_EXSTYLE);

        if (IsWindowVisible(hwnd) &&
            !GetParent(hwnd) &&
            (((exstyle & WS_EX_TOOLWINDOW) == 0 && !owner) ||
             ((exstyle & WS_EX_APPWINDOW) && owner))) {

            DWORD pid;

            GetWindowThreadProcessId(hwnd, &pid);

            HANDLE hprocess = OpenProcess(
                PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
            WCHAR path[MAX_PATH], name[MAX_PATH];

            if (!hprocess) {
                return TRUE;
            }

            if (GetModuleFileNameEx(hprocess, nullptr, path, MAX_PATH) &&
                GetWindowText(hwnd, name, MAX_PATH)) {

                (*processMapPtr)[TCHARToUTF8(path)].emplace_back(
                    TCHARToUTF8(name));
            }

            CloseHandle(hprocess);
        }

        return TRUE;
    }, (LPARAM)&processMap);

    std::vector<RunningApplication> result;

    for (auto& kv : processMap) {
        RunningApplication app;
        auto productName = getProductName(kv.first);

        app.path = kv.first;
        app.name = productName.size() > 0 ? productName : kv.second[0];
        result.push_back(app);
    }

    return result;
}

} // namespace Sar
