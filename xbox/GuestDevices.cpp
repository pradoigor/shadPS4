// SPDX-License-Identifier: GPL-2.0-or-later
#include "GuestDevices.h"
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Gaming.Input.h>
#include <xaudio2.h>
#include <roapi.h>
#include <algorithm>
#include <chrono>
#include <cstring>

namespace Lab {
namespace {
struct Apartment {
    HRESULT result=RoInitialize(RO_INIT_MULTITHREADED);
    ~Apartment() { if(SUCCEEDED(result)) RoUninitialize(); }
};
void EnsureApartment() { static thread_local Apartment apartment; if(FAILED(apartment.result) && apartment.result!=RPC_E_CHANGED_MODE) winrt::check_hresult(apartment.result); }
struct PadData {
    std::uint32_t buttons{};
    std::uint8_t lx{128},ly{128},rx{128},ry{128},l2{},r2{},padding[2]{};
    float orientation[4]{0,0,0,1},acceleration[3]{},angular[3]{};
    std::uint8_t touch[24]{},connected{};
    std::uint64_t timestamp{};
    std::uint8_t extension[16]{},connectedCount{},reserved[2]{},uniqueLength{},unique[12]{};
};
static_assert(sizeof(PadData)==120 && offsetof(PadData,timestamp)==80);
struct AudioPort final : IXAudio2VoiceCallback {
    IXAudio2SourceVoice* voice{};
    std::vector<std::uint8_t> bytes;
    std::mutex serial,completion;
    std::condition_variable ready;
    bool done{};
    HRESULT error{S_OK};
    ~AudioPort() { if(voice) voice->DestroyVoice(); }
    void STDMETHODCALLTYPE OnVoiceProcessingPassStart(UINT32) override {}
    void STDMETHODCALLTYPE OnVoiceProcessingPassEnd() override {}
    void STDMETHODCALLTYPE OnStreamEnd() override {}
    void STDMETHODCALLTYPE OnBufferStart(void*) override {}
    void STDMETHODCALLTYPE OnLoopEnd(void*) override {}
    void STDMETHODCALLTYPE OnBufferEnd(void*) override { std::lock_guard lock(completion); done=true; ready.notify_all(); }
    void STDMETHODCALLTYPE OnVoiceError(void*,HRESULT result) override { std::lock_guard lock(completion); error=result; done=true; ready.notify_all(); }
};
}
struct GuestDeviceState {
    std::mutex mutex;
    bool padOpen{};
    winrt::com_ptr<IXAudio2> engine;
    IXAudio2MasteringVoice* master{};
    std::unordered_map<std::uint64_t,std::shared_ptr<AudioPort>> ports;
    std::uint64_t nextPort{1};
    ~GuestDeviceState() { ports.clear(); if(master) master->DestroyVoice(); }
};
namespace {
GuestDeviceState& State(HleDispatcher& d) {
    static std::mutex creation; std::lock_guard lock(creation);
    if(!d.DeviceState()) d.DeviceState()=std::make_shared<GuestDeviceState>();
    return *d.DeviceState();
}
enum class Op { PadInit,PadOpen,PadClose,PadRead,PadVibration,AudioInit,AudioOpen,AudioClose,AudioOutput };
template<Op operation> std::uint64_t Call(HleDispatcher& d,GuestCallFrame const& f) noexcept {
    try {
        EnsureApartment(); auto& state=State(d);
        if constexpr(operation==Op::PadInit || operation==Op::AudioInit) return 0;
        else if constexpr(operation==Op::PadOpen) { std::lock_guard lock(state.mutex); state.padOpen=true; return 1; }
        else if constexpr(operation==Op::PadClose) { std::lock_guard lock(state.mutex); state.padOpen=false; return 0; }
        else if constexpr(operation==Op::PadRead || operation==Op::PadVibration) {
            { std::lock_guard lock(state.mutex); if(!state.padOpen || f.gpr[0]!=1) return 0x80920003ull; }
            using namespace winrt::Windows::Gaming::Input;
            auto pads=Gamepad::Gamepads();
            if constexpr(operation==Op::PadVibration) {
                auto* input=static_cast<std::uint8_t const*>(d.GuestReadable(f,f.gpr[1],2));
                if(!input) return 0x80920001ull;
                if(pads.Size()) pads.GetAt(0).Vibration({input[1]/255.0,input[0]/255.0,0,0});
                return 0;
            } else {
                auto* out=static_cast<PadData*>(d.GuestWritable(f,f.gpr[1],sizeof(PadData)));
                if(!out) return 0x80920001ull;
                PadData value;
                if(pads.Size()) {
                    auto r=pads.GetAt(0).GetCurrentReading(); value.connected=1; value.connectedCount=1; value.timestamp=r.Timestamp;
                    auto map=[&](GamepadButtons button,std::uint32_t mask){if((r.Buttons&button)!=GamepadButtons::None)value.buttons|=mask;};
                    map(GamepadButtons::A,0x4000); map(GamepadButtons::B,0x2000); map(GamepadButtons::X,0x8000); map(GamepadButtons::Y,0x1000);
                    map(GamepadButtons::DPadUp,0x10); map(GamepadButtons::DPadRight,0x20); map(GamepadButtons::DPadDown,0x40); map(GamepadButtons::DPadLeft,0x80);
                    map(GamepadButtons::LeftShoulder,0x400); map(GamepadButtons::RightShoulder,0x800);
                    map(GamepadButtons::LeftThumbstick,2); map(GamepadButtons::RightThumbstick,4);
                    const bool hostShortcut =
                        (r.Buttons & GamepadButtons::Menu) != GamepadButtons::None &&
                        (r.Buttons & GamepadButtons::View) != GamepadButtons::None;
                    if (!hostShortcut) {
                        map(GamepadButtons::Menu,8);
                        map(GamepadButtons::View,0x100000);
                    }
                    auto axis=[](double x){return static_cast<std::uint8_t>(std::clamp((x+1)*127.5,0.0,255.0)+0.5);};
                    value.lx=axis(r.LeftThumbstickX); value.ly=axis(-r.LeftThumbstickY); value.rx=axis(r.RightThumbstickX); value.ry=axis(-r.RightThumbstickY);
                    value.l2=static_cast<std::uint8_t>(r.LeftTrigger*255); value.r2=static_cast<std::uint8_t>(r.RightTrigger*255);
                    if(value.l2>0)value.buttons|=0x100; if(value.r2>0)value.buttons|=0x200;
                }
                *out=value; return 0;
            }
        } else if constexpr(operation==Op::AudioOpen) {
            const auto samples=f.gpr[3],rate=f.gpr[4],format=f.gpr[5]&0xff;
            if(samples==0 || samples>8192) return 0x80260006ull;
            if(rate!=48000) return 0x80260008ull;
            if(format!=0 && format!=1 && format!=3 && format!=4) return 0x80260007ull;
            std::lock_guard lock(state.mutex);
            if(state.ports.size()>=8) return 0x80260005ull;
            if(!state.engine) winrt::check_hresult(XAudio2Create(state.engine.put(),0,XAUDIO2_DEFAULT_PROCESSOR));
            if(!state.master) winrt::check_hresult(state.engine->CreateMasteringVoice(&state.master,XAUDIO2_DEFAULT_CHANNELS,48000,0,nullptr,nullptr,AudioCategory_GameMedia));
            WAVEFORMATEX wave{}; wave.wFormatTag=format>=3?WAVE_FORMAT_IEEE_FLOAT:WAVE_FORMAT_PCM;
            wave.nChannels=(format==1 || format==4)?2:1; wave.nSamplesPerSec=48000; wave.wBitsPerSample=format>=3?32:16;
            wave.nBlockAlign=wave.nChannels*wave.wBitsPerSample/8; wave.nAvgBytesPerSec=wave.nSamplesPerSec*wave.nBlockAlign;
            auto port=std::make_shared<AudioPort>(); port->bytes.resize(samples*wave.nBlockAlign);
            winrt::check_hresult(state.engine->CreateSourceVoice(&port->voice,&wave,0,XAUDIO2_DEFAULT_FREQ_RATIO,port.get()));
            winrt::check_hresult(port->voice->Start());
            auto handle=state.nextPort++; state.ports.emplace(handle,std::move(port));
            d.GraphicsLog("AudioOut: XAudio2 ativo, 48000 Hz, canais="+std::to_string(wave.nChannels)); return handle;
        } else {
            std::shared_ptr<AudioPort> port;
            { std::lock_guard lock(state.mutex); auto it=state.ports.find(f.gpr[0]); if(it==state.ports.end())return 0x80260003ull; port=it->second;
              if constexpr(operation==Op::AudioClose) { state.ports.erase(it); return 0; } }
            std::lock_guard serial(port->serial);
            if(!port->voice) return 0x80260003ull;
            if(!f.gpr[1]) return 0; // Output is synchronous; the previous buffer has already completed.
            auto* data=d.GuestReadable(f,f.gpr[1],port->bytes.size()); if(!data)return 0x80260004ull;
            std::memcpy(port->bytes.data(),data,port->bytes.size());
            { std::lock_guard lock(port->completion); port->done=false; port->error=S_OK; }
            XAUDIO2_BUFFER buffer{}; buffer.AudioBytes=static_cast<UINT32>(port->bytes.size()); buffer.pAudioData=port->bytes.data();
            winrt::check_hresult(port->voice->SubmitSourceBuffer(&buffer));
            std::unique_lock lock(port->completion);
            if(!port->ready.wait_for(lock,std::chrono::seconds(2),[&]{return port->done;})) {
                lock.unlock(); port->voice->DestroyVoice(); port->voice=nullptr;
                {std::lock_guard stateLock(state.mutex); state.ports.erase(f.gpr[0]);}
                d.GraphicsLog("AudioOut: timeout aguardando reprodução PCM."); return 0x80260011ull;
            }
            return FAILED(port->error)?0x80260011ull:0;
        }
    } catch(winrt::hresult_error const& error) {
        d.GraphicsLog("Dispositivo UWP: HRESULT="+std::to_string(static_cast<std::uint32_t>(error.code().value)));
        return operation==Op::AudioOpen || operation==Op::AudioOutput?0x80260011ull:0x80020005ull;
    } catch(...) { return 0x80020005ull; }
}
}
HleHandler LookupDeviceHandler(std::string_view name) noexcept {
    if(name=="scePadInit")return &Call<Op::PadInit>;
    if(name=="scePadOpen")return &Call<Op::PadOpen>;
    if(name=="scePadClose")return &Call<Op::PadClose>;
    if(name=="scePadReadState")return &Call<Op::PadRead>;
    if(name=="scePadSetVibration")return &Call<Op::PadVibration>;
    if(name=="sceAudioOutInit")return &Call<Op::AudioInit>;
    if(name=="sceAudioOutOpen")return &Call<Op::AudioOpen>;
    if(name=="sceAudioOutClose")return &Call<Op::AudioClose>;
    if(name=="sceAudioOutOutput")return &Call<Op::AudioOutput>;
    return nullptr;
}
}
