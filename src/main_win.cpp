#include "wasapi_engine.h"
#include <windowsx.h>
#include <commctrl.h>
#include <commdlg.h>
#include <mmdeviceapi.h>
#include <shlobj.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>
#include <memory>
#include <sstream>
#include <string>

using Microsoft::WRL::ComPtr;
namespace {
constexpr UINT devicesChanged=WM_APP+2;
constexpr COLORREF bg=RGB(247,249,252),ink=RGB(27,40,60),mutedInk=RGB(82,99,122),accent=RGB(0,112,111);
enum Id:int {
    In=101,Out,Mode,Period,Rate,Safety,Route,Raw,Refresh,Start,Mute,Bypass,Record,SaveReport,Help,Confirm,
    InputGain,OutputGain,Low,Mid,High,GateLevel,Highpass,Gate,Compressor,Defaults,Status,Notice,Details,
    InputValue,OutputValue,LowValue,MidValue,HighValue,GateValue,MeterIn,MeterL,MeterR,Panic,MuteKey,
    LabelIn=201,LabelOut,LabelMode,LabelPeriod,LabelRate,LabelSafety,LabelRoute,
    LabelInputGain,LabelOutputGain,LabelLow,LabelMid,LabelHigh,LabelGate,LabelMeters,LabelDiag
};
std::wstring utf8_to_wide(const std::string& text) {
    int n=MultiByteToWideChar(CP_UTF8,0,text.c_str(),-1,nullptr,0);
    if(n<=0)return L"";
    std::wstring result(static_cast<std::size_t>(n),L'\0');
    MultiByteToWideChar(CP_UTF8,0,text.c_str(),-1,result.data(),n);result.pop_back();return result;
}
std::string wide_to_utf8(const std::wstring& text) {
    int n=WideCharToMultiByte(CP_UTF8,0,text.c_str(),-1,nullptr,0,nullptr,nullptr);
    if(n<=0)return "";
    std::string result(static_cast<std::size_t>(n),'\0');
    WideCharToMultiByte(CP_UTF8,0,text.c_str(),-1,result.data(),n,nullptr,nullptr);result.pop_back();return result;
}
std::filesystem::path app_folder() {
    PWSTR raw=nullptr;std::filesystem::path path;
    if(SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData,0,nullptr,&raw))) {path=raw;CoTaskMemFree(raw);}
    else path=std::filesystem::temp_directory_path();
    path/=L"Tiaoyin";std::filesystem::create_directories(path);return path;
}
class Notification final:public IMMNotificationClient {
public:
    explicit Notification(HWND window):window_(window){}
    void detach()noexcept{window_=nullptr;}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid,void** p)override {
        if(!p)return E_POINTER;*p=nullptr;
        if(iid==__uuidof(IUnknown)||iid==__uuidof(IMMNotificationClient)){*p=static_cast<IMMNotificationClient*>(this);AddRef();return S_OK;}
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef()override{return static_cast<ULONG>(InterlockedIncrement(&references_));}
    ULONG STDMETHODCALLTYPE Release()override{auto n=InterlockedDecrement(&references_);if(!n)delete this;return static_cast<ULONG>(n);}
    HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR,DWORD)override{return post();}
    HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR)override{return post();}
    HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR)override{return post();}
    HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow,ERole,LPCWSTR)override{return post();}
    HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR,const PROPERTYKEY)override{return post();}
private:
    HRESULT post(){auto w=window_.load();if(w)PostMessageW(w,devicesChanged,0,0);return S_OK;}
    LONG references_=1;std::atomic<HWND> window_;
};
struct App {
    HWND window=nullptr;HFONT font=nullptr,titleFont=nullptr;HBRUSH brush=CreateSolidBrush(bg);
    UINT dpi=96;int scrollY=0,scrollMax=0;std::map<int,HWND> controls;
    ty::WasapiEngine engine;
    std::vector<ty::Endpoint> inputs,outputs;
    std::filesystem::path folder=app_folder(),settings=folder/L"settings.ini",recordPath;
    std::wstring selectedInput,selectedOutput;
    ComPtr<IMMDeviceEnumerator> notifications;
    Notification* listener=nullptr;
    bool closed=false,failedShown=false,recordFaultShown=false;
    ty::EngineState lastState=ty::EngineState::Stopped;
    std::array<float,3> meter{-60,-60,-60};
    std::uint64_t sessionStart=0;
    ~App(){shutdown();if(font)DeleteObject(font);if(titleFont)DeleteObject(titleFont);DeleteObject(brush);}
    int px(int v)const{return MulDiv(v,static_cast<int>(dpi),96);}
    HWND c(int id)const{auto it=controls.find(id);return it==controls.end()?nullptr:it->second;}
    int index(int id)const{return static_cast<int>(SendMessageW(c(id),CB_GETCURSEL,0,0));}
    bool checked(int id)const{return SendMessageW(c(id),BM_GETCHECK,0,0)==BST_CHECKED;}
    void check(int id,bool v){SendMessageW(c(id),BM_SETCHECK,v?BST_CHECKED:BST_UNCHECKED,0);}
    void text(int id,const std::wstring& value){SetWindowTextW(c(id),value.c_str());}
    int slider(int id)const{return static_cast<int>(SendMessageW(c(id),TBM_GETPOS,0,0));}
    void set_slider(int id,int v){SendMessageW(c(id),TBM_SETPOS,TRUE,v);}
    HWND add(int id,const wchar_t* klass,const wchar_t* label,DWORD style=0,DWORD ex=0) {
        auto w=CreateWindowExW(ex,klass,label,WS_CHILD|WS_VISIBLE|style,0,0,10,10,window,
                              reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),GetModuleHandleW(nullptr),nullptr);
        if(!w)throw std::runtime_error("Cannot create UI control");
        controls[id]=w;SendMessageW(w,WM_SETFONT,reinterpret_cast<WPARAM>(font),TRUE);return w;
    }
    void combo(int id,std::initializer_list<const wchar_t*> items,int choice) {
        add(id,L"COMBOBOX",L"",CBS_DROPDOWNLIST|WS_TABSTOP|WS_VSCROLL);
        for(auto value:items)SendMessageW(c(id),CB_ADDSTRING,0,reinterpret_cast<LPARAM>(value));
        SendMessageW(c(id),CB_SETCURSEL,choice,0);
    }
    void track(int id,int lo,int hi,int pos) {
        add(id,TRACKBAR_CLASSW,L"",TBS_HORZ|TBS_NOTICKS|WS_TABSTOP);
        SendMessageW(c(id),TBM_SETRANGE,TRUE,MAKELPARAM(lo,hi));
        SendMessageW(c(id),TBM_SETPAGESIZE,0,3);set_slider(id,pos);
    }
    void fonts() {
        if(font)DeleteObject(font);if(titleFont)DeleteObject(titleFont);
        font=CreateFontW(-px(14),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Microsoft YaHei UI");
        titleFont=CreateFontW(-px(27),0,0,0,FW_SEMIBOLD,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Microsoft YaHei UI");
        for(const auto& entry:controls)SendMessageW(entry.second,WM_SETFONT,reinterpret_cast<WPARAM>(font),TRUE);
    }
    int setting(const wchar_t* key,int value)const{return static_cast<int>(GetPrivateProfileIntW(L"Audio",key,value,settings.c_str()));}
    std::wstring setting_string(const wchar_t* key)const {
        std::array<wchar_t,2048> buf{};GetPrivateProfileStringW(L"Audio",key,L"",buf.data(),static_cast<DWORD>(buf.size()),settings.c_str());return buf.data();
    }
    void initialize() {
        dpi=GetDpiForWindow(window);fonts();
        const std::pair<int,const wchar_t*> labels[]{
            {LabelIn,L"输入设备 / 麦克风"},{LabelOut,L"输出设备 / 有线耳机"},
            {LabelMode,L"音频模式"},{LabelPeriod,L"目标周期"},{LabelRate,L"独占采样率"},{LabelSafety,L"缓冲余量"},{LabelRoute,L"输入声道路由"},
            {LabelInputGain,L"输入增益"},{LabelOutputGain,L"耳返音量"},{LabelLow,L"低频 · 120 Hz"},{LabelMid,L"中频 · 1.5 kHz"},
            {LabelHigh,L"高频 · 6 kHz"},{LabelGate,L"噪声门阈值"},{LabelMeters,L"实时电平  /  dBFS"},{LabelDiag,L"运行诊断  /  周期不等于端到端延迟"}
        };
        for(const auto& label:labels)add(label.first,L"STATIC",label.second);
        combo(In,{},0);combo(Out,{},0);
        combo(Mode,{L"WASAPI 共享",L"WASAPI 独占"},std::clamp(setting(L"mode",0),0,1));
        combo(Period,{L"1.5 ms · 激进",L"3 ms · 低延迟",L"6 ms · 平衡",L"10 ms · 稳健"},std::clamp(setting(L"period",1),0,3));
        combo(Rate,{L"44,100 Hz",L"48,000 Hz",L"96,000 Hz"},std::clamp(setting(L"rate",1),0,2));
        combo(Safety,{L"1 块 · 紧凑",L"2 块 · 稳健",L"3 块 · 兼容"},std::clamp(setting(L"safety",0),0,2));
        combo(Route,{L"通道 1 → 双耳",L"通道 2 → 双耳",L"前两路立体声",L"前两路混合 → 双耳"},std::clamp(setting(L"route",0),0,3));
        add(Raw,L"BUTTON",L"优先 RAW（共享模式）",BS_AUTOCHECKBOX|WS_TABSTOP);check(Raw,setting(L"raw",1)!=0);
        add(Refresh,L"BUTTON",L"刷新设备",BS_PUSHBUTTON|WS_TABSTOP);
        add(Start,L"BUTTON",L"开始监听  F8",BS_DEFPUSHBUTTON|WS_TABSTOP);
        add(Mute,L"BUTTON",L"静音  F9",BS_AUTOCHECKBOX|WS_TABSTOP);
        add(Bypass,L"BUTTON",L"旁通音效",BS_AUTOCHECKBOX|WS_TABSTOP);
        add(Record,L"BUTTON",L"录制耳返 WAV",BS_PUSHBUTTON|WS_TABSTOP);
        add(SaveReport,L"BUTTON",L"导出诊断",BS_PUSHBUTTON|WS_TABSTOP);
        add(Help,L"BUTTON",L"使用帮助",BS_PUSHBUTTON|WS_TABSTOP);
        add(Confirm,L"BUTTON",L"已连接有线耳机并降低系统音量；不使用外放，避免啸叫。",BS_AUTOCHECKBOX|WS_TABSTOP);
        add(Highpass,L"BUTTON",L"80 Hz 低切",BS_AUTOCHECKBOX|WS_TABSTOP);check(Highpass,setting(L"highpass",0)!=0);
        add(Gate,L"BUTTON",L"噪声门",BS_AUTOCHECKBOX|WS_TABSTOP);check(Gate,setting(L"gate",0)!=0);
        add(Compressor,L"BUTTON",L"人声压缩（3:1）",BS_AUTOCHECKBOX|WS_TABSTOP);check(Compressor,setting(L"compressor",0)!=0);
        add(Defaults,L"BUTTON",L"恢复干声",BS_PUSHBUTTON|WS_TABSTOP);
        track(InputGain,-24,24,std::clamp(setting(L"inputGain",0),-24,24));
        track(OutputGain,-60,0,std::clamp(setting(L"outputGain",-18),-60,-12));
        track(Low,-12,12,std::clamp(setting(L"low",0),-12,12));
        track(Mid,-12,12,std::clamp(setting(L"mid",0),-12,12));
        track(High,-12,12,std::clamp(setting(L"high",0),-12,12));
        track(GateLevel,-80,-10,std::clamp(setting(L"gateLevel",-50),-80,-10));
        for(int id:{InputValue,OutputValue,LowValue,MidValue,HighValue,GateValue})add(id,L"STATIC",L"",SS_RIGHT);
        add(Status,L"STATIC",L"未启动 · 本地离线",SS_RIGHT);
        add(Notice,L"STATIC",L"启动前先连接耳机。Esc 可紧急停止；不会自动切换到扬声器。");
        add(Details,L"EDIT",L"启动后显示实际采样率、驱动周期、队列和异常计数。\r\n输入、输出可来自不同设备；优先使用同一声卡的有线输入和耳机接口。",ES_READONLY|ES_MULTILINE|ES_AUTOVSCROLL|WS_VSCROLL,WS_EX_CLIENTEDGE);
        for(int id:{MeterIn,MeterL,MeterR})add(id,L"STATIC",L"",SS_OWNERDRAW);
        selectedInput=setting_string(L"inputId");selectedOutput=setting_string(L"outputId");
        refresh();sync_parameters();enable_controls();layout();
        if(SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator),nullptr,CLSCTX_ALL,IID_PPV_ARGS(&notifications)))) {
            listener=new Notification(window);
            if(FAILED(notifications->RegisterEndpointNotificationCallback(listener))){listener->Release();listener=nullptr;}
        }
        SetTimer(window,1,100,nullptr);
    }
    void place(int id,int x,int y,int w,int h){MoveWindow(c(id),px(x),px(y-scrollY),px(w),px(h),TRUE);}
    void layout() {
        RECT r;GetClientRect(window,&r);const int width=MulDiv(r.right,96,static_cast<int>(dpi));
        const int visibleHeight=MulDiv(r.bottom,96,static_cast<int>(dpi));
        const int height=std::max(visibleHeight,770);scrollMax=height-visibleHeight;
        scrollY=std::clamp(scrollY,0,scrollMax);
        SCROLLINFO info{sizeof(info),SIF_RANGE|SIF_PAGE|SIF_POS,0,height-1,static_cast<UINT>(visibleHeight),scrollY,0};
        SetScrollInfo(window,SB_VERT,&info,TRUE);
        const int margin=24,usable=width-2*margin,half=(usable-20)/2;
        place(Status,width-300,22,276,26);
        place(LabelIn,24,80,half,22);place(LabelOut,44+half,80,half,22);
        place(In,24,105,half,280);place(Out,44+half,105,half,280);
        const int col=(usable-4*14)/5;
        const int labelIds[]{LabelMode,LabelPeriod,LabelRate,LabelSafety,LabelRoute};
        const int comboIds[]{Mode,Period,Rate,Safety,Route};
        for(int i=0;i<5;++i){place(labelIds[i],24+i*(col+14),145,col,22);place(comboIds[i],24+i*(col+14),170,col,200);}
        place(Raw,24,209,250,26);place(Confirm,280,209,usable-256,26);
        place(Start,24,247,170,35);place(Mute,216,250,118,30);place(Bypass,340,250,124,30);
        place(Record,478,247,152,35);place(Refresh,646,247,116,35);place(Help,778,247,usable-754,35);
        place(LabelInputGain,24,306,150,22);place(InputValue,24+half-92,306,92,22);
        place(InputGain,18,332,half+12,32);
        place(LabelOutputGain,44+half,306,150,22);place(OutputValue,44+2*half-92,306,92,22);
        place(OutputGain,38+half,332,half+12,32);
        const int q=(usable-3*18)/4;
        const int toneLabels[]{LabelLow,LabelMid,LabelHigh,LabelGate};
        const int toneTracks[]{Low,Mid,High,GateLevel};const int toneValues[]{LowValue,MidValue,HighValue,GateValue};
        for(int i=0;i<4;++i){const int x=24+i*(q+18);place(toneLabels[i],x,381,q,22);place(toneValues[i],x+q-66,409,66,24);place(toneTracks[i],x-6,407,q-65,30);}
        place(Highpass,24,452,150,26);place(Gate,192,452,120,26);place(Compressor,322,452,205,26);place(Defaults,550,448,125,32);
        place(LabelMeters,24,502,usable,22);
        const int meterWidth=(usable-20)/3;place(MeterIn,24,531,meterWidth,29);place(MeterL,34+meterWidth,531,meterWidth,29);place(MeterR,44+2*meterWidth,531,meterWidth,29);
        place(LabelDiag,24,580,usable,22);place(Details,24,607,usable,std::max(70,height-660));
        place(Notice,24,height-36,usable-142,26);place(SaveReport,width-152,height-41,128,30);
        InvalidateRect(window,nullptr,TRUE);
    }
    void fill_endpoints(int id,const std::vector<ty::Endpoint>& endpoints,const std::wstring& saved) {
        SendMessageW(c(id),CB_RESETCONTENT,0,0);int chosen=-1;
        for(std::size_t i=0;i<endpoints.size();++i) {
            const auto label=endpoints[i].name+(endpoints[i].isDefault?L"  [系统默认]":L"");
            SendMessageW(c(id),CB_ADDSTRING,0,reinterpret_cast<LPARAM>(label.c_str()));
            if(endpoints[i].id==saved||(saved.empty()&&endpoints[i].isDefault))chosen=static_cast<int>(i);
        }
        SendMessageW(c(id),CB_SETCURSEL,chosen,0);
    }
    void refresh() {
        try{inputs=ty::enumerate_endpoints(true);outputs=ty::enumerate_endpoints(false);
            fill_endpoints(In,inputs,selectedInput);fill_endpoints(Out,outputs,selectedOutput);
            if(inputs.empty()||outputs.empty())text(Notice,L"未检测到完整输入/输出。连接设备后刷新；检查麦克风权限。");
            else if(index(In)<0||index(Out)<0)text(Notice,L"之前的设备已不在列表中，请手动选择；不会自动改用其他设备。");
            else text(Notice,L"设备就绪。先以低音量试听；Esc 紧急停止，F9 静音。");
        }catch(...){text(Notice,L"设备枚举失败。检查 Windows Audio 服务，再点击刷新。");}
        remember_selection();
    }
    void remember_selection(){
        const int a=index(In),b=index(Out);
        if(a>=0&&a<static_cast<int>(inputs.size()))selectedInput=inputs[static_cast<std::size_t>(a)].id;
        if(b>=0&&b<static_cast<int>(outputs.size()))selectedOutput=outputs[static_cast<std::size_t>(b)].id;
    }
    void sync_parameters() {
        auto& p=engine.parameters;
        p.inputDb=static_cast<float>(slider(InputGain));p.outputDb=static_cast<float>(slider(OutputGain));
        p.lowDb=static_cast<float>(slider(Low));p.midDb=static_cast<float>(slider(Mid));p.highDb=static_cast<float>(slider(High));p.gateDb=static_cast<float>(slider(GateLevel));
        p.muted=checked(Mute);p.bypass=checked(Bypass);p.highpass=checked(Highpass);p.gate=checked(Gate);p.compressor=checked(Compressor);p.route=std::max(index(Route),0);
        const std::pair<int,int> values[]{{InputValue,InputGain},{OutputValue,OutputGain},{LowValue,Low},{MidValue,Mid},{HighValue,High},{GateValue,GateLevel}};
        for(const auto& v:values)text(v.first,std::to_wstring(slider(v.second))+(v.first==GateValue?L" dBFS":L" dB"));
    }
    void enable_controls() {
        const auto state=engine.state();const bool busy=state==ty::EngineState::Starting||state==ty::EngineState::Running;
        for(int id:{In,Out,Mode,Period,Safety,Route,Refresh})EnableWindow(c(id),!busy);
        EnableWindow(c(Rate),!busy&&index(Mode)==1);EnableWindow(c(Raw),!busy&&index(Mode)==0);
        EnableWindow(c(Record),state==ty::EngineState::Running||engine.recorder.active());
        text(Start,busy?L"停止监听  F8":L"开始监听  F8");
    }
    void start_stop() {
        if(engine.state()==ty::EngineState::Running||engine.state()==ty::EngineState::Starting){stop();return;}
        if(index(In)<0||index(Out)<0){MessageBoxW(window,L"请选择实际存在的本地输入和输出设备。",L"设备选择",MB_OK|MB_ICONINFORMATION);return;}
        if(!checked(Confirm)){MessageBoxW(window,L"请先接好有线耳机，降低系统及耳机音量，再勾选确认。外放会形成声反馈，限幅不等于防啸叫。",L"监听前检查",MB_OK|MB_ICONWARNING);return;}
        remember_selection();sync_parameters();ty::StreamConfig config;
        config.inputId=selectedInput;config.outputId=selectedOutput;config.exclusive=index(Mode)==1;config.raw=checked(Raw);
        const unsigned rates[]{44100,48000,96000};const double periods[]{1.5,3,6,10};
        config.exclusiveRate=rates[std::clamp(index(Rate),0,2)];config.periodMs=periods[std::clamp(index(Period),0,3)];config.safetyBlocks=static_cast<unsigned>(std::clamp(index(Safety),0,2)+1);
        failedShown=recordFaultShown=false;
        try{engine.start(std::move(config));sessionStart=GetTickCount64();text(Status,L"正在初始化音频设备…");text(Notice,L"正在协商实际格式和周期；独占失败不会自动降级。");}
        catch(const std::exception& e){MessageBoxW(window,utf8_to_wide(e.what()).c_str(),L"启动失败",MB_OK|MB_ICONERROR);}
        enable_controls();
    }
    void stop(){engine.stop();text(Status,L"已停止 · 本地离线");text(Record,L"录制耳返 WAV");enable_controls();}
    void emergency_stop(){check(Mute,true);engine.parameters.muted=true;stop();text(Notice,L"已紧急停止并保持静音。重新监听前请检查连接和音量。");}
    std::filesystem::path choose_file(const wchar_t* filter,const wchar_t* extension,const std::wstring& initial) {
        std::array<wchar_t,32768> buffer{};wcsncpy_s(buffer.data(),buffer.size(),initial.c_str(),_TRUNCATE);
        OPENFILENAMEW dialog{};dialog.lStructSize=sizeof(dialog);dialog.hwndOwner=window;dialog.lpstrFilter=filter;
        dialog.lpstrFile=buffer.data();dialog.nMaxFile=static_cast<DWORD>(buffer.size());dialog.lpstrDefExt=extension;
        dialog.Flags=OFN_OVERWRITEPROMPT|OFN_NOCHANGEDIR|OFN_PATHMUSTEXIST;
        return GetSaveFileNameW(&dialog)?std::filesystem::path(buffer.data()):std::filesystem::path{};
    }
    void record() {
        if(engine.recorder.active()){engine.recorder.stop();text(Record,L"录制耳返 WAV");text(Notice,L"已保存 WAV（监听仍在运行）。");return;}
        if(engine.state()!=ty::EngineState::Running)return;
        auto directory=folder/L"Recordings";std::filesystem::create_directories(directory);
        SYSTEMTIME t;GetLocalTime(&t);wchar_t name[96];swprintf_s(name,L"Tiaoyin-%04u%02u%02u-%02u%02u%02u.wav",t.wYear,t.wMonth,t.wDay,t.wHour,t.wMinute,t.wSecond);
        auto path=choose_file(L"PCM WAV (*.wav)\0*.wav\0\0",L"wav",(directory/name).wstring());if(path.empty())return;
        if(engine.state()!=ty::EngineState::Running)return;
        std::string error;recordFaultShown=false;
        if(engine.recorder.start(path,engine.stats.outputRate.load(),error)){recordPath=path;text(Record,L"停止录音");text(Notice,L"正在录制处理后的耳返；静音也会被录入。音频不上传。");}
        else MessageBoxW(window,utf8_to_wide(error).c_str(),L"录音失败",MB_OK|MB_ICONERROR);
    }
    std::wstring diagnostics()const {
        const auto& s=engine.stats;std::wostringstream o;o<<std::fixed<<std::setprecision(2);
        o<<L"格式：输入 "<<s.inputRate<<L" Hz / "<<s.inputChannels<<L" ch  →  输出 "<<s.outputRate<<L" Hz / "<<s.outputChannels<<L" ch\r\n";
        o<<L"实际周期：输入 "<<s.inputPeriod<<L" 帧（"<<(s.inputRate?1000.0*s.inputPeriod/s.inputRate:0)<<L" ms），输出 "<<s.outputPeriod<<L" 帧（"<<(s.outputRate?1000.0*s.outputPeriod/s.outputRate:0)<<L" ms）；容量 "<<s.inputBuffer<<L" / "<<s.outputBuffer<<L" 帧\r\n";
        o<<L"驱动报告延迟 "<<s.inputLatencyMs<<L" + "<<s.outputLatencyMs<<L" ms；软件排队约 "<<s.queueMs<<L" ms；以上均非端到端实测\r\n";
        o<<L"队列欠载 "<<s.underruns<<L"；重同步 "<<s.resyncs<<L"；丢弃帧 "<<s.discarded<<L"；采集不连续 "<<s.discontinuities<<L"；输入削波 "<<s.clipped<<L"；限幅采样 "<<s.limited<<L"\r\n";
        o<<L"时钟补偿 "<<s.driftPpm<<L" ppm；处理负荷约 "<<s.loadPercent<<L"%；MMCSS "<<(s.mmcss?L"已启用":L"不可用")<<L"\r\n";
        o<<L"低周期路径 入/出："<<(s.inputLowLatency?L"启用":L"传统共享回退")<<L" / "<<(s.outputLowLatency?L"启用":L"传统共享回退")
         <<L"；RAW 入/出："<<(s.inputRaw?L"是":L"否")<<L" / "<<(s.outputRaw?L"是":L"否")<<L"\r\n";
        o<<L"录音 "<<engine.recorder.frames_written()<<L" 帧；录音故障码 "<<engine.recorder.fault()<<L"。设备拔出不会自动恢复或转向扬声器。";
        return o.str();
    }
    void report() {
        auto path=choose_file(L"Text report (*.txt)\0*.txt\0\0",L"txt",(folder/L"tiaoyin-diagnostics.txt").wstring());if(path.empty())return;
        std::ofstream out(path,std::ios::binary);out<<"\xEF\xBB\xBF";
        out<<"Tiaoyin 0.1.0 diagnostics\nNo hardware latency measurement has been performed by this report.\n";
        out<<"Input endpoint: "<<wide_to_utf8(selectedInput)<<"\nOutput endpoint: "<<wide_to_utf8(selectedOutput)<<"\n";
        out<<wide_to_utf8(diagnostics())<<"\nLast error: "<<wide_to_utf8(engine.last_error())<<"\n";out.flush();
        if(!out)MessageBoxW(window,L"诊断文件写入失败。",L"导出失败",MB_OK|MB_ICONERROR);
        else text(Notice,L"诊断已导出；包含所选设备 ID，分享前可自行删去。");
    }
    void tick() {
        const auto state=engine.state();
        if(state!=lastState){enable_controls();lastState=state;
            if(state==ty::EngineState::Running)text(Notice,L"监听已启动。逐渐调整耳返音量；Esc 停止，F9 静音。设备切换需先停止。");
        }
        if(state==ty::EngineState::Failed&&!failedShown) {
            failedShown=true;const auto message=engine.last_error();text(Details,message);stop();text(Status,L"设备错误 · 已停止");
            text(Notice,L"请检查设备/权限/占用；不会自动切换设备。完整原因见诊断。");return;
        }
        if(engine.recorder.fault()&&!recordFaultShown) {
            recordFaultShown=true;engine.recorder.stop();text(Record,L"录制耳返 WAV");
            text(Notice,L"录音因缓冲溢出/磁盘错误/4GB 限制停止，监听不受影响。请查看诊断。");
        }
        const float values[]{engine.stats.inputPeak.exchange(0),engine.stats.outputPeakL.exchange(0),engine.stats.outputPeakR.exchange(0)};
        for(std::size_t i=0;i<3;++i)meter[i]=std::max(-60.0f,std::max(ty::gain_to_db(values[i]),meter[i]-3));
        for(int id:{MeterIn,MeterL,MeterR})InvalidateRect(c(id),nullptr,FALSE);
        if(state==ty::EngineState::Running) {
            text(Status,checked(Mute)?L"监听运行中 · 已静音":L"监听运行中 · 本地离线");
            const auto first=SendMessageW(c(Details),EM_GETFIRSTVISIBLELINE,0,0);
            text(Details,diagnostics());SendMessageW(c(Details),EM_LINESCROLL,0,first);
            if(!engine.stats.inputLowLatency||!engine.stats.outputLowLatency)text(Notice,L"低周期协商未成功，已使用传统共享路径；请查看实际周期。");
            else if(engine.stats.underruns>0)text(Notice,L"检测到队列欠载。停止后提高目标周期或缓冲余量，再试听。");
        }
    }
    void save_settings() {
        remember_selection();
        if(!std::filesystem::exists(settings)){std::ofstream f(settings,std::ios::binary);f.write("\xFF\xFE",2);}
        const auto put=[this](const wchar_t* key,const std::wstring& value){WritePrivateProfileStringW(L"Audio",key,value.c_str(),settings.c_str());};
        put(L"inputId",selectedInput);put(L"outputId",selectedOutput);
        const std::pair<const wchar_t*,int> values[]{
            {L"mode",index(Mode)},{L"period",index(Period)},{L"rate",index(Rate)},{L"safety",index(Safety)},{L"route",index(Route)},
            {L"raw",checked(Raw)},{L"inputGain",slider(InputGain)},{L"outputGain",slider(OutputGain)},
            {L"low",slider(Low)},{L"mid",slider(Mid)},{L"high",slider(High)},{L"gateLevel",slider(GateLevel)},
            {L"highpass",checked(Highpass)},{L"gate",checked(Gate)},{L"compressor",checked(Compressor)}
        };
        for(const auto& v:values)put(v.first,std::to_wstring(v.second));
    }
    void shutdown() {
        if(closed)return;closed=true;
        if(window)KillTimer(window,1);engine.stop();
        if(listener){listener->detach();notifications->UnregisterEndpointNotificationCallback(listener);listener->Release();listener=nullptr;}
        notifications.Reset();
    }
    void help() {
        MessageBoxW(window,
            L"1. 接好有线耳机，先降低系统与耳机音量。选择本地输入、输出并勾选耳机确认。\n"
            L"2. 先用共享模式 / 3 ms / 1 块；开始后逐渐调耳返音量。设备协商结果以诊断为准。\n"
            L"3. 出现爆音/欠载：停止，改 6 ms 或 2 块。稳定后才尝试 1.5 ms 或独占模式。\n"
            L"4. 共享模式可与伴奏软件共用设备；独占模式可能占用设备，其他软件无法发声。\n"
            L"5. 旁通关闭低切、噪声门、压缩和 EQ；输入增益、耳返音量、静音和安全限幅仍有效。\n"
            L"6. 录音保存处理后的双声道 PCM16 WAV；静音同步录入。磁盘写入在独立线程。\n\n"
            L"F8 开始/停止；F9 静音；Esc 紧急停止。断开设备后不会自动切到扬声器。\n"
            L"关闭 Windows『侦听此设备』和声卡硬件直通，避免双重耳返。不要使用蓝牙评估低延迟。\n\n"
            L"软件限幅并不能保证声压安全或防止啸叫。请勿将大功率扬声器输出接入麦克风。\n"
            L"本版本不包含 ASIO/VST 插件宿主、变声或商业软件素材。具体低延迟验收见 docs。",
            L"Tiaoyin 使用帮助",MB_OK|MB_ICONINFORMATION);
    }
    void draw_meter(const DRAWITEMSTRUCT& d) {
        const int i=d.CtlID==MeterIn?0:d.CtlID==MeterL?1:2;RECT rect=d.rcItem;
        HBRUSH base=CreateSolidBrush(RGB(226,233,241));FillRect(d.hDC,&rect,base);DeleteObject(base);
        RECT bar=rect;bar.right=bar.left+static_cast<LONG>((bar.right-bar.left)*std::clamp((meter[static_cast<std::size_t>(i)]+60)/60,0.0f,1.0f));
        const auto color=meter[static_cast<std::size_t>(i)]>-1?RGB(222,83,71):meter[static_cast<std::size_t>(i)]>-6?RGB(221,164,45):RGB(78,178,155);
        HBRUSH fill=CreateSolidBrush(color);FillRect(d.hDC,&bar,fill);DeleteObject(fill);
        SetBkMode(d.hDC,TRANSPARENT);SetTextColor(d.hDC,ink);SelectObject(d.hDC,font);
        wchar_t value[64];swprintf_s(value,L"%s    %.1f dBFS",i==0?L"输入":i==1?L"耳返 L":L"耳返 R",meter[static_cast<std::size_t>(i)]);
        DrawTextW(d.hDC,value,-1,&rect,DT_CENTER|DT_VCENTER|DT_SINGLELINE);
    }
};
LRESULT CALLBACK procedure(HWND window,UINT message,WPARAM w,LPARAM l) {
    App* app=reinterpret_cast<App*>(GetWindowLongPtrW(window,GWLP_USERDATA));
    if(message==WM_NCCREATE) {app=static_cast<App*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams);app->window=window;SetWindowLongPtrW(window,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(app));}
    if(!app)return DefWindowProcW(window,message,w,l);
    try {
        switch(message) {
        case WM_CREATE:app->initialize();return 0;
        case WM_SIZE:if(app->font)app->layout();return 0;
        case WM_GETMINMAXINFO:{auto* m=reinterpret_cast<MINMAXINFO*>(l);m->ptMinTrackSize={app->px(980),app->px(520)};return 0;}
        case WM_DPICHANGED:{app->dpi=HIWORD(w);app->fonts();auto* r=reinterpret_cast<RECT*>(l);SetWindowPos(window,nullptr,r->left,r->top,r->right-r->left,r->bottom-r->top,SWP_NOZORDER|SWP_NOACTIVATE);app->layout();return 0;}
        case WM_ERASEBKGND:{RECT r;GetClientRect(window,&r);FillRect(reinterpret_cast<HDC>(w),&r,app->brush);return 1;}
        case WM_PAINT:{PAINTSTRUCT ps;HDC dc=BeginPaint(window,&ps);SetBkMode(dc,TRANSPARENT);SetTextColor(dc,ink);SelectObject(dc,app->titleFont);
            RECT title{app->px(24),app->px(16-app->scrollY),app->px(410),app->px(55-app->scrollY)};DrawTextW(dc,L"调音  Tiaoyin",-1,&title,DT_LEFT|DT_SINGLELINE|DT_VCENTER);
            HPEN pen=CreatePen(PS_SOLID,1,RGB(218,226,236));auto old=SelectObject(dc,pen);RECT client;GetClientRect(window,&client);
            for(int y:{65,295,373,491,570}){MoveToEx(dc,app->px(24),app->px(y-app->scrollY),nullptr);LineTo(dc,client.right-app->px(24),app->px(y-app->scrollY));}
            SelectObject(dc,old);DeleteObject(pen);EndPaint(window,&ps);return 0;}
        case WM_CTLCOLORSTATIC:{HDC dc=reinterpret_cast<HDC>(w);SetBkColor(dc,bg);SetTextColor(dc,reinterpret_cast<HWND>(l)==app->c(Status)?accent:mutedInk);return reinterpret_cast<LRESULT>(app->brush);}
        case WM_DRAWITEM:{auto* item=reinterpret_cast<DRAWITEMSTRUCT*>(l);if(item->CtlType==ODT_STATIC){app->draw_meter(*item);return TRUE;}break;}
        case WM_VSCROLL:{
            const int action=LOWORD(w);SCROLLINFO info{sizeof(info),SIF_TRACKPOS};GetScrollInfo(window,SB_VERT,&info);
            if(action==SB_LINEUP)app->scrollY-=30;else if(action==SB_LINEDOWN)app->scrollY+=30;
            else if(action==SB_PAGEUP)app->scrollY-=180;else if(action==SB_PAGEDOWN)app->scrollY+=180;
            else if(action==SB_THUMBTRACK||action==SB_THUMBPOSITION)app->scrollY=info.nTrackPos;
            else if(action==SB_TOP)app->scrollY=0;else if(action==SB_BOTTOM)app->scrollY=app->scrollMax;
            app->layout();return 0;}
        case WM_HSCROLL:app->sync_parameters();return 0;
        case WM_TIMER:app->tick();return 0;
        case devicesChanged:app->text(Notice,L"音频设备列表发生变化。停止后刷新；不会自动更换输出。");return 0;
        case WM_COMMAND:{const int id=LOWORD(w),code=HIWORD(w);
            switch(id) {
            case Start:app->start_stop();break;
            case Panic:app->emergency_stop();break;
            case MuteKey:app->check(Mute,!app->checked(Mute));app->sync_parameters();break;
            case Refresh:app->remember_selection();app->refresh();break;
            case Record:app->record();break;
            case SaveReport:app->report();break;
            case Help:app->help();break;
            case Defaults:for(int control:{Low,Mid,High})app->set_slider(control,0);for(int control:{Highpass,Gate,Compressor,Bypass})app->check(control,false);app->sync_parameters();break;
            case Mode:if(code==CBN_SELCHANGE)app->enable_controls();break;
            case In:case Out:if(code==CBN_SELCHANGE)app->remember_selection();break;
            default:app->sync_parameters();break;
            }return 0;}
        case WM_CLOSE:app->save_settings();app->shutdown();DestroyWindow(window);return 0;
        case WM_DESTROY:PostQuitMessage(0);return 0;
        }
    }catch(const std::exception& e){MessageBoxW(window,utf8_to_wide(e.what()).c_str(),L"Tiaoyin",MB_OK|MB_ICONERROR);}
    catch(...){MessageBoxW(window,L"操作失败；请检查设备、路径及权限。",L"Tiaoyin",MB_OK|MB_ICONERROR);}
    return DefWindowProcW(window,message,w,l);
}
} // namespace
int WINAPI wWinMain(HINSTANCE instance,HINSTANCE,PWSTR command,int show) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const HRESULT com=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
    if(FAILED(com))return 1;
    int result=0;
    try {
        if(command&&std::wstring(command).find(L"--smoke-test")!=std::wstring::npos) {
            std::ofstream f("smoke-test.txt");
            try {const auto in=ty::enumerate_endpoints(true),out=ty::enumerate_endpoints(false);
                f<<"WASAPI enumeration OK. inputs="<<in.size()<<" outputs="<<out.size()<<"\n";
            } catch(...) {f<<"Enumeration unavailable on this machine (e.g. headless runner/no audio service).\n";}
            f<<"Process/COM startup completed. No streams started. No hardware latency validated.\n";
        } else {
            INITCOMMONCONTROLSEX common{sizeof(common),ICC_BAR_CLASSES|ICC_STANDARD_CLASSES};InitCommonControlsEx(&common);
            App app;WNDCLASSEXW klass{};klass.cbSize=sizeof(klass);klass.lpfnWndProc=procedure;klass.hInstance=instance;
            klass.hCursor=LoadCursorW(nullptr,IDC_ARROW);klass.hIcon=LoadIconW(nullptr,IDI_APPLICATION);klass.lpszClassName=L"TiaoyinDesktop";
            RegisterClassExW(&klass);
            const UINT dpi=GetDpiForSystem();RECT work{};SystemParametersInfoW(SPI_GETWORKAREA,0,&work,0);
            auto window=CreateWindowExW(0,klass.lpszClassName,L"Tiaoyin · 本地实时耳返",WS_OVERLAPPEDWINDOW|WS_CLIPCHILDREN|WS_VSCROLL,
                CW_USEDEFAULT,CW_USEDEFAULT,MulDiv(1020,static_cast<int>(dpi),96),std::min(MulDiv(880,static_cast<int>(dpi),96),static_cast<int>(work.bottom-work.top)),nullptr,nullptr,instance,&app);
            if(!window)throw std::runtime_error("Cannot create application window");
            ShowWindow(window,show);UpdateWindow(window);
            ACCEL keys[]{{FVIRTKEY,VK_F8,Start},{FVIRTKEY,VK_F9,MuteKey},{FVIRTKEY,VK_ESCAPE,Panic}};
            auto accelerators=CreateAcceleratorTableW(keys,3);MSG message{};BOOL status;
            while((status=GetMessageW(&message,nullptr,0,0))>0)
                if(!TranslateAcceleratorW(window,accelerators,&message)&&!IsDialogMessageW(window,&message)){TranslateMessage(&message);DispatchMessageW(&message);}
            DestroyAcceleratorTable(accelerators);result=status<0?1:static_cast<int>(message.wParam);
        }
    }catch(const std::exception& e){MessageBoxW(nullptr,utf8_to_wide(e.what()).c_str(),L"Tiaoyin 启动失败",MB_OK|MB_ICONERROR);result=1;}
    catch(...){result=1;}
    CoUninitialize();return result;
}
