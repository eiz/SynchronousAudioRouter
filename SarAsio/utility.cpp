// SynchronousAudioRouter
// Copyright (C) 2015 Mackenzie Straight
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 2 of the License, or
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

std::string TCHARToLocal(const TCHAR *ptr)
{
    UINT codePage = GetACP();
    int len = WideCharToMultiByte(codePage, 0, ptr, -1, 0, 0, 0, 0);
    std::string out(len, '\0');
    WideCharToMultiByte(codePage, 0, ptr, -1, &out[0], out.size(), 0, 0);
    return out;
}

std::wstring LoggingPath()
{
    TCHAR path[MAX_PATH];

    if (SUCCEEDED(SHGetFolderPath(
        nullptr, CSIDL_APPDATA, nullptr, 0, path))) {

        PathAppend(path, TEXT("\\SynchronousAudioRouter\\"));
        CreateDirectory(path, nullptr);
        PathAppend(path, TEXT("\\logs\\"));
        CreateDirectory(path, nullptr);
    }

    return std::wstring(path);
}

std::wstring ConfigurationPath(const std::wstring& name)
{
    TCHAR path[MAX_PATH] = { 0 };

    if (SUCCEEDED(SHGetFolderPath(
        nullptr, CSIDL_APPDATA, nullptr, 0, path))) {

        PathAppend(path, TEXT("\\SynchronousAudioRouter\\"));
        CreateDirectory(path, nullptr);
        PathAppend(path, name.c_str());
    }

    return std::wstring(path);
}

static std::wstring getProductName(const std::wstring& wpath)
{
    auto verLength = GetFileVersionInfoSize(wpath.c_str(), nullptr);

    if (!verLength) {
        return L"";
    }

    auto buffer = malloc(verLength);

    if (!GetFileVersionInfo(wpath.c_str(), 0, verLength, buffer)) {
        free(buffer);
        return L"";
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
        return L"";
    }

    if (translationsLength < sizeof(LANGANDCODEPAGE)) {
        free(buffer);
        return L"";
    }

    // TODO: try current locale/codepage
    if (!VerQueryValue(buffer, neutralSubBlock, (void **)&name, &nameLength)) {
        _snwprintf_s(subBlockName, _TRUNCATE,
            L"\\StringFileInfo\\%04x%04x\\FileDescription",
            translations[0].language, translations[0].codePage);

        if (!VerQueryValue(buffer, subBlockName, (void **)&name, &nameLength)) {
            free(buffer);
            return L"";
        }
    }

    std::wstring result = name;

    free(buffer);
    return result;
}

std::vector<RunningApplication> RunningApplications()
{
    std::unordered_map<std::wstring, std::vector<std::wstring>> processMap;
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

                (*processMapPtr)[std::wstring(path)].emplace_back(
                    std::wstring(name));
            }

            CloseHandle(hprocess);
        }

        return TRUE;
    }, (LPARAM)&processMap);

    std::vector<RunningApplication> result;

    for (auto& kv : processMap) {
        RunningApplication app;
        std::wstring productName = getProductName(kv.first);

        app.path = kv.first;
        app.name = productName.size() > 0 ? productName : kv.second[0];
        result.push_back(app);
    }

    return result;
}

} // namespace Sar
