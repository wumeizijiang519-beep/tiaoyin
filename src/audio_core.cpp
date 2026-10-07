#include "audio_core.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>

namespace ty {
float db_to_gain(float db) noexcept { return std::pow(10.0f, db / 20.0f); }
float gain_to_db(float g) noexcept { return 20.0f * std::log10(std::max(g, 1.0e-6f)); }
float sanitize(float v) noexcept { return std::isfinite(v) ? v : 0.0f; }
bool PcmFormat::supported() const noexcept {
    return rate >= 8000 && rate <= 192000 && channels >= 1 && channels <= 32 &&
        ((floating && bits == 32 && validBits == 32) ||
         (!floating && (bits == 8 || bits == 16 || bits == 24 || bits == 32) &&
          validBits >= 2 && validBits <= bits));
}
float read_pcm(const std::uint8_t* p, const PcmFormat& f) noexcept {
    if (f.floating) { float x; std::memcpy(&x, p, sizeof x); return sanitize(x); }
    if (f.bits == 8) return (static_cast<int>(*p) - 128) / 128.0f;
    std::uint32_t u = 0;
    for (unsigned i = 0; i < f.sample_bytes(); ++i) u |= std::uint32_t(p[i]) << (i * 8);
    const std::int64_t s = (u & (std::uint32_t(1) << (f.bits - 1)))
        ? static_cast<std::int64_t>(u) - (std::int64_t(1) << f.bits) : u;
    return static_cast<float>(s / static_cast<double>(std::int64_t(1) << (f.bits - 1)));
}
void write_pcm(std::uint8_t* p, float x, const PcmFormat& f) noexcept {
    x = std::clamp(sanitize(x), -1.0f, 1.0f);
    if (f.floating) { std::memcpy(p, &x, sizeof x); return; }
    if (f.bits == 8) {
        *p = static_cast<std::uint8_t>(std::clamp(std::lround(x * 128 + 128), 0L, 255L)); return;
    }
    const auto scale = std::int64_t(1) << (f.validBits - 1);
    const auto s = std::clamp<std::int64_t>(std::llround(static_cast<double>(x) * scale), -scale, scale - 1);
    const auto u = static_cast<std::uint32_t>(s * (std::int64_t(1) << (f.bits - f.validBits)));
    for (unsigned i = 0; i < f.sample_bytes(); ++i) p[i] = static_cast<std::uint8_t>(u >> (8 * i));
}
Parameters AtomicParameters::load() const noexcept {
    Parameters p;
    p.inputDb = inputDb.load(std::memory_order_relaxed); p.outputDb = outputDb.load(std::memory_order_relaxed);
    p.gateDb = gateDb.load(std::memory_order_relaxed);
    p.lowDb = lowDb.load(std::memory_order_relaxed); p.midDb = midDb.load(std::memory_order_relaxed);
    p.highDb = highDb.load(std::memory_order_relaxed);
    p.muted = muted.load(std::memory_order_relaxed); p.highpass = highpass.load(std::memory_order_relaxed);
    p.gate = gate.load(std::memory_order_relaxed); p.compressor = compressor.load(std::memory_order_relaxed);
    p.bypass = bypass.load(std::memory_order_relaxed);
    p.route = static_cast<Route>(std::clamp(route.load(std::memory_order_relaxed), 0, 3));
    return p;
}
void Biquad::reset(double sr) noexcept {
    c = target = {1,0,0,0,0}; z1 = z2 = 0; smooth = 1 - std::exp(-1.0 / (0.01 * sr));
}
void Biquad::design(int type, double freq, double db, double sr) noexcept {
    freq = std::min(freq, sr * 0.4);
    const double w = 2 * pi * freq / sr, cs = std::cos(w), sn = std::sin(w);
    const double a = std::pow(10.0, db / 40), alpha = sn / std::sqrt(2.0), beta = 2 * std::sqrt(a) * alpha;
    double b0, b1, b2, a0, a1, a2;
    if (type == 0) { // RBJ low shelf, S = 1
        b0=a*((a+1)-(a-1)*cs+beta); b1=2*a*((a-1)-(a+1)*cs); b2=a*((a+1)-(a-1)*cs-beta);
        a0=(a+1)+(a-1)*cs+beta; a1=-2*((a-1)+(a+1)*cs); a2=(a+1)+(a-1)*cs-beta;
    } else if (type == 2) { // high shelf
        b0=a*((a+1)+(a-1)*cs+beta); b1=-2*a*((a-1)+(a+1)*cs); b2=a*((a+1)+(a-1)*cs-beta);
        a0=(a+1)-(a-1)*cs+beta; a1=2*((a-1)-(a+1)*cs); a2=(a+1)-(a-1)*cs-beta;
    } else { // peak, Q = 0.7071
        b0=1+alpha*a; b1=-2*cs; b2=1-alpha*a; a0=1+alpha/a; a1=-2*cs; a2=1-alpha/a;
    }
    target = {b0/a0,b1/a0,b2/a0,a1/a0,a2/a0};
}
float Biquad::process(float x) noexcept {
    for (std::size_t i=0; i<c.size(); ++i) c[i] += smooth*(target[i]-c[i]);
    const double y=c[0]*x+z1;
    z1=c[1]*x-c[3]*y+z2; z2=c[2]*x-c[4]*y;
    if (std::abs(z1)<1e-25) z1=0;
    if (std::abs(z2)<1e-25) z2=0;
    return sanitize(static_cast<float>(y));
}
void VoiceProcessor::reset(unsigned rate) noexcept {
    rate_=rate; p_=Parameters{}; hpX_={}; hpY_={}; inputGain_=1; envelope_=0;
    gateGain_=compGain_=1; gateOpenState_=true;
    hpA_=static_cast<float>(std::exp(-2*pi*80/rate));
    const auto coeff=[rate](double seconds){return static_cast<float>(1-std::exp(-1.0/(rate*seconds)));};
    smoothing_=coeff(.005); envAttack_=coeff(.002); envRelease_=coeff(.100);
    gateOpen_=coeff(.005); gateClose_=coeff(.080); compAttack_=coeff(.003); compRelease_=coeff(.120);
    for(auto& channel:eq_) for(auto& filter:channel) filter.reset(rate);
}
void VoiceProcessor::configure(const Parameters& p) noexcept {
    const std::array<float,3> old{p_.lowDb,p_.midDb,p_.highDb}, now{p.lowDb,p.midDb,p.highDb};
    for(unsigned i=0;i<3;++i) if(now[i]!=old[i])
        for(auto& ch:eq_) ch[i].design(static_cast<int>(i), i==0?120:i==1?1500:6000,
                                      std::clamp(now[i],-12.0f,12.0f), rate_);
    p_=p;
}
Frame VoiceProcessor::process(Frame x) noexcept {
    const float target=db_to_gain(std::clamp(p_.inputDb,-24.0f,24.0f));
    inputGain_+=smoothing_*(target-inputGain_);
    x.l=std::clamp(sanitize(x.l),-16.0f,16.0f)*inputGain_;
    x.r=std::clamp(sanitize(x.r),-16.0f,16.0f)*inputGain_;
    float channel[2]{x.l,x.r};
    for(unsigned i=0;i<2;++i) {
        const float hp=hpA_*(hpY_[i]+channel[i]-hpX_[i]); hpX_[i]=channel[i]; hpY_[i]=hp;
        float wet=(!p_.bypass&&p_.highpass)?hp:channel[i];
        for(auto& filter:eq_[i]) wet=filter.process(wet);
        if(!p_.bypass) channel[i]=wet;
    }
    const float level=std::max(std::abs(channel[0]),std::abs(channel[1]));
    envelope_+=(level>envelope_?envAttack_:envRelease_)*(level-envelope_);
    const float threshold=db_to_gain(std::clamp(p_.gateDb,-80.0f,-10.0f));
    if(envelope_>threshold*1.412538f) gateOpenState_=true;
    else if(envelope_<threshold) gateOpenState_=false;
    const float gt=(!p_.bypass&&p_.gate&&!gateOpenState_)?0.0f:1.0f;
    gateGain_+=(gt>gateGain_?gateOpen_:gateClose_)*(gt-gateGain_);
    float ct=1;
    if(!p_.bypass&&p_.compressor&&envelope_>0.12589254f)
        ct=std::pow(envelope_/0.12589254f,-2.0f/3.0f); // -18 dBFS, 3:1, no lookahead
    compGain_+=(ct<compGain_?compAttack_:compRelease_)*(ct-compGain_);
    return {channel[0]*gateGain_*compGain_,channel[1]*gateGain_*compGain_};
}
void MonitorGain::reset(unsigned rate) noexcept {
    gain_=target_=0; limited_=0; alpha_=static_cast<float>(1-std::exp(-1.0/(.005*rate)));
}
void MonitorGain::configure(const Parameters& p) noexcept {
    target_=p.muted?0:db_to_gain(std::clamp(p.outputDb,-60.0f,0.0f));
}
Frame MonitorGain::process(Frame x) noexcept {
    gain_+=alpha_*(target_-gain_);
    if(target_==0&&gain_<1e-6f) gain_=0;
    x.l=sanitize(x.l)*gain_; x.r=sanitize(x.r)*gain_;
    if(std::abs(x.l)>.95f) ++limited_;
    if(std::abs(x.r)>.95f) ++limited_;
    return {std::clamp(x.l,-.95f,.95f),std::clamp(x.r,-.95f,.95f)};
}
void AudioBridge::reset(unsigned inRate,unsigned outRate,std::size_t cq,std::size_t rq,unsigned blocks,std::size_t compactReserveFrames) {
    if(inRate<8000||outRate<8000||inRate>192000||outRate>192000||!cq||!rq||cq>192000||rq>192000)
        throw std::invalid_argument("Invalid audio bridge format/quantum");
    ratio_=static_cast<double>(inRate)/outRate; outputRate_=outRate;
    quantum_=std::max(cq,static_cast<std::size_t>(std::ceil(rq*ratio_)));
    target_=quantum_*std::clamp(blocks,1u,4u);
    compact_=blocks==1 && compactReserveFrames>0;
    if(compact_)target_=std::min(target_,std::max<std::size_t>(32,compactReserveFrames));
    ring_.assign(std::max<std::size_t>(8192,(target_+quantum_)*8+taps),{});
    kernel_.resize(phases+1);
    const double cutoff=.94*std::min(1.0,1.0/(ratio_*1.002));
    for(std::size_t p=0;p<=phases;++p) {
        double sum=0;
        for(std::size_t j=0;j<taps;++j) {
            const double x=static_cast<double>(j)-left-static_cast<double>(p)/phases;
            const double sinc=std::abs(x)<1e-12?cutoff:std::sin(pi*cutoff*x)/(pi*x);
            const double window=std::abs(x)>16?0:.42+.5*std::cos(pi*x/16)+.08*std::cos(2*pi*x/16);
            kernel_[p][j]=static_cast<float>(sinc*window); sum+=kernel_[p][j];
        }
        for(float& k:kernel_[p]) k=static_cast<float>(k/sum);
    }
    underruns_=resyncs_=discarded_=0; filteredError_=integral_=correctionPpm_=0;
    head_=size_=0; position_=left; primed_=false; fade_=0;
    for(std::size_t i=0;i<left;++i) push({});
}
const Frame& AudioBridge::at(std::size_t i) const noexcept { return ring_[(head_+i)%ring_.size()]; }
void AudioBridge::advance(std::size_t n) noexcept { n=std::min(n,size_); head_=(head_+n)%ring_.size(); size_-=n; }
void AudioBridge::discontinuity() noexcept {
    head_=size_=0; position_=left; primed_=false; fade_=0;
    filteredError_=integral_=correctionPpm_=0; ++resyncs_;
    for(std::size_t i=0;i<left;++i) push({});
}
void AudioBridge::push(Frame f) noexcept {
    if(size_==ring_.size()) { advance(1); ++discarded_; primed_=false; fade_=0; }
    ring_[(head_+size_)%ring_.size()]=f; ++size_;
}
std::size_t AudioBridge::queued_frames() const noexcept { return size_>left?size_-left:0; }
void AudioBridge::render(Frame* out,std::size_t count) noexcept {
    const std::size_t needed=static_cast<std::size_t>(std::ceil(count*ratio_*1.002))+taps;
    if(!primed_) {
        if(size_<needed+target_) { std::fill_n(out,count,Frame{}); return; }
        // Whole capture packets may overshoot the startup reserve by a quantum.
        // Trim only before starting/fading in, never on every normal packet.
        if(compact_ && size_>needed+target_) {
            const auto drop=size_-(needed+target_);advance(drop);discarded_+=drop;
        }
        primed_=true; fade_=0;
    }
    if(size_>needed+target_+3*quantum_) {
        const auto drop=size_-(needed+target_); advance(drop); discarded_+=drop; ++resyncs_; fade_=0;
    }
    const double dt=static_cast<double>(count)/outputRate_;
    const double error=(static_cast<double>(size_)-needed-target_)/std::max<double>(1,target_);
    filteredError_+=(1-std::exp(-dt/.5))*(error-filteredError_);
    integral_=std::clamp(integral_+filteredError_*dt*180.0,-1800.0,1800.0);
    correctionPpm_=std::clamp(integral_+filteredError_*1200.0,-2000.0,2000.0);
    const double step=ratio_*(1+correctionPpm_*1e-6);
    for(std::size_t n=0;n<count;++n) {
        const auto pos=static_cast<std::size_t>(position_);
        if(pos+16>=size_) {
            std::fill(out+n,out+count,Frame{}); ++underruns_; primed_=false; fade_=0; return;
        }
        const auto phase=std::min(phases,static_cast<std::size_t>((position_-pos)*phases+.5));
        Frame y{};
        for(std::size_t j=0;j<taps;++j) {
            const auto& f=at(pos-left+j); y.l+=f.l*kernel_[phase][j]; y.r+=f.r*kernel_[phase][j];
        }
        fade_=std::min(1.0f,fade_+1.0f/(.005f*outputRate_));
        out[n]={y.l*fade_,y.r*fade_}; position_+=step;
        const auto consumed=static_cast<std::size_t>(position_)-left;
        advance(consumed); position_-=consumed;
    }
}
SpscFrames::SpscFrames(std::size_t n):storage_(std::max<std::size_t>(n,2)){}
bool SpscFrames::push(Frame f) noexcept {
    const auto w=write_.load(std::memory_order_relaxed);
    if(w-read_.load(std::memory_order_acquire)>=storage_.size()) return false;
    storage_[w%storage_.size()]=f; write_.store(w+1,std::memory_order_release); return true;
}
bool SpscFrames::pop(Frame& f) noexcept {
    const auto r=read_.load(std::memory_order_relaxed);
    if(r==write_.load(std::memory_order_acquire)) return false;
    f=storage_[r%storage_.size()]; read_.store(r+1,std::memory_order_release); return true;
}
bool SpscFrames::empty() const noexcept { return read_.load(std::memory_order_acquire)==write_.load(std::memory_order_acquire); }
void SpscFrames::reset_quiescent() noexcept { write_.store(0); read_.store(0); }
} // namespace ty
