#include "windows/devices.h"
#include "windows/diagnostics.h"
#include <audiopolicy.h>
#include <functiondiscoverykeys_devpkey.h>
#include <tlhelp32.h>
#include <wrl/client.h>
#include <algorithm>
#include <cwctype>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>

namespace partymic {
using Microsoft::WRL::ComPtr;
namespace {
std::wstring lower(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(), [](wchar_t c) { return std::towlower(c); });
    return value;
}
struct Process { std::wstring name; DWORD parent; };
std::map<DWORD, Process> processes() {
    std::map<DWORD, Process> result;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return result;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (Process32FirstW(snapshot, &entry)) do {
        result.emplace(entry.th32ProcessID, Process{entry.szExeFile, entry.th32ParentProcessID});
    } while (Process32NextW(snapshot, &entry));
    CloseHandle(snapshot);
    return result;
}
bool excluded(const std::wstring& name) {
    const auto n = lower(name);
    // Convenience filter only, not an ACE whitelist or a security guarantee.
    return n.find(L"league") != std::wstring::npos || n.find(L"riot") != std::wstring::npos ||
        n.find(L"anticheat") != std::wstring::npos || n.find(L"sguard") != std::wstring::npos ||
        n == L"wegame.exe" || n == L"partymic.exe";
}
}

void check(HRESULT hr, const char* operation) {
    if (FAILED(hr)) {
        std::ostringstream text;
        text << operation << " (HRESULT 0x" << std::hex << static_cast<unsigned long>(hr) << ")";
        diagnostics::write("api.error", text.str());
        throw std::runtime_error(text.str());
    }
}

std::wstring hresult_text(HRESULT hr) {
    wchar_t* message = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
        FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, static_cast<DWORD>(hr), 0,
        reinterpret_cast<wchar_t*>(&message), 0, nullptr);
    std::wstring result = message ? message : L"请查看 README 中的故障排查。";
    if (message) LocalFree(message);
    return result;
}

bool is_virtual_cable(const std::wstring& name) {
    // Callers restrict this to playback endpoints. Pack45 can expose its stereo
    // playback endpoint as localized "Speakers (VB-Audio Virtual Cable)".
    const auto n = lower(name);
    return (n.find(L"cable") != std::wstring::npos && n.find(L"input") != std::wstring::npos &&
        n.find(L"vb-audio") != std::wstring::npos) ||
        (n.ends_with(L"(vb-audio virtual cable)") && n.find(L"16") == std::wstring::npos &&
         n.find(L"output") == std::wstring::npos);
}

std::wstring default_endpoint_id(EDataFlow flow, ERole role) {
    ComPtr<IMMDeviceEnumerator> enumerator;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
        IID_PPV_ARGS(&enumerator)))) return {};
    ComPtr<IMMDevice> device;
    if (FAILED(enumerator->GetDefaultAudioEndpoint(flow, role, &device))) return {};
    LPWSTR id = nullptr;
    if (FAILED(device->GetId(&id))) return {};
    std::wstring result(id);
    CoTaskMemFree(id);
    return result;
}

std::vector<Endpoint> endpoints(EDataFlow flow) {
    ComPtr<IMMDeviceEnumerator> enumerator;
    check(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
        IID_PPV_ARGS(&enumerator)), "Create endpoint enumerator");
    ComPtr<IMMDeviceCollection> collection;
    check(enumerator->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, &collection), "List audio endpoints");
    UINT count = 0;
    check(collection->GetCount(&count), "Count audio endpoints");
    std::vector<Endpoint> result;
    for (UINT i = 0; i < count; ++i) {
        ComPtr<IMMDevice> device;
        ComPtr<IPropertyStore> properties;
        if (FAILED(collection->Item(i, &device)) ||
            FAILED(device->OpenPropertyStore(STGM_READ, &properties))) continue;
        PROPVARIANT name{};
        if (FAILED(properties->GetValue(PKEY_Device_FriendlyName, &name))) continue;
        LPWSTR id = nullptr;
        if (SUCCEEDED(device->GetId(&id))) {
            if (name.vt == VT_LPWSTR && name.pwszVal) result.push_back({id, name.pwszVal});
            CoTaskMemFree(id);
        }
        PropVariantClear(&name);
    }
    return result;
}

std::vector<Application> audio_applications() {
    const auto all = processes();
    std::vector<Application> result;
    std::set<DWORD> seen;
    ComPtr<IMMDeviceEnumerator> enumerator;
    check(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
        IID_PPV_ARGS(&enumerator)), "Create session enumerator");
    ComPtr<IMMDeviceCollection> devices;
    check(enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &devices), "List playback endpoints");
    UINT device_count = 0;
    check(devices->GetCount(&device_count), "Count playback endpoints");
    for (UINT d = 0; d < device_count; ++d) {
        ComPtr<IMMDevice> device;
        ComPtr<IAudioSessionManager2> manager;
        ComPtr<IAudioSessionEnumerator> sessions;
        if (FAILED(devices->Item(d, &device)) || FAILED(device->Activate(
            __uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr,
            reinterpret_cast<void**>(manager.GetAddressOf()))) ||
            FAILED(manager->GetSessionEnumerator(&sessions))) continue;
        int count = 0;
        if (FAILED(sessions->GetCount(&count))) continue;
        for (int i = 0; i < count; ++i) {
            ComPtr<IAudioSessionControl> control;
            ComPtr<IAudioSessionControl2> session;
            if (FAILED(sessions->GetSession(i, &control)) || FAILED(control.As(&session))) continue;
            DWORD pid = 0;
            AudioSessionState state{};
            if (FAILED(session->GetProcessId(&pid)) || !pid || pid == GetCurrentProcessId() ||
                FAILED(session->GetState(&state)) || state == AudioSessionStateExpired) continue;
            auto found = all.find(pid);
            if (found == all.end() || excluded(found->second.name)) continue;
            // Chromium/Electron players often render in a child with the same exe.
            // Ascend only identical executable names, never a generic launcher.
            std::set<DWORD> parents;
            while (parents.insert(pid).second) {
                auto parent = all.find(found->second.parent);
                if (parent == all.end() || lower(parent->second.name) != lower(found->second.name)) break;
                pid = parent->first;
                found = parent;
            }
            if (seen.insert(pid).second) {
                const auto& name = found->second.name;
                result.push_back({pid, name, name + L"  ·  PID " + std::to_wstring(pid)});
            }
        }
    }
    std::sort(result.begin(), result.end(), [](const auto& a, const auto& b) { return a.label < b.label; });
    return result;
}

bool process_matches(DWORD pid, const std::wstring& executable) {
    // The watcher needs one PID, not an allocating map of every process every
    // two seconds. A snapshot reads metadata only; no target handles are opened.
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) throw std::runtime_error("Cannot snapshot selected source metadata");
    PROCESSENTRY32W entry{};entry.dwSize = sizeof(entry);
    bool matches = false;
    if (Process32FirstW(snapshot, &entry)) do {
        if (entry.th32ProcessID == pid) {
            matches = !excluded(entry.szExeFile) && _wcsicmp(entry.szExeFile, executable.c_str()) == 0;
            break;
        }
    } while (Process32NextW(snapshot, &entry));
    CloseHandle(snapshot);return matches;
}
} // namespace partymic
