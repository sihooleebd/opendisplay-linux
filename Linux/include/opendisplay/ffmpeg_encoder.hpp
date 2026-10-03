#pragma once

#include "opendisplay/encoder.hpp"
#include "opendisplay/types.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace od {

/// Low-latency FFmpeg subprocess adapter. Capture threads only replace a
/// single pending frame, and no more than `maxFramesInFlight` frames are handed
/// to FFmpeg at once, so a slow encoder costs dropped frames rather than a
/// growing queue the application cannot see.
class FfmpegEncoder final : public Encoder {
public:
    FfmpegEncoder() = default;
    ~FfmpegEncoder() override;

    void start(EncoderConfig config, FrameCallback callback) override;
    void submit(CapturedFrame frame) override;
    void requestKeyframe() override;
    void stop() override;
    [[nodiscard]] std::string selectedEncoder() const override;
    /// The subprocess receives frames as raw bytes over a pipe, which a GPU
    /// buffer cannot travel through.
    [[nodiscard]] bool acceptsDmabuf() const override { return false; }

private:
    void run();
    void startProcess(const VideoFormat& input);
    void stopProcess();
    void readOutput(int fd);
    void consumeNal(std::string nal, std::string& accessUnit, bool& hasVcl);
    void emitAccessUnit(std::string accessUnit);
    std::vector<std::string> arguments(const VideoFormat& input) const;
    EncoderKind chooseEncoder() const;

    EncoderConfig config_;
    FrameCallback callback_;
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::optional<CapturedFrame> pending_;
    std::deque<std::int64_t> timestamps_;
    // Frames written to FFmpeg that have not come back as access units. The
    // macOS sender caps the same count at one ("latest frame wins"); FFmpeg
    // sits behind two pipes and needs more pipelining to stay busy. Measured on
    // Intel iHD at 2388x1668: a cap of 2 costs a third of the throughput (39
    // fps) for 3 ms of latency, while 4 and above give nothing back but
    // latency. Three is the knee -- 51 fps at a 66 ms median, against 39 fps
    // and 129 ms with no cap at all.
    static constexpr int maxFramesInFlight = 3;
    int inFlight_ = 0;
    std::thread worker_;
    std::thread reader_;
    bool running_ = false;
    bool restartRequested_ = false;
    int inputFd_ = -1;
    int outputFd_ = -1;
    int childPid_ = -1;
    VideoFormat inputFormat_;
    EncoderKind selected_ = EncoderKind::Software;
};

}  // namespace od
