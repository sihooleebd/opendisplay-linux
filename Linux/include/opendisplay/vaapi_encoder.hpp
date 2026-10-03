#pragma once

#include "opendisplay/encoder.hpp"
#include "opendisplay/types.hpp"

#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

struct AVBufferRef;
struct AVCodecContext;
struct AVFilterContext;
struct AVFilterGraph;
struct AVFrame;
struct AVPacket;
struct AVBSFContext;

namespace od {

/// In-process VA-API H.264 encoder.
///
/// The FFmpeg subprocess had to receive each frame as raw bytes through a
/// pipe, which cost a 16 MB write per frame and a process boundary, and ruled
/// out handing over a GPU buffer at all. Encoding in-process removes both and
/// is what makes zero-copy capture possible: a compositor DMA-BUF is imported
/// straight into a VA-API surface, and only a CPU frame is uploaded.
///
/// Like the subprocess encoder, the capture thread only ever replaces a single
/// pending frame, so a slow encoder costs stale frames rather than latency.
/// Unlike it, encoding is synchronous on the worker, so a frame's latency is
/// the encode itself rather than the depth of a pipeline.
class VaapiEncoder final : public Encoder {
public:

    VaapiEncoder() = default;
    ~VaapiEncoder() override;

    /// Throws if VA-API cannot be brought up, so callers can fall back to the
    /// subprocess encoder.
    void start(EncoderConfig config, FrameCallback callback) override;
    void submit(CapturedFrame frame) override;
    void requestKeyframe() override;
    void stop() override;
    [[nodiscard]] std::string selectedEncoder() const override {
        return "h264_vaapi (in-process)";
    }
    [[nodiscard]] bool acceptsDmabuf() const override { return true; }
    /// Opens and releases the device so an unusable configuration is reported
    /// before the session commits to this encoder.
    void probe(const EncoderConfig& config);
    [[nodiscard]] std::optional<std::string> error() const;

    /// True when the last frame was imported from a DMA-BUF rather than
    /// uploaded, which is what the zero-copy path is for.
    [[nodiscard]] bool lastFrameWasZeroCopy() const;

    /// Rolling per-stage timings, so a latency figure can be attributed
    /// rather than guessed at. Fixed capacity: sampling must not allocate on
    /// the encode path.
    class StageTimings {
    public:
        void add(double milliseconds);
        [[nodiscard]] double percentile(double fraction) const;
        [[nodiscard]] std::size_t count() const { return count_; }
        void reset() { count_ = 0; next_ = 0; }

    private:
        static constexpr std::size_t capacity = 256;
        std::array<double, capacity> samples_{};
        std::size_t count_ = 0;
        std::size_t next_ = 0;
    };

private:
    void run();
    void reportTimings();
    void openDevice();
    void openGraphAndEncoder(const VideoFormat& input);
    void closeGraphAndEncoder();
    /// Produces a VA-API surface for `frame`, importing a DMA-BUF when one is
    /// present and uploading the CPU buffer otherwise.
    AVFrame* surfaceFor(const CapturedFrame& frame);
    void encodeOne(CapturedFrame frame);
    void drainPackets();
    void fail(const std::string& message);

    EncoderConfig config_;
    FrameCallback callback_;
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::optional<CapturedFrame> pending_;
    std::thread worker_;
    bool running_ = false;
    bool restartRequested_ = false;
    std::string error_;
    bool zeroCopy_ = false;

    AVBufferRef* drmDevice_ = nullptr;
    AVBufferRef* vaapiDevice_ = nullptr;
    AVBufferRef* inputFrames_ = nullptr;
    AVFilterGraph* graph_ = nullptr;
    AVFilterContext* graphSource_ = nullptr;
    AVFilterContext* graphSink_ = nullptr;
    AVCodecContext* codec_ = nullptr;
    AVBSFContext* bitstream_ = nullptr;
    AVFrame* uploadStaging_ = nullptr;
    AVPacket* packet_ = nullptr;
    VideoFormat inputFormat_;
    std::int64_t nextPts_ = 0;
    std::int64_t pendingCapturedAtMs_ = 0;
    StageTimings mapTimings_;
    StageTimings filterTimings_;
    StageTimings encodeTimings_;
    std::chrono::steady_clock::time_point lastReport_{};
};

}  // namespace od
