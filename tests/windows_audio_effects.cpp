// Opt-in capability probe. Initializes two private capture clients but never
// starts them, saves audio, or changes any device/global effect settings.
#include "windows/devices.h"
#include <audioclient.h>
#include <wrl/client.h>
#include <fcntl.h>
#include <io.h>
#include <iomanip>
#include <iostream>

using namespace partymic;
using Microsoft::WRL::ComPtr;
namespace {
// Minimal ABI from Microsoft's win32metadata Audioclient.h (2026-10-18).
// Distinct names avoid conflicts with native SDKs that already declare them.
struct Effect {
    GUID id;
    BOOL can_set;
    int state;
};
struct EffectsManager : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE Register(IUnknown *) = 0;
    virtual HRESULT STDMETHODCALLTYPE Unregister(IUnknown *) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetEffects(Effect **, UINT32 *) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetState(GUID, int) = 0;
};
constexpr GUID manager_iid{0x4460b3ae, 0x4b44, 0x4527, {0x86, 0x76, 0x75, 0x48, 0xa8, 0xac, 0xd2, 0x60}};
constexpr GUID aec_control_iid{0xf4ae25b5, 0xaaa3, 0x437d, {0xb6, 0xb3, 0xdb, 0xbe, 0x2d, 0x0e, 0x95, 0x49}};
constexpr GUID aec_effect{0x6f64adbe, 0x8211, 0x11e2, {0x8c, 0x70, 0x2c, 0x27, 0xd7, 0xf0, 0x01, 0xfa}};
void probe(IMMDevice *device, bool communications) {
    ComPtr<IAudioClient> client;
    check(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                           reinterpret_cast<void **>(client.GetAddressOf())),
          "Activate capture");
    ComPtr<IAudioClient2> properties;
    check(client.As(&properties), "Capture properties");
    AudioClientProperties options{};
    options.cbSize = sizeof(options);
    options.eCategory = communications ? AudioCategory_Communications : AudioCategory_Other;
    check(properties->SetClientProperties(&options), "Set private stream category");
    WAVEFORMATEX format{};
    format.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
    format.nChannels = 2;
    format.nSamplesPerSec = 48000;
    format.wBitsPerSample = 32;
    format.nBlockAlign = 8;
    format.nAvgBytesPerSec = 384000;
    check(client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                             AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                             1000000, 0, &format, nullptr),
          "Initialize private capture");
    ComPtr<IUnknown> aec;
    const HRESULT aec_hr = client->GetService(aec_control_iid, reinterpret_cast<void **>(aec.GetAddressOf()));
    ComPtr<EffectsManager> manager;
    const HRESULT effects_hr =
        client->GetService(manager_iid, reinterpret_cast<void **>(manager.GetAddressOf()));
    std::wcout << L"CATEGORY=" << (communications ? L"communications" : L"other")
               << L" aec_control_hresult=0x" << std::hex << unsigned(aec_hr) << L" effects_manager_hresult=0x"
               << unsigned(effects_hr) << std::dec << std::endl;
    if (SUCCEEDED(effects_hr)) {
        Effect *effects = nullptr;
        UINT32 count = 0;
        check(manager->GetEffects(&effects, &count), "Enumerate stream effects");
        bool has_aec = false;
        for (UINT32 i = 0; i < count; ++i) {
            wchar_t id[40]{};
            StringFromGUID2(effects[i].id, id, 40);
            const bool is_aec = IsEqualGUID(effects[i].id, aec_effect);
            has_aec |= is_aec;
            std::wcout << L"EFFECT id=" << id << L" can_set=" << effects[i].can_set << L" state="
                       << effects[i].state << L" is_aec=" << is_aec << std::endl;
        }
        CoTaskMemFree(effects);
        std::wcout << L"EFFECT_COUNT=" << count << L" AEC_ENUMERATED=" << has_aec << std::endl;
    }
}
} // namespace
int wmain() {
    SetConsoleOutputCP(CP_UTF8);
    _setmode(_fileno(stdout), _O_U8TEXT);
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(initialized))
        return 1;
    int result = 0;
    try {
        std::wstring id;
        for (const auto &endpoint : endpoints(eCapture))
            if (endpoint.name.find(L"Brio 90") != std::wstring::npos)
                id = endpoint.id;
        if (id.empty())
            throw std::runtime_error("Brio 90 microphone not found");
        ComPtr<IMMDeviceEnumerator> enumerator;
        check(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator)),
              "Device enumerator");
        ComPtr<IMMDevice> device;
        check(enumerator->GetDevice(id.c_str(), &device), "Brio device");
        probe(device.Get(), false);
        probe(device.Get(), true);
        std::wcout << L"CAPABILITY PROBE COMPLETED (no effect state changed)" << std::endl;
    } catch (const std::exception &error) {
        std::wcout << L"FAIL " << error.what() << std::endl;
        result = 1;
    }
    CoUninitialize();
    return result;
}
