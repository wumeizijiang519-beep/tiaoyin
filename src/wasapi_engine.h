#pragma once
#include "audio_core.h"
#include "recorder.h"
#include <windows.h>
#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace ty {
struct Endpoint { std::wstring id, name; bool isDefault = false; };
std::vector<Endpoint> enumerate_endpoints(bool capture);
std::wstring audio_error(HRESULT hr);

struct StreamConfig {
    std::wstring inputId, outputId;
    bool inputExclusive = false, outputExclusive = false, raw = true;
    unsigned exclusiveRate = 48000, safetyBlocks = 1;
    double periodMs = 3.0;
};
enum class EngineState : int { Stopped, Starting, Running, Failed };
struct EngineStats {
    std::atomic<unsigned> inputRate{0},outputRate{0},inputChannels{0},outputChannels{0};
    std::atomic<unsigned> inputPeriod{0},outputPeriod{0},inputBuffer{0},outputBuffer{0};
    std::atomic<unsigned> inputMinPeriod{0},outputMinPeriod{0};
    std::atomic<double> reserveMs{0};
    std::atomic<double> inputLatencyMs{0},outputLatencyMs{0},queueMs{0},driftPpm{0},loadPercent{0};
    std::atomic<float> inputPeak{0},outputPeakL{0},outputPeakR{0};
    std::atomic<std::uint64_t> underruns{0},resyncs{0},discarded{0},discontinuities{0},clipped{0},limited{0};
    std::atomic<bool> mmcss{false},inputLowLatency{false},outputLowLatency{false},inputRaw{false},outputRaw{false};
    std::atomic<bool> inputExclusive{false},outputExclusive{false};
    void reset() noexcept;
};
class WasapiEngine {
public:
    WasapiEngine();
    ~WasapiEngine();
    WasapiEngine(const WasapiEngine&) = delete;
    WasapiEngine& operator=(const WasapiEngine&) = delete;
    void start(StreamConfig config);
    void stop();
    EngineState state() const noexcept {return state_.load(std::memory_order_acquire);}
    std::wstring last_error() const;
    AtomicParameters parameters;
    EngineStats stats;
    Recorder recorder;
private:
    void run(StreamConfig config) noexcept;
    HANDLE stopEvent_ = nullptr;
    std::thread worker_;
    std::atomic<EngineState> state_{EngineState::Stopped};
    mutable std::mutex errorMutex_;
    std::wstring error_;
};
} // namespace ty
