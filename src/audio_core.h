#pragma once
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace ty {
struct Frame { float l = 0, r = 0; };
inline constexpr double pi = 3.14159265358979323846;
float db_to_gain(float db) noexcept;
float gain_to_db(float gain) noexcept;
float sanitize(float value) noexcept;

// Unaligned, little-endian PCM conversion. Valid bits in a wider PCM container
// are left aligned, per WAVEFORMATEXTENSIBLE. No type-punning/unaligned loads.
struct PcmFormat {
    unsigned rate = 48000, channels = 2, bits = 32, validBits = 32;
    bool floating = true;
    bool supported() const noexcept;
    unsigned sample_bytes() const noexcept { return bits / 8; }
    unsigned frame_bytes() const noexcept { return channels * sample_bytes(); }
};
float read_pcm(const std::uint8_t* p, const PcmFormat& f) noexcept;
void write_pcm(std::uint8_t* p, float x, const PcmFormat& f) noexcept;

enum class Route : int { MonoLeft, MonoRight, Stereo, MonoMix };
struct Parameters {
    float inputDb = 0, outputDb = -18, gateDb = -50;
    float lowDb = 0, midDb = 0, highDb = 0;
    bool muted = false, highpass = false, gate = false, compressor = false;
    bool bypass = false;
    Route route = Route::MonoLeft;
};
// UI -> audio, individually atomic controls, sampled once per packet.
struct AtomicParameters {
    std::atomic<float> inputDb{0}, outputDb{-18}, gateDb{-50};
    std::atomic<float> lowDb{0}, midDb{0}, highDb{0};
    std::atomic<bool> muted{false}, highpass{false}, gate{false}, compressor{false}, bypass{false};
    std::atomic<int> route{0};
    Parameters load() const noexcept;
};
struct Biquad {
    std::array<double, 5> c{1,0,0,0,0}, target{1,0,0,0,0};
    double z1 = 0, z2 = 0, smooth = 0.01;
    void reset(double sampleRate) noexcept;
    void design(int type, double frequency, double db, double sampleRate) noexcept;
    float process(float x) noexcept;
};
class VoiceProcessor {
public:
    void reset(unsigned rate) noexcept;
    void configure(const Parameters& p) noexcept;
    Frame process(Frame x) noexcept;
private:
    unsigned rate_ = 48000;
    Parameters p_{};
    std::array<std::array<Biquad, 3>, 2> eq_{};
    std::array<float, 2> hpX_{}, hpY_{};
    float inputGain_ = 1, envelope_ = 0, gateGain_ = 1, compGain_ = 1;
    float hpA_ = 0, smoothing_ = 0, envAttack_ = 0, envRelease_ = 0;
    float gateOpen_ = 0, gateClose_ = 0, compAttack_ = 0, compRelease_ = 0;
    bool gateOpenState_ = true;
};
class MonitorGain {
public:
    void reset(unsigned rate) noexcept;
    void configure(const Parameters& p) noexcept;
    Frame process(Frame x) noexcept;
    std::uint64_t limited_samples() const noexcept { return limited_; }
private:
    float gain_ = 0, target_ = 0, alpha_ = 0.01f;
    std::uint64_t limited_ = 0;
};

// One audio thread owns this bounded bridge. Capture and render may have different
// rates and clocks. A 32-tap/1024-phase low-pass interpolator compensates both.
// All tables/storage are allocated by reset(), before either stream starts.
class AudioBridge {
public:
    void reset(unsigned inputRate, unsigned outputRate, std::size_t captureQuantum,
               std::size_t renderQuantum, unsigned safetyBlocks);
    void discontinuity() noexcept;
    void push(Frame frame) noexcept;
    void render(Frame* output, std::size_t count) noexcept;
    std::size_t queued_frames() const noexcept;
    double correction_ppm() const noexcept { return correctionPpm_; }
    std::uint64_t underruns() const noexcept { return underruns_; }
    std::uint64_t resyncs() const noexcept { return resyncs_; }
    std::uint64_t discarded_frames() const noexcept { return discarded_; }
    std::size_t target_frames() const noexcept { return target_; }
private:
    static constexpr std::size_t taps = 32, left = 15, phases = 1024;
    std::vector<Frame> ring_;
    std::vector<std::array<float, taps>> kernel_;
    std::size_t head_ = 0, size_ = 0, target_ = 0, quantum_ = 0;
    double ratio_ = 1, position_ = left, filteredError_ = 0, integral_ = 0, correctionPpm_ = 0;
    unsigned outputRate_ = 48000;
    bool primed_ = false;
    float fade_ = 0;
    std::uint64_t underruns_ = 0, resyncs_ = 0, discarded_ = 0;
    const Frame& at(std::size_t index) const noexcept;
    void advance(std::size_t count) noexcept;
};

// Only the recording path needs SPSC concurrency. Producer never overwrites data
// the consumer is reading. The only loss policy is rejecting new frames.
class SpscFrames {
public:
    explicit SpscFrames(std::size_t capacity);
    bool push(Frame value) noexcept;
    bool pop(Frame& value) noexcept;
    bool empty() const noexcept;
    void reset_quiescent() noexcept;
private:
    std::vector<Frame> storage_;
    alignas(64) std::atomic<std::uint64_t> write_{0};
    alignas(64) std::atomic<std::uint64_t> read_{0};
};
static_assert(std::atomic<float>::is_always_lock_free, "Lock-free audio controls required");
static_assert(std::atomic<std::uint64_t>::is_always_lock_free, "64-bit build required");
} // namespace ty
