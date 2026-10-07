#include "wasapi_engine.h"
#include <audioclient.h>
#include <avrt.h>
#include <mmdeviceapi.h>
#include <functiondiscoverykeys_devpkey.h>
#include <ks.h>
#include <ksmedia.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <utility>
#include <xmmintrin.h>

namespace ty {
using Microsoft::WRL::ComPtr;
namespace {
struct AudioFailure { HRESULT code; std::wstring where; };
void check(HRESULT hr,const wchar_t* where) {if(FAILED(hr)) throw AudioFailure{hr,where};}
struct ComScope {
    HRESULT hr=CoInitializeEx(nullptr,COINIT_MULTITHREADED);
    ComScope(){check(hr,L"初始化 COM");}
    ~ComScope(){if(SUCCEEDED(hr))CoUninitialize();}
};
struct TaskMemDelete {void operator()(void* p)const noexcept{CoTaskMemFree(p);}};
struct Handle {
    HANDLE value=nullptr;
    Handle():value(CreateEventW(nullptr,FALSE,FALSE,nullptr)){if(!value)throw AudioFailure{HRESULT_FROM_WIN32(GetLastError()),L"创建音频事件"};}
    ~Handle(){if(value)CloseHandle(value);}
};
ComPtr<IMMDeviceEnumerator> enumerator() {
    ComPtr<IMMDeviceEnumerator> e;
    check(CoCreateInstance(__uuidof(MMDeviceEnumerator),nullptr,CLSCTX_ALL,IID_PPV_ARGS(&e)),L"打开设备管理器"); return e;
}
struct Client {
    Handle event; // outlives COM clients that retain the event handle
    ComPtr<IMMDevice> endpoint;
    ComPtr<IAudioClient> audio;
    ComPtr<IAudioCaptureClient> capture;
    ComPtr<IAudioRenderClient> render;
    PcmFormat format;
    std::vector<std::uint8_t> wave;
    UINT32 buffer=0,period=0;
    REFERENCE_TIME reportedLatency=0;
    bool exclusive=false,lowLatency=false,raw=false,started=false;
    ~Client(){if(started&&audio)audio->Stop();}
};
PcmFormat parse_format(const WAVEFORMATEX* w) {
    PcmFormat f{w->nSamplesPerSec,w->nChannels,w->wBitsPerSample,w->wBitsPerSample,false};
    if(w->wFormatTag==WAVE_FORMAT_IEEE_FLOAT)f.floating=true;
    else if(w->wFormatTag==WAVE_FORMAT_EXTENSIBLE&&w->cbSize>=22) {
        const auto* x=reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(w);
        f.validBits=x->Samples.wValidBitsPerSample?x->Samples.wValidBitsPerSample:f.bits;
        if(IsEqualGUID(x->SubFormat,KSDATAFORMAT_SUBTYPE_IEEE_FLOAT))f.floating=true;
        else if(!IsEqualGUID(x->SubFormat,KSDATAFORMAT_SUBTYPE_PCM))throw AudioFailure{AUDCLNT_E_UNSUPPORTED_FORMAT,L"设备不是 PCM/Float 格式"};
    } else if(w->wFormatTag!=WAVE_FORMAT_PCM)throw AudioFailure{AUDCLNT_E_UNSUPPORTED_FORMAT,L"不支持设备的编码格式"};
    if(!f.supported()||w->nBlockAlign!=f.frame_bytes())throw AudioFailure{AUDCLNT_E_UNSUPPORTED_FORMAT,L"采样率、声道数或位深不支持"};
    return f;
}
void activate(Client& c,bool requestRaw) {
    c.audio.Reset();
    check(c.endpoint->Activate(__uuidof(IAudioClient),CLSCTX_ALL,nullptr,reinterpret_cast<void**>(c.audio.GetAddressOf())),L"激活音频设备");
    c.raw=false;
    ComPtr<IAudioClient2> v2;
    if(SUCCEEDED(c.audio.As(&v2))) {
        AudioClientProperties props{}; props.cbSize=sizeof(props); props.eCategory=AudioCategory_Other;
        props.Options=requestRaw?AUDCLNT_STREAMOPTIONS_RAW:AUDCLNT_STREAMOPTIONS_NONE;
        if(SUCCEEDED(v2->SetClientProperties(&props)))c.raw=requestRaw;
        else {props.Options=AUDCLNT_STREAMOPTIONS_NONE; v2->SetClientProperties(&props);}
    }
}
void choose_exclusive_format(Client& c,unsigned rate,const WAVEFORMATEX* mix) {
    // Probe supported hardware formats. Never silently switch the requested rate.
    std::array<unsigned,3> channels{mix->nChannels,2,1};
    for(unsigned ch:channels) for(unsigned kind=0;kind<4;++kind) {
        if(ch==0||ch>32)continue;
        WAVEFORMATEXTENSIBLE f{};
        f.Format.wFormatTag=WAVE_FORMAT_EXTENSIBLE;f.Format.cbSize=22;
        f.Format.nChannels=static_cast<WORD>(ch);f.Format.nSamplesPerSec=rate;
        f.Format.wBitsPerSample=static_cast<WORD>(kind==0||kind==1?32:kind==2?24:16);
        f.Samples.wValidBitsPerSample=f.Format.wBitsPerSample;
        f.Format.nBlockAlign=static_cast<WORD>(ch*f.Format.wBitsPerSample/8);
        f.Format.nAvgBytesPerSec=rate*f.Format.nBlockAlign;
        if(ch==1)f.dwChannelMask=SPEAKER_FRONT_CENTER;
        else if(ch==2)f.dwChannelMask=SPEAKER_FRONT_LEFT|SPEAKER_FRONT_RIGHT;
        else if(mix->wFormatTag==WAVE_FORMAT_EXTENSIBLE&&mix->cbSize>=22)
            f.dwChannelMask=reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(mix)->dwChannelMask;
        f.SubFormat=kind==0?KSDATAFORMAT_SUBTYPE_IEEE_FLOAT:KSDATAFORMAT_SUBTYPE_PCM;
        if(c.audio->IsFormatSupported(AUDCLNT_SHAREMODE_EXCLUSIVE,&f.Format,nullptr)==S_OK) {
            c.wave.resize(sizeof(f));std::memcpy(c.wave.data(),&f,sizeof(f));return;
        }
        // Some older stereo/mono drivers only accept WAVEFORMATEX.
        if(ch<=2) {
            f.Format.wFormatTag=kind==0?WAVE_FORMAT_IEEE_FLOAT:WAVE_FORMAT_PCM;f.Format.cbSize=0;
            if(c.audio->IsFormatSupported(AUDCLNT_SHAREMODE_EXCLUSIVE,&f.Format,nullptr)==S_OK) {
                c.wave.resize(sizeof(WAVEFORMATEX));std::memcpy(c.wave.data(),&f.Format,sizeof(WAVEFORMATEX));return;
            }
        }
    }
    throw AudioFailure{AUDCLNT_E_UNSUPPORTED_FORMAT,L"独占模式不支持所选采样率；尝试 48/44.1 kHz 或共享模式"};
}
void open_client(Client& c,const std::wstring& id,bool isCapture,const StreamConfig& config) {
    c.exclusive=config.exclusive;
    auto e=enumerator();check(e->GetDevice(id.c_str(),&c.endpoint),L"查找所选设备（未自动替换）");
    activate(c,config.raw&&!config.exclusive);
    WAVEFORMATEX* mixRaw=nullptr;check(c.audio->GetMixFormat(&mixRaw),L"读取设备格式");
    std::unique_ptr<WAVEFORMATEX,TaskMemDelete> mix(mixRaw);
    if(config.exclusive)choose_exclusive_format(c,config.exclusiveRate,mix.get());
    else {
        const auto bytes=sizeof(WAVEFORMATEX)+mix->cbSize;c.wave.resize(bytes);std::memcpy(c.wave.data(),mix.get(),bytes);
    }
    const auto* wf=reinterpret_cast<const WAVEFORMATEX*>(c.wave.data());c.format=parse_format(wf);
    HRESULT hr=E_FAIL;
    const DWORD flags=AUDCLNT_STREAMFLAGS_EVENTCALLBACK;
    if(config.exclusive) {
        REFERENCE_TIME def=0,min=0;check(c.audio->GetDevicePeriod(&def,&min),L"读取独占周期");
        REFERENCE_TIME duration=std::max(min,static_cast<REFERENCE_TIME>(std::llround(config.periodMs*10000)));
        hr=c.audio->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE,flags,duration,duration,wf,nullptr);
        if(hr==AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED) {
            UINT32 aligned=0;check(c.audio->GetBufferSize(&aligned),L"读取驱动对齐大小");
            duration=static_cast<REFERENCE_TIME>(std::llround(10000000.0*aligned/c.format.rate));
            activate(c,false);
            hr=c.audio->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE,flags,duration,duration,wf,nullptr);
        }
        check(hr,L"启动独占模式（不自动降级）");c.lowLatency=true;
    } else {
        ComPtr<IAudioClient3> v3;
        if(SUCCEEDED(c.audio.As(&v3))) {
            UINT32 def=0,step=0,lo=0,hi=0;
            if(SUCCEEDED(v3->GetSharedModeEnginePeriod(wf,&def,&step,&lo,&hi))&&step>0&&hi>=lo) {
                const auto requested=static_cast<UINT32>(std::ceil(config.periodMs*c.format.rate/1000));
                const UINT32 lower=((std::max(lo,requested)+step-1)/step)*step;
                c.period=std::min(lower,(hi/step)*step);
                if(c.period>=lo)hr=v3->InitializeSharedAudioStream(flags,c.period,wf,nullptr);
                if(SUCCEEDED(hr))c.lowLatency=true;
            }
        }
        if(FAILED(hr)) {
            // A failed Initialize can poison the client; recreate before fallback.
            // The UI exposes this fallback explicitly via the lowLatency flag.
            v3.Reset();activate(c,config.raw);
            hr=c.audio->Initialize(AUDCLNT_SHAREMODE_SHARED,flags,0,0,wf,nullptr);
            check(hr,L"启动共享模式");
            REFERENCE_TIME def=0,min=0;check(c.audio->GetDevicePeriod(&def,&min),L"读取共享周期");
            c.period=std::max<UINT32>(1,static_cast<UINT32>(std::llround(def*c.format.rate/10000000.0)));
        }
    }
    check(c.audio->SetEventHandle(c.event.value),L"绑定音频事件");
    check(c.audio->GetBufferSize(&c.buffer),L"读取实际缓冲大小");
    if(c.exclusive)c.period=c.buffer;
    if(c.buffer==0||c.buffer>c.format.rate*2||c.period==0)throw AudioFailure{E_INVALIDARG,L"驱动返回了不合理的缓冲大小"};
    c.audio->GetStreamLatency(&c.reportedLatency);
    if(isCapture)check(c.audio->GetService(IID_PPV_ARGS(&c.capture)),L"获取输入服务");
    else check(c.audio->GetService(IID_PPV_ARGS(&c.render)),L"获取输出服务");
}
struct Mmcss {
    DWORD task=0;HANDLE handle=AvSetMmThreadCharacteristicsW(L"Pro Audio",&task);
    Mmcss(){if(handle)AvSetMmThreadPriority(handle,AVRT_PRIORITY_CRITICAL);}
    ~Mmcss(){if(handle)AvRevertMmThreadCharacteristics(handle);}
};
void atomic_peak(std::atomic<float>& dst,float value) noexcept {
    float old=dst.load(std::memory_order_relaxed);
    while(old<value&&!dst.compare_exchange_weak(old,value,std::memory_order_relaxed)){}
}
} // namespace
std::vector<Endpoint> enumerate_endpoints(bool capture) {
    auto e=enumerator();ComPtr<IMMDeviceCollection> list; const auto flow=capture?eCapture:eRender;
    check(e->EnumAudioEndpoints(flow,DEVICE_STATE_ACTIVE,&list),L"枚举音频设备");
    std::wstring defaultId;ComPtr<IMMDevice> def;
    if(SUCCEEDED(e->GetDefaultAudioEndpoint(flow,eConsole,&def))) {
        LPWSTR id=nullptr;if(SUCCEEDED(def->GetId(&id))){defaultId=id;CoTaskMemFree(id);}
    }
    UINT count=0;check(list->GetCount(&count),L"读取设备数量");std::vector<Endpoint> result;
    for(UINT i=0;i<count;++i) {
        ComPtr<IMMDevice> d;LPWSTR id=nullptr;ComPtr<IPropertyStore> props;PROPVARIANT value;PropVariantInit(&value);
        if(FAILED(list->Item(i,&d))||FAILED(d->GetId(&id)))continue;
        Endpoint x;x.id=id;CoTaskMemFree(id);x.name=L"未命名音频设备";x.isDefault=x.id==defaultId;
        if(SUCCEEDED(d->OpenPropertyStore(STGM_READ,&props))&&SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName,&value))&&value.vt==VT_LPWSTR)x.name=value.pwszVal;
        PropVariantClear(&value);result.push_back(std::move(x));
    }
    return result;
}
std::wstring audio_error(HRESULT hr) {
    const wchar_t* text=L"音频设备错误";
    switch(hr) {
    case AUDCLNT_E_DEVICE_INVALIDATED:text=L"设备已拔出、禁用或配置发生变化；监听已停止。请刷新并重新选择设备。";break;
    case AUDCLNT_E_DEVICE_IN_USE:text=L"设备正被其他程序占用；关闭 DAW/会议软件，或改用共享模式。";break;
    case AUDCLNT_E_EXCLUSIVE_MODE_NOT_ALLOWED:text=L"系统未允许应用独占设备；在声音设备高级属性中启用独占，或选择共享模式。";break;
    case AUDCLNT_E_UNSUPPORTED_FORMAT:text=L"设备不支持该音频格式；尝试 48/44.1 kHz 或共享模式。";break;
    case AUDCLNT_E_SERVICE_NOT_RUNNING:text=L"Windows Audio 服务没有运行。";break;
    case E_ACCESSDENIED:text=L"麦克风权限被拒绝；开启 Windows 的麦克风和桌面应用访问权限。";break;
    case HRESULT_FROM_WIN32(ERROR_TIMEOUT):text=L"设备超过 2 秒未交付音频；已停止而非继续累积延迟。检查连接并重新启动监听。";break;
    default:break;
    }
    wchar_t hex[24];swprintf_s(hex,L" [0x%08lX]",static_cast<unsigned long>(hr));return std::wstring(text)+hex;
}
void EngineStats::reset() noexcept {
    inputRate=outputRate=inputChannels=outputChannels=0;inputPeriod=outputPeriod=inputBuffer=outputBuffer=0;
    inputLatencyMs=outputLatencyMs=queueMs=driftPpm=loadPercent=0;inputPeak=outputPeakL=outputPeakR=0;
    underruns=resyncs=discarded=discontinuities=clipped=limited=0;
    mmcss=inputLowLatency=outputLowLatency=inputRaw=outputRaw=false;
}
WasapiEngine::WasapiEngine(){stopEvent_=CreateEventW(nullptr,TRUE,FALSE,nullptr);if(!stopEvent_)throw std::runtime_error("CreateEvent failed");}
WasapiEngine::~WasapiEngine(){stop();CloseHandle(stopEvent_);}
void WasapiEngine::start(StreamConfig config) {
    stop();stats.reset();{std::lock_guard<std::mutex> lock(errorMutex_);error_.clear();}
    ResetEvent(stopEvent_);state_=EngineState::Starting;
    try{worker_=std::thread(&WasapiEngine::run,this,std::move(config));}
    catch(...){state_=EngineState::Stopped;throw;}
}
void WasapiEngine::stop() {
    SetEvent(stopEvent_);if(worker_.joinable())worker_.join();recorder.stop();state_=EngineState::Stopped;
}
std::wstring WasapiEngine::last_error()const{std::lock_guard<std::mutex> lock(errorMutex_);return error_;}
void WasapiEngine::run(StreamConfig config) noexcept {
    try {
        ComScope com;Client in,out;
        if(config.inputId.empty()||config.outputId.empty())throw AudioFailure{E_INVALIDARG,L"必须选择明确的本地输入和输出设备"};
        open_client(in,config.inputId,true,config);open_client(out,config.outputId,false,config);
        if(parameters.load().route==Route::MonoRight&&in.format.channels<2)throw AudioFailure{E_INVALIDARG,L"所选输入只有一个通道；请选择通道 1"};
        stats.inputRate=in.format.rate;stats.outputRate=out.format.rate;
        stats.inputChannels=in.format.channels;stats.outputChannels=out.format.channels;
        stats.inputPeriod=in.period;stats.outputPeriod=out.period;stats.inputBuffer=in.buffer;stats.outputBuffer=out.buffer;
        stats.inputLatencyMs=in.reportedLatency/10000.0;stats.outputLatencyMs=out.reportedLatency/10000.0;
        stats.inputLowLatency=in.lowLatency;stats.outputLowLatency=out.lowLatency;stats.inputRaw=in.raw;stats.outputRaw=out.raw;
        AudioBridge bridge;bridge.reset(in.format.rate,out.format.rate,in.period,out.period,config.safetyBlocks);
        VoiceProcessor voice;voice.reset(in.format.rate);MonitorGain gain;gain.reset(out.format.rate);
        std::vector<Frame> scratch(out.buffer); // allocate before entering the audio loop
        Mmcss priority;stats.mmcss=priority.handle!=nullptr;
        _MM_SET_FLUSH_ZERO_MODE(_MM_FLUSH_ZERO_ON);
        LARGE_INTEGER frequency{};QueryPerformanceFrequency(&frequency);
        BYTE* initial=nullptr;const UINT32 prime=out.exclusive?out.buffer:std::min(out.buffer,out.period);
        check(out.render->GetBuffer(prime,&initial),L"初始化输出缓冲");
        check(out.render->ReleaseBuffer(prime,AUDCLNT_BUFFERFLAGS_SILENT),L"静音预填充");
        if(WaitForSingleObject(stopEvent_,0)==WAIT_OBJECT_0){state_=EngineState::Stopped;return;}
        check(in.audio->Start(),L"开始采集");in.started=true;
        check(out.audio->Start(),L"开始播放");out.started=true;
        state_.store(EngineState::Running,std::memory_order_release);
        HANDLE events[]{stopEvent_,out.event.value,in.event.value};
        ULONGLONG lastInput=GetTickCount64(),lastOutput=lastInput;
        bool seenCapture=false;double busySeconds=0;
        while(true) {
            const DWORD wait=WaitForMultipleObjects(3,events,FALSE,1000);
            if(wait==WAIT_OBJECT_0)break;
            if(wait==WAIT_FAILED)throw AudioFailure{HRESULT_FROM_WIN32(GetLastError()),L"等待音频事件"};
            const auto now=GetTickCount64();
            if(now-lastInput>2000||now-lastOutput>2000)throw AudioFailure{HRESULT_FROM_WIN32(ERROR_TIMEOUT),L"设备无响应"};
            LARGE_INTEGER begin{},end{};QueryPerformanceCounter(&begin);
            // Always drain input first, including when the render event woke us.
            bool captureReady=wait==WAIT_OBJECT_0+2;
            if(!captureReady)captureReady=WaitForSingleObject(in.event.value,0)==WAIT_OBJECT_0;
            UINT32 packet=0;
            if(in.exclusive)packet=captureReady?in.buffer:0;
            else check(in.capture->GetNextPacketSize(&packet),L"查询输入缓冲");
            const auto p=parameters.load();voice.configure(p);gain.configure(p);
            unsigned packetsProcessed=0;
            while(packet>0&&packetsProcessed++<16) {
                if(WaitForSingleObject(stopEvent_,0)==WAIT_OBJECT_0)break;
                BYTE* data=nullptr;UINT32 n=0;DWORD flags=0;
                const HRESULT hr=in.capture->GetBuffer(&data,&n,&flags,nullptr,nullptr);
                if(hr==AUDCLNT_S_BUFFER_EMPTY)break;
                check(hr,L"读取输入音频");
                if((flags&AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY)&&seenCapture){++stats.discontinuities;bridge.discontinuity();}
                seenCapture=true;float peak=0;std::uint64_t clips=0;
                const bool silent=(flags&AUDCLNT_BUFFERFLAGS_SILENT)!=0||data==nullptr;
                for(UINT32 i=0;i<n;++i) {
                    Frame f{};
                    if(!silent) {
                        const auto* src=data+static_cast<std::size_t>(i)*in.format.frame_bytes();
                        f.l=read_pcm(src,in.format);f.r=in.format.channels>1?read_pcm(src+in.format.sample_bytes(),in.format):f.l;
                        const auto level=std::max(std::abs(f.l),std::abs(f.r));peak=std::max(peak,level);if(level>=.999f)++clips;
                        if(p.route==Route::MonoLeft)f.r=f.l;
                        else if(p.route==Route::MonoRight)f.l=f.r;
                        else if(p.route==Route::MonoMix)f.l=f.r=(f.l+f.r)*.5f;
                    }
                    bridge.push(voice.process(f));
                }
                check(in.capture->ReleaseBuffer(n),L"释放输入缓冲");
                stats.clipped.fetch_add(clips);atomic_peak(stats.inputPeak,peak);lastInput=GetTickCount64();
                if(in.exclusive)packet=0; // GetNextPacketSize is shared-mode ONLY.
                else check(in.capture->GetNextPacketSize(&packet),L"继续读取输入");
            }
            bool renderReady=wait==WAIT_OBJECT_0+1;
            if(!renderReady)renderReady=WaitForSingleObject(out.event.value,0)==WAIT_OBJECT_0;
            UINT32 writtenNow=0;
            if(renderReady) {
                UINT32 n=out.buffer;
                if(!out.exclusive) {
                    UINT32 padding=0;check(out.audio->GetCurrentPadding(&padding),L"读取输出队列");
                    // Keep only one engine quantum queued, not the whole shared-mode capacity.
                    n=padding>=out.period?0:std::min(out.buffer-padding,out.period-padding);
                }
                if(n>0) {
                    BYTE* dst=nullptr;check(out.render->GetBuffer(n,&dst),L"获取输出缓冲");
                    bridge.render(scratch.data(),n);float l=0,r=0;
                    for(UINT32 i=0;i<n;++i) {
                        auto f=gain.process(scratch[i]);scratch[i]=f;l=std::max(l,std::abs(f.l));r=std::max(r,std::abs(f.r));
                        auto* frame=dst+static_cast<std::size_t>(i)*out.format.frame_bytes();
                        for(unsigned ch=0;ch<out.format.channels;++ch)
                            write_pcm(frame+ch*out.format.sample_bytes(),out.format.channels==1?(f.l+f.r)*.5f:ch==0?f.l:ch==1?f.r:0,out.format);
                    }
                    check(out.render->ReleaseBuffer(n,0),L"提交输出音频");
                    recorder.push(scratch.data(),n);atomic_peak(stats.outputPeakL,l);atomic_peak(stats.outputPeakR,r);writtenNow=n;
                }
                lastOutput=GetTickCount64();
            }
            QueryPerformanceCounter(&end);busySeconds+=static_cast<double>(end.QuadPart-begin.QuadPart)/frequency.QuadPart;
            if(writtenNow) {
                stats.loadPercent=100*busySeconds/(static_cast<double>(writtenNow)/out.format.rate);busySeconds=0;
                stats.queueMs=1000.0*bridge.queued_frames()/in.format.rate;stats.driftPpm=bridge.correction_ppm();
                stats.underruns=bridge.underruns();stats.resyncs=bridge.resyncs();stats.discarded=bridge.discarded_frames();stats.limited=gain.limited_samples();
            }
        }
        // Client destructors stop/release streams on their owning COM thread.
        state_.store(EngineState::Stopped,std::memory_order_release);
    } catch(const AudioFailure& f) {
        {std::lock_guard<std::mutex> lock(errorMutex_);error_=f.where+L"\r\n"+audio_error(f.code);}
        state_.store(EngineState::Failed,std::memory_order_release);
    } catch(const std::exception&) {
        {std::lock_guard<std::mutex> lock(errorMutex_);error_=L"音频引擎初始化失败（内存/线程资源）。监听已停止。";}
        state_.store(EngineState::Failed,std::memory_order_release);
    } catch(...) {
        {std::lock_guard<std::mutex> lock(errorMutex_);error_=L"未预期音频错误；监听已停止。";}
        state_.store(EngineState::Failed,std::memory_order_release);
    }
}
} // namespace ty
