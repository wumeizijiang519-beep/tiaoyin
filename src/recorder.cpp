#include "recorder.h"
#include <array>
#include <chrono>

namespace ty {
Recorder::Recorder():queue_(262144){} // ~5.46 seconds at 48 kHz, fixed memory
Recorder::~Recorder(){stop();}
void Recorder::header(std::uint32_t bytes) {
    const auto u16=[this](std::uint16_t v){char b[2]{static_cast<char>(v),static_cast<char>(v>>8)};file_.write(b,2);};
    const auto u32=[this](std::uint32_t v){char b[4]{static_cast<char>(v),static_cast<char>(v>>8),static_cast<char>(v>>16),static_cast<char>(v>>24)};file_.write(b,4);};
    file_.seekp(0); file_.write("RIFF",4); u32(bytes+36); file_.write("WAVEfmt ",8);
    u32(16); u16(1); u16(2); u32(rate_); u32(rate_*4); u16(4); u16(16);
    file_.write("data",4); u32(bytes);
}
bool Recorder::start(const std::filesystem::path& path,unsigned rate,std::string& error) {
    stop(); fault_=0; written_=0; queue_.reset_quiescent(); rate_=rate;
    if(rate<8000||rate>192000){error="Invalid recording sample rate";return false;}
    file_.clear(); file_.open(path,std::ios::binary|std::ios::trunc);
    if(!file_){error="Cannot create WAV file (check folder permissions/free space)";return false;}
    header(0);
    if(!file_){file_.close();error="Cannot write WAV header";return false;}
    enabled_.store(true,std::memory_order_release);
    try{thread_=std::thread(&Recorder::run,this);}
    catch(const std::exception& e){enabled_=false;file_.close();error=e.what();return false;}
    return true;
}
void Recorder::push(const Frame* frames,std::size_t count) noexcept {
    producers_.fetch_add(1,std::memory_order_acq_rel);
    if(enabled_.load(std::memory_order_acquire)) {
        for(std::size_t i=0;i<count;++i) if(!queue_.push(frames[i])) {
            fault_=1; enabled_.store(false,std::memory_order_release); break;
        }
    }
    producers_.fetch_sub(1,std::memory_order_release);
}
void Recorder::stop() {
    enabled_.store(false,std::memory_order_release);
    if(thread_.joinable()) thread_.join();
    while(producers_.load(std::memory_order_acquire)!=0) std::this_thread::yield();
    if(file_.is_open()) file_.close();
}
void Recorder::run() noexcept {
    std::array<std::uint8_t,4096*4> bytes{};
    const PcmFormat fmt{rate_,2,16,16,false};
    std::uint64_t frames=0;
    for(;;) {
        std::size_t n=0; Frame f;
        while(n<4096&&queue_.pop(f)) {
            write_pcm(bytes.data()+n*4,f.l,fmt); write_pcm(bytes.data()+n*4+2,f.r,fmt); ++n;
        }
        if(n) {
            if((frames+n)*4>0xffffff00u){fault_=3;enabled_=false;break;}
            file_.write(reinterpret_cast<const char*>(bytes.data()),static_cast<std::streamsize>(n*4));
            if(!file_){fault_=2;enabled_=false;break;}
            frames+=n; written_=frames;
        } else if(!enabled_.load(std::memory_order_acquire)&&producers_.load(std::memory_order_acquire)==0&&queue_.empty()) break;
        else std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    // Try to salvage a valid header even following a write failure.
    file_.clear(); header(static_cast<std::uint32_t>(frames*4)); file_.flush();
    if(!file_) fault_=2;
    file_.close(); enabled_=false;
}
} // namespace ty
