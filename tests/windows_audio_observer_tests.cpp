// No audio hardware: verify native-format fallback and subscription ownership.
#include "windows/audio_observer.h"
#include <audiopolicy.h>
#include <cstring>
#include <iostream>
#include <stdexcept>
using namespace partymic;
namespace {
void require(bool value,const char* reason){if(!value)throw std::runtime_error(reason);}
class Session final : public IAudioSessionControl {
public:
    ULONG references=1;
    unsigned registered=0,unregistered=0;
    IAudioSessionEvents* events=nullptr;
    ~Session(){if(events)events->Release();}
    STDMETHOD(QueryInterface)(REFIID iid,void** value) override {
        if(!value)return E_POINTER;*value=nullptr;
        if(iid!=__uuidof(IUnknown)&&iid!=__uuidof(IAudioSessionControl))return E_NOINTERFACE;
        *value=static_cast<IAudioSessionControl*>(this);AddRef();return S_OK;
    }
    STDMETHOD_(ULONG,AddRef)() override{return ++references;}
    STDMETHOD_(ULONG,Release)() override{return --references;}
    STDMETHOD(GetState)(AudioSessionState*) override{return E_NOTIMPL;}
    STDMETHOD(GetDisplayName)(LPWSTR*) override{return E_NOTIMPL;}
    STDMETHOD(SetDisplayName)(LPCWSTR,LPCGUID) override{return E_NOTIMPL;}
    STDMETHOD(GetIconPath)(LPWSTR*) override{return E_NOTIMPL;}
    STDMETHOD(SetIconPath)(LPCWSTR,LPCGUID) override{return E_NOTIMPL;}
    STDMETHOD(GetGroupingParam)(GUID*) override{return E_NOTIMPL;}
    STDMETHOD(SetGroupingParam)(LPCGUID,LPCGUID) override{return E_NOTIMPL;}
    STDMETHOD(RegisterAudioSessionNotification)(IAudioSessionEvents* value) override {
        if(events)return E_UNEXPECTED;
        events=value;events->AddRef();++registered;return S_OK;
    }
    STDMETHOD(UnregisterAudioSessionNotification)(IAudioSessionEvents* value) override {
        if(value!=events)return E_INVALIDARG;
        events->Release();events=nullptr;++unregistered;return S_OK;
    }
};
class Client final : public IAudioClient {
public:
    WAVEFORMATEXTENSIBLE format{};
    Session session;
    HRESULT query_result=S_OK;
    bool touched=false;
    Client(){format.Format={WAVE_FORMAT_IEEE_FLOAT,2,48000,384000,8,32,0};}
    STDMETHOD(QueryInterface)(REFIID,void**) override{return E_NOINTERFACE;}
    STDMETHOD_(ULONG,AddRef)() override{return 1;}
    STDMETHOD_(ULONG,Release)() override{return 1;}
    STDMETHOD(Initialize)(AUDCLNT_SHAREMODE,DWORD,REFERENCE_TIME,REFERENCE_TIME,const WAVEFORMATEX*,LPCGUID) override {touched=true;return E_NOTIMPL;}
    STDMETHOD(GetBufferSize)(UINT32*) override{return E_NOTIMPL;}
    STDMETHOD(GetStreamLatency)(REFERENCE_TIME*) override{return E_NOTIMPL;}
    STDMETHOD(GetCurrentPadding)(UINT32*) override{return E_NOTIMPL;}
    STDMETHOD(IsFormatSupported)(AUDCLNT_SHAREMODE,const WAVEFORMATEX*,WAVEFORMATEX**) override{return E_NOTIMPL;}
    STDMETHOD(GetMixFormat)(WAVEFORMATEX** value) override {
        *value=nullptr;if(FAILED(query_result))return query_result;
        auto* result=static_cast<WAVEFORMATEXTENSIBLE*>(CoTaskMemAlloc(sizeof(format)));
        if(!result)return E_OUTOFMEMORY;
        *result=format;*value=&result->Format;return S_OK;
    }
    STDMETHOD(GetDevicePeriod)(REFERENCE_TIME*,REFERENCE_TIME*) override{return E_NOTIMPL;}
    STDMETHOD(Start)() override{touched=true;return E_NOTIMPL;}
    STDMETHOD(Stop)() override{touched=true;return E_NOTIMPL;}
    STDMETHOD(Reset)() override{touched=true;return E_NOTIMPL;}
    STDMETHOD(SetEventHandle)(HANDLE) override{return E_NOTIMPL;}
    STDMETHOD(GetService)(REFIID iid,void** value) override {
        return session.QueryInterface(iid,value);
    }
};
void subscriptions() {
    Client client;
    {
        AudioObserver observer;
        observer.attach(&client,nullptr,"test");
        require(client.session.references==2&&client.session.events,"Observer owns one session reference");
        // Simulate in-flight notification ownership while the observer detaches.
        auto* pending=client.session.events;pending->AddRef();
        observer.close();
        require(client.session.references==1&&!client.session.events,"Close releases session registration and reference");
        require(pending->OnSimpleVolumeChanged(0.7f,FALSE,nullptr)==S_OK,"Detached pending callback remains valid");
        pending->Release();
        observer.attach(&client,nullptr,"reattach");
    }
    require(client.session.registered==2&&client.session.unregistered==2,"Destructor unregisters every attached session");
    require(client.session.references==1&&!client.touched,"Observer leaves client lifecycle and endpoint settings alone");
}
}
int main() {
    try{subscriptions();std::cout<<"PASS subscription release and pending callback lifetime\n";return 0;}
    catch(const std::exception& error){std::cerr<<"FAIL "<<error.what()<<'\n';return 1;}
}
