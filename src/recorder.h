#pragma once
#include "audio_core.h"
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

namespace ty {
// Disk I/O is confined to the writer/control threads, never the audio thread.
class Recorder {
public:
    Recorder();
    ~Recorder();
    Recorder(const Recorder&) = delete;
    Recorder& operator=(const Recorder&) = delete;
    bool start(const std::filesystem::path& path, unsigned sampleRate, std::string& error);
    void stop();
    void push(const Frame* frames, std::size_t count) noexcept;
    bool active() const noexcept { return enabled_.load(std::memory_order_acquire); }
    unsigned fault() const noexcept { return fault_.load(); } // 1 queue overrun, 2 disk, 3 RIFF limit
    std::uint64_t frames_written() const noexcept { return written_.load(); }
private:
    void run() noexcept;
    void header(std::uint32_t bytes);
    SpscFrames queue_;
    std::atomic<bool> enabled_{false};
    std::atomic<unsigned> producers_{0}, fault_{0};
    std::atomic<std::uint64_t> written_{0};
    std::thread thread_;
    std::ofstream file_;
    unsigned rate_ = 48000;
};
} // namespace ty
