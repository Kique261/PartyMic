#pragma once
#include <windows.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>
#include <string>
#include <vector>

namespace partymic {
struct Endpoint { std::wstring id, name; };
struct Application { DWORD pid; std::wstring executable, label; };
std::vector<Endpoint> endpoints(EDataFlow flow);
std::wstring default_endpoint_id(EDataFlow flow, ERole role);
Microsoft::WRL::ComPtr<IMMDevice> device_by_id(const std::wstring& id, EDataFlow required_flow);
std::vector<Application> audio_applications();
bool is_virtual_cable(const std::wstring& name);
// Read process metadata using Toolhelp only. Never opens target process handles.
bool process_matches(DWORD pid, const std::wstring& executable);
std::wstring hresult_text(HRESULT hr);
void check(HRESULT hr, const char* operation);
} // namespace partymic
