#include "windows/audio_observer.h"
#include "windows/diagnostics.h"
#include <audiopolicy.h>
#include <endpointvolume.h>
#include <ks.h>
#include <ksmedia.h>
#include <wrl/client.h>
#include <string>

namespace partymic {
using Microsoft::WRL::ComPtr;
namespace {
std::string context_text(LPCGUID context) {
    if(!context)return "none";
    wchar_t text[40]{};StringFromGUID2(*context,text,40);
    return diagnostics::utf8(text);
}
class SessionEvents final : public IAudioSessionEvents {
    LONG references_=1;
    std::string label_;
public:
    explicit SessionEvents(const char* label):label_(label){}
    STDMETHOD(QueryInterface)(REFIID iid,void** object) override {
        if(!object)return E_POINTER;*object=nullptr;
        if(iid!=__uuidof(IUnknown)&&iid!=__uuidof(IAudioSessionEvents))return E_NOINTERFACE;
        *object=static_cast<IAudioSessionEvents*>(this);AddRef();return S_OK;
    }
    STDMETHOD_(ULONG,AddRef)() override {return ULONG(InterlockedIncrement(&references_));}
    STDMETHOD_(ULONG,Release)() override {
        const auto left=InterlockedDecrement(&references_);if(!left)delete this;return ULONG(left);
    }
    STDMETHOD(OnDisplayNameChanged)(LPCWSTR,LPCGUID) override{return S_OK;}
    STDMETHOD(OnIconPathChanged)(LPCWSTR,LPCGUID) override{return S_OK;}
    STDMETHOD(OnSimpleVolumeChanged)(float volume,BOOL muted,LPCGUID context) override {
        try{diagnostics::write("audio.session.volume", "stream="+label_+" volume="+std::to_string(volume)+
            " muted="+std::to_string(muted)+" context="+context_text(context));}catch(...){}
        return S_OK;
    }
    STDMETHOD(OnChannelVolumeChanged)(DWORD count,float[],DWORD channel,LPCGUID) override {
        try{diagnostics::write("audio.session.channel_volume","stream="+label_+" count="+
            std::to_string(count)+" changed="+std::to_string(channel));}catch(...){}
        return S_OK;
    }
    STDMETHOD(OnGroupingParamChanged)(LPCGUID,LPCGUID) override{return S_OK;}
    STDMETHOD(OnStateChanged)(AudioSessionState state) override {
        try{diagnostics::write("audio.session.state","stream="+label_+" state="+std::to_string(state));}catch(...){}
        return S_OK;
    }
    STDMETHOD(OnSessionDisconnected)(AudioSessionDisconnectReason reason) override {
        static constexpr const char* reasons[]={"device_removal","server_shutdown","format_changed",
            "session_logoff","session_disconnected","exclusive_override"};
        const auto index=static_cast<unsigned>(reason);
        try{diagnostics::write("audio.session.disconnected","stream="+label_+" reason="+
            (index<6?reasons[index]:"unknown"));}catch(...){}
        return S_OK;
    }
};
class EndpointEvents final : public IAudioEndpointVolumeCallback {
    LONG references_=1;
    std::string label_;
public:
    explicit EndpointEvents(const char* label):label_(label){}
    STDMETHOD(QueryInterface)(REFIID iid,void** object) override {
        if(!object)return E_POINTER;*object=nullptr;
        if(iid!=__uuidof(IUnknown)&&iid!=__uuidof(IAudioEndpointVolumeCallback))return E_NOINTERFACE;
        *object=static_cast<IAudioEndpointVolumeCallback*>(this);AddRef();return S_OK;
    }
    STDMETHOD_(ULONG,AddRef)() override {return ULONG(InterlockedIncrement(&references_));}
    STDMETHOD_(ULONG,Release)() override {
        const auto left=InterlockedDecrement(&references_);if(!left)delete this;return ULONG(left);
    }
    STDMETHOD(OnNotify)(PAUDIO_VOLUME_NOTIFICATION_DATA data) override {
        if(!data)return E_POINTER;
        try{diagnostics::write("microphone.endpoint.changed","stream="+label_+
            " muted="+std::to_string(data->bMuted)+" volume="+std::to_string(data->fMasterVolume)+
            " context="+context_text(&data->guidEventContext));}catch(...){}
        return S_OK;
    }
};
}
struct AudioObserver::State {
    ComPtr<IAudioSessionControl> session;
    ComPtr<SessionEvents> session_events;
    ComPtr<IAudioEndpointVolume> endpoint;
    ComPtr<EndpointEvents> endpoint_events;
    bool session_registered=false,endpoint_registered=false;
    ~State() {
        if(session_registered)session->UnregisterAudioSessionNotification(session_events.Get());
        if(endpoint_registered)endpoint->UnregisterControlChangeNotify(endpoint_events.Get());
    }
};
AudioObserver::AudioObserver()=default;
AudioObserver::~AudioObserver(){close();}
void AudioObserver::close() noexcept {state_.reset();}
void AudioObserver::attach(IAudioClient* client,IMMDevice* endpoint,const char* label) {
    close();
    state_=std::make_unique<State>();
    auto& state=*state_;
    const auto session_hr=client->GetService(IID_PPV_ARGS(&state.session));
    if(SUCCEEDED(session_hr)) {
        state.session_events.Attach(new SessionEvents(label));
        const auto hr=state.session->RegisterAudioSessionNotification(state.session_events.Get());
        state.session_registered=SUCCEEDED(hr);
        diagnostics::write("audio.session.observe","stream="+std::string(label)+" register_hr="+std::to_string(hr));
    }else diagnostics::write("audio.session.observe","stream="+std::string(label)+" service_hr="+std::to_string(session_hr));
    if(endpoint&&SUCCEEDED(endpoint->Activate(__uuidof(IAudioEndpointVolume),CLSCTX_ALL,nullptr,
        reinterpret_cast<void**>(state.endpoint.GetAddressOf())))) {
        state.endpoint_events.Attach(new EndpointEvents(label));
        const auto hr=state.endpoint->RegisterControlChangeNotify(state.endpoint_events.Get());
        state.endpoint_registered=SUCCEEDED(hr);
        diagnostics::write("microphone.endpoint.observe","register_hr="+std::to_string(hr));
    }
}

}
