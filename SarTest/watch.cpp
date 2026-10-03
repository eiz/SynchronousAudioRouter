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

#include "watch.h"
#include "install.h"

#include <cfgmgr32.h>
#include <mmdeviceapi.h>
#include <functiondiscoverykeys_devpkey.h>
#include <mutex>
#include <set>

namespace SarTest {

namespace {

// KSCATEGORY_AUDIO: every SAR wave and topology filter is registered in it.
const GUID kKsCategoryAudio =
    { 0x6994AD04, 0x93EF, 0x11D0, { 0xA3, 0xCC, 0x00, 0xA0, 0xC9, 0x22, 0x31, 0x96 } };

const char *stateName(DWORD state)
{
    switch (state) {
    case DEVICE_STATE_ACTIVE: return "active";
    case DEVICE_STATE_DISABLED: return "disabled";
    case DEVICE_STATE_NOTPRESENT: return "notpresent";
    case DEVICE_STATE_UNPLUGGED: return "unplugged";
    default: return "unknown";
    }
}

// The reference string at the end of an interface path, which for SAR is
// "<endpoint id>_<channels>_<rate>_<sample size>[_topology]".
std::string referenceString(const std::wstring& path)
{
    auto slash = path.find_last_of(L'\\');

    return narrow(slash == std::wstring::npos ? path : path.substr(slash + 1));
}

// Reference string -> interface path of every enabled SAR KS interface.
std::map<std::string, std::wstring> enabledInterfacePaths(const std::wstring& instanceId)
{
    std::map<std::string, std::wstring> result;
    ULONG length = 0;

    if (CM_Get_Device_Interface_List_SizeW(&length, (LPGUID)&kKsCategoryAudio,
            (DEVINSTID_W)instanceId.c_str(),
            CM_GET_DEVICE_INTERFACE_LIST_PRESENT) != CR_SUCCESS || length == 0) {
        return result;
    }

    std::vector<wchar_t> buffer(length + 1, L'\0');

    if (CM_Get_Device_Interface_ListW((LPGUID)&kKsCategoryAudio,
            (DEVINSTID_W)instanceId.c_str(), buffer.data(), length,
            CM_GET_DEVICE_INTERFACE_LIST_PRESENT) != CR_SUCCESS) {
        return result;
    }

    for (const wchar_t *p = buffer.data(); *p; p += wcslen(p) + 1) {
        result[referenceString(p)] = p;
    }

    return result;
}

std::set<std::string> enabledInterfaces(const std::wstring& instanceId)
{
    std::set<std::string> result;

    for (const auto& pair : enabledInterfacePaths(instanceId)) {
        result.insert(pair.first);
    }

    return result;
}

// CPU time used so far by the process hosting a service (its own svchost on
// machines with enough memory), so a busy endpoint builder shows up even when
// it changes nothing anyone can observe.
class ServiceCpu
{
public:
    explicit ServiceCpu(const wchar_t *service)
    {
        SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
        SC_HANDLE svc = scm ? OpenServiceW(scm, service, SERVICE_QUERY_STATUS) : nullptr;
        SERVICE_STATUS_PROCESS status = {};
        DWORD needed = 0;

        if (svc && QueryServiceStatusEx(svc, SC_STATUS_PROCESS_INFO,
                (LPBYTE)&status, sizeof(status), &needed)) {
            _process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
                status.dwProcessId);
        }

        if (svc) {
            CloseServiceHandle(svc);
        }

        if (scm) {
            CloseServiceHandle(scm);
        }

        _last = totalMs();
    }

    ~ServiceCpu()
    {
        if (_process) {
            CloseHandle(_process);
        }
    }

    // Milliseconds of CPU used since the previous call.
    double delta()
    {
        double now = totalMs();
        double result = now - _last;

        _last = now;
        return result;
    }

private:
    double totalMs()
    {
        FILETIME created, exited, kernel, user;

        if (!_process || !GetProcessTimes(_process, &created, &exited, &kernel, &user)) {
            return 0.0;
        }

        auto ms = [](const FILETIME& ft) {
            return (((ULONGLONG)ft.dwHighDateTime << 32) | ft.dwLowDateTime) / 10000.0;
        };

        return ms(kernel) + ms(user);
    }

    HANDLE _process = nullptr;
    double _last = 0.0;
};

// Opens a KS filter instance the way a client does, and closes it again.
DWORD probeFilter(const std::wstring& path)
{
    HANDLE handle = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE,
        0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);

    if (handle == INVALID_HANDLE_VALUE) {
        return GetLastError();
    }

    CloseHandle(handle);
    return ERROR_SUCCESS;
}

struct WatchEvent
{
    double ms;
    std::string kind;
    std::string subject;
    std::string detail;
};

class EndpointWatcher: public IMMNotificationClient
{
public:
    EndpointWatcher(double originMs, IMMDeviceEnumerator *enumerator)
        : _originMs(originMs), _enumerator(enumerator)
    {
        IMMDeviceCollection *devices = nullptr;

        // Names of every endpoint Windows knows, present or not, so events
        // can be logged by name.
        if (SUCCEEDED(_enumerator->EnumAudioEndpoints(
                eAll, DEVICE_STATEMASK_ALL, &devices))) {
            UINT count = 0;

            devices->GetCount(&count);

            for (UINT i = 0; i < count; ++i) {
                IMMDevice *device = nullptr;

                if (SUCCEEDED(devices->Item(i, &device))) {
                    LPWSTR id = nullptr;

                    if (SUCCEEDED(device->GetId(&id))) {
                        _names[id] = deviceName(device);
                        CoTaskMemFree(id);
                    }

                    device->Release();
                }
            }

            devices->Release();
        }
    }

    ULONG STDMETHODCALLTYPE AddRef() override { return 1; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppv) override
    {
        if (riid == IID_IUnknown || riid == __uuidof(IMMNotificationClient)) {
            *ppv = static_cast<IMMNotificationClient *>(this);
            return S_OK;
        }

        *ppv = nullptr;
        return E_NOINTERFACE;
    }

    HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR id, DWORD state) override
    {
        record("endpoint", id, stateName(state));
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR id) override
    {
        record("endpoint", id, "added");
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR id) override
    {
        record("endpoint", id, "removed");
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow, ERole, LPCWSTR) override
    {
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) override
    {
        return S_OK;
    }

    void addEvent(const WatchEvent& event)
    {
        std::lock_guard<std::mutex> lock(_mutex);

        _events.push_back(event);
    }

    std::vector<WatchEvent> events()
    {
        std::lock_guard<std::mutex> lock(_mutex);

        return _events;
    }

private:
    static std::string deviceName(IMMDevice *device)
    {
        IPropertyStore *store = nullptr;
        std::string name;

        if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &store))) {
            PROPVARIANT value;

            PropVariantInit(&value);

            if (SUCCEEDED(store->GetValue(PKEY_Device_DeviceDesc, &value)) &&
                value.vt == VT_LPWSTR && value.pwszVal) {
                name = narrow(value.pwszVal);
            }

            PropVariantClear(&value);
            store->Release();
        }

        return name;
    }

    void record(const char *kind, LPCWSTR id, const char *detail)
    {
        std::string subject;

        {
            std::lock_guard<std::mutex> lock(_mutex);
            auto it = _names.find(id);

            subject = it != _names.end() && !it->second.empty() ?
                it->second : narrow(id);
        }

        logf("%s %s: %s", kind, subject.c_str(), detail);
        addEvent({ nowMs() - _originMs, kind, subject, detail });
    }

    double _originMs;
    IMMDeviceEnumerator *_enumerator;
    std::mutex _mutex;
    std::map<std::wstring, std::string> _names;
    std::vector<WatchEvent> _events;
};

} // namespace

std::string runWatch(const WatchOptions& options)
{
    double originMs = nowMs();
    std::wstring instanceId = sarDeviceInstanceId();
    IMMDeviceEnumerator *enumerator = nullptr;

    if (instanceId.empty()) {
        logf("No SAR device node found; watching endpoints only");
    } else {
        logf("Watching KS interfaces of %s", narrow(instanceId).c_str());
    }

    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
        CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void **)&enumerator);

    if (FAILED(hr)) {
        logf("CoCreateInstance(MMDeviceEnumerator) failed: %s", hresultText(hr).c_str());
        return "{\"error\": \"no device enumerator\"}";
    }

    EndpointWatcher watcher(originMs, enumerator);

    enumerator->RegisterEndpointNotificationCallback(&watcher);

    std::set<std::string> enabled = enabledInterfaces(instanceId);

    logf("%zu SAR interfaces enabled at start", enabled.size());

    double deadline = originMs + options.durationSeconds * 1000.0;
    double nextProbe = originMs;
    double nextCpu = originMs + 1000.0;
    ServiceCpu builderCpu(L"AudioEndpointBuilder");
    ServiceCpu audiosrvCpu(L"Audiosrv");
    std::map<std::string, DWORD> lastProbe;
    long long probes = 0, probeFailures = 0;

    while (nowMs() < deadline) {
        if (!options.stopFile.empty() &&
            GetFileAttributesW(options.stopFile.c_str()) != INVALID_FILE_ATTRIBUTES) {
            logf("Stop file found");
            break;
        }

        std::set<std::string> now = enabledInterfaces(instanceId);

        for (const auto& ref : now) {
            if (!enabled.count(ref)) {
                logf("interface %s: enabled", ref.c_str());
                watcher.addEvent({ nowMs() - originMs, "interface", ref, "enabled" });
            }
        }

        for (const auto& ref : enabled) {
            if (!now.count(ref)) {
                logf("interface %s: disabled", ref.c_str());
                watcher.addEvent({ nowMs() - originMs, "interface", ref, "disabled" });
            }
        }

        enabled.swap(now);

        if (nowMs() >= nextCpu) {
            double builder = builderCpu.delta();
            double audiosrv = audiosrvCpu.delta();

            nextCpu += 1000.0;

            if (builder >= 50.0 || audiosrv >= 50.0) {
                char detail[96];

                sprintf_s(detail, "builder %.0f ms, audiosrv %.0f ms", builder, audiosrv);
                logf("cpu %s", detail);
                watcher.addEvent({ nowMs() - originMs, "cpu", "services", detail });
            }
        }

        if (options.probeMs > 0 && nowMs() >= nextProbe) {
            nextProbe = nowMs() + options.probeMs;

            for (const auto& pair : enabledInterfacePaths(instanceId)) {
                double t0 = nowMs();
                DWORD result = probeFilter(pair.second);
                double took = nowMs() - t0;
                auto it = lastProbe.find(pair.first);

                probes++;
                probeFailures += result != ERROR_SUCCESS;

                if (it == lastProbe.end() || it->second != result || took > 500) {
                    char detail[96];

                    sprintf_s(detail, "%s (%lu) in %.0f ms",
                        result == ERROR_SUCCESS ? "opened" : "open failed",
                        result, took);
                    logf("probe %s: %s", pair.first.c_str(), detail);
                    watcher.addEvent({ t0 - originMs, "probe", pair.first, detail });
                }

                lastProbe[pair.first] = result;
            }
        }

        Sleep((DWORD)options.pollMs);
    }

    enumerator->UnregisterEndpointNotificationCallback(&watcher);
    enumerator->Release();

    std::vector<std::string> items;

    for (const auto& event : watcher.events()) {
        JsonObject item;

        item.setDouble("ms", event.ms)
            .setString("kind", event.kind)
            .setString("subject", event.subject)
            .setString("detail", event.detail);
        items.push_back(item.str());
    }

    JsonObject result;

    result.setString("command", "watch")
        .setBool("passed", true)
        .setInt("exitCode", 0)
        .setString("sarDevice", narrow(instanceId))
        .setInt("events", (long long)items.size())
        .setInt("probes", probes)
        .setInt("probeFailures", probeFailures)
        .setRaw("timeline", jsonArray(items));
    return result.str();
}

} // namespace SarTest
