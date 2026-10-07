#include "audio_core.h"
#include "recorder.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace ty;
namespace {
int assertions=0;
void require(bool condition,const char* what){++assertions;if(!condition)throw std::runtime_error(what);}
void near(double a,double b,double eps,const char* what){require(std::abs(a-b)<=eps,what);}
void pcm_test(){
    for(unsigned bits:{8u,16u,24u,32u}) for(float x:{-1.f,-.5f,0.f,.5f,1.f}) {
        PcmFormat f{48000,2,bits,bits,false};std::array<std::uint8_t,8> b{};
        write_pcm(b.data()+1,x,f);
    near(read_pcm(b.data()+1,f),x,bits==8?.008:.00004,"PCM roundtrip");
    }
    PcmFormat padded{48000,2,32,24,false};std::array<std::uint8_t,4>b{};
    write_pcm(b.data(),-.5f,padded);
    near(read_pcm(b.data(),padded),-.5,1e-7,"24-in-32 PCM");
    require(b[0]==0,"PCM valid bit alignment");
    PcmFormat f;write_pcm(b.data(),std::numeric_limits<float>::quiet_NaN(),f);
    near(read_pcm(b.data(),f),0,0,"NaN sanitized");
    require(!PcmFormat{48000,2,64,64,true}.supported(),"Reject unsupported float64");
}
void dsp_test(){
    VoiceProcessor dsp;dsp.reset(48000);Parameters p;dsp.configure(p);
    for(int i=0;i<1000;++i){auto f=dsp.process({.25f,-.25f});
    near(f.l,.25,1e-6,"Flat DSP");
    near(f.r,-.25,1e-6,"Stereo preserved");}
    p.highpass=true;dsp.configure(p);Frame f;for(int i=0;i<48000;++i)f=dsp.process({.25f,.25f});
    require(std::abs(f.l)<.00001,"Highpass rejects DC");
    dsp.reset(48000);p=Parameters{};p.gate=true;dsp.configure(p);
    for(int i=0;i<48000;++i)f=dsp.process({.0001f,.0001f});
    require(std::abs(f.l)<1e-7,"Gate closes smoothly");
    for(int i=0;i<24000;++i)f=dsp.process({.3f,.3f});
    require(f.l>.29f,"Gate reopens");
    dsp.reset(48000);p=Parameters{};p.compressor=true;dsp.configure(p);
    for(int i=0;i<48000;++i)f=dsp.process({.8f,.8f});
    require(f.l<.4f&&f.l>.15f,"Compressor gain reduction");
    p.lowDb=12;p.midDb=-12;p.highDb=12;dsp.configure(p);
    for(int i=0;i<48000;++i){f=dsp.process({float(std::sin(i*.04)),float(std::cos(i*.07))});
    require(std::isfinite(f.l)&&std::isfinite(f.r),"EQ remains finite");}
    MonitorGain gain;gain.reset(48000);p.outputDb=0;gain.configure(p);
    for(int i=0;i<5000;++i){f=gain.process({2,-2});
    require(std::abs(f.l)<=.95f&&std::abs(f.r)<=.95f,"Output ceiling");}
    require(gain.limited_samples()>0,"Limiter telemetry");p.muted=true;gain.configure(p);
    for(int i=0;i<10000;++i)f=gain.process({1,1});
    near(f.l,0,0,"Mute settles to exact zero");
}
void fifo_test(){
    SpscFrames q(4);Frame f;
    for(int i=0;i<4;++i)require(q.push({float(i),0}),"FIFO push");
    require(!q.push({}),"FIFO bounded");
    for(int i=0;i<4;++i){require(q.pop(f),"FIFO pop");
    near(f.l,i,0,"FIFO order");}require(!q.pop(f),"FIFO empty");
    SpscFrames threaded(257);constexpr int count=100000;std::atomic<bool> ok{true};
    std::thread producer([&]{for(int i=0;i<count;++i)while(!threaded.push({float(i),0}))std::this_thread::yield();});
    std::thread consumer([&]{for(int i=0;i<count;++i){Frame a;while(!threaded.pop(a))std::this_thread::yield();if(a.l!=float(i))ok=false;}});
    producer.join();consumer.join();require(ok,"Concurrent FIFO order");
}
void bridge_test(double ppm,unsigned inRate,unsigned outRate,double seconds,std::size_t block=128,bool compact=false){
    AudioBridge bridge;
    bridge.reset(inRate,outRate,static_cast<std::size_t>(std::ceil(block*double(inRate)/outRate)),block,1,compact?inRate/500:0);
    std::vector<Frame> out(block);double fraction=0;std::uint64_t sample=0;double energy=0;
    const auto begin=std::chrono::steady_clock::now();
    for(unsigned k=0;k<unsigned(seconds*outRate/block);++k){
        fraction+=block*double(inRate)/outRate*(1+ppm*1e-6);
        const auto n=static_cast<unsigned>(fraction);fraction-=n;
        for(unsigned i=0;i<n;++i){float s=float(.2*std::sin(2*pi*1000*sample++/inRate));bridge.push({s,-s});}
        bridge.render(out.data(),out.size());
        if(k>20)for(const auto& f:out){require(std::isfinite(f.l),"ASRC finite");
    near(f.l,-f.r,1e-6,"ASRC stereo phase");energy+=f.l*f.l;}
        require(bridge.queued_frames()<10000,"Queue cannot grow unbounded");
    }
    require(bridge.underruns()==0,"ASRC steady stream no underruns");
    require(bridge.resyncs()==0,"ASRC normal drift no hard resync");
    require(energy>100,"ASRC signal present");
    if(compact) require(bridge.queued_frames()<inRate/200,"Compact queue stays below 5 ms");
    if(seconds>=30)near(bridge.correction_ppm(),ppm,120,"ASRC tracks clock drift");
    std::cout<<"  ASRC "<<inRate<<" -> "<<outRate<<", "<<ppm<<" ppm: queue="<<bridge.queued_frames()<<", correction="<<bridge.correction_ppm()<<", runtime="<<std::chrono::duration<double>(std::chrono::steady_clock::now()-begin).count()<<" s\n";
    for(int i=0;i<200;++i)bridge.render(out.data(),out.size());
    require(bridge.underruns()>0,"Starvation detected");
    for(const auto& f:out)near(f.l,0,0,"Starvation zero-fills");
    for(int i=0;i<30000;++i)bridge.push({.1f,.1f});bridge.render(out.data(),out.size());
    require(bridge.discarded_frames()>0,"Overrun bounded and visible");
}
void recording_test(){
    const auto path=std::filesystem::temp_directory_path()/"tiaoyin-unit-test.wav";
    Recorder recorder;std::string err;
    require(recorder.start(path,48000,err),"Recorder opens");
    std::vector<Frame> f(1000,Frame{.25f,-.25f});recorder.push(f.data(),f.size());recorder.stop();
    require(recorder.fault()==0,"Recorder no fault");
    require(recorder.frames_written()==1000,"Recorder flushes on stop");
    require(std::filesystem::file_size(path)==4044,"WAV size/header");std::ifstream in(path,std::ios::binary);char h[44];in.read(h,44);
    require(std::string(h,4)=="RIFF"&&std::string(h+8,4)=="WAVE","WAV signature");in.close();std::filesystem::remove(path);
    require(!recorder.start(path,0,err),"Reject invalid recording rate");
    for(int i=0;i<3;++i){require(recorder.start(path,44100,err),"Recorder restarts");recorder.push(f.data(),f.size());recorder.stop();}
    std::filesystem::remove(path);
}
}
int main(){try{
    pcm_test();std::cout<<"PASS PCM conversion\n";dsp_test();std::cout<<"PASS DSP/gain/safety\n";
    fifo_test();std::cout<<"PASS bounded concurrent FIFO\n";
    bridge_test(500,48000,48000,40);bridge_test(-500,48000,48000,40);
    bridge_test(300,44100,48000,40);bridge_test(-300,48000,44100,40);
    bridge_test(500,48000,48000,120,480,true);bridge_test(-500,48000,48000,120,480,true);
    bridge_test(300,44100,48000,120,480,true);bridge_test(-300,48000,44100,120,441,true);
    std::cout<<"PASS ASRC/rates/drift/starvation/overrun\n";recording_test();std::cout<<"PASS WAV recording\n";
    std::cout<<"ALL TESTS PASSED ("<<assertions<<" checks)\n";return 0;
}catch(const std::exception& e){std::cerr<<"FAIL: "<<e.what()<<"\n";return 1;}}
