#pragma once

#include "opendisplay/types.hpp"

#include <pipewire/pipewire.h>
#include <spa/param/video/format-utils.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

namespace od {

/// Shared between the capture object and any outstanding buffer lease, so a
/// frame released after the stream is gone can tell and do nothing.
struct CaptureStreamHandle {
    pw_thread_loop* loop = nullptr;
    pw_stream* stream = nullptr;
    std::atomic_bool alive{false};
};

class PipeWireCapture {
public:
    using FrameCallback = std::function<void(CapturedFrame)>;

    PipeWireCapture() = default;
    ~PipeWireCapture();
    PipeWireCapture(const PipeWireCapture&) = delete;
    PipeWireCapture& operator=(const PipeWireCapture&) = delete;

    /// `allowDmabuf` offers the producer a GPU-buffer path alongside the
    /// system-memory one. A producer that cannot share GPU buffers negotiates
    /// system memory as before, so this is safe to leave on.
    void start(int remoteFd, std::uint32_t nodeId, int width, int height, int fps,
               FrameCallback callback, bool allowDmabuf = true);
    /// True once a frame has arrived as a GPU buffer rather than a copy.
    [[nodiscard]] bool usingDmabuf() const;
    void stop();
    [[nodiscard]] std::optional<std::string> error() const;

    // PipeWire's C callbacks are public only so the static event table can
    // reference them; callers should use start()/stop().
    static void stateChanged(void* data, pw_stream_state oldState, pw_stream_state state,
                             const char* error);
    static void parameterChanged(void* data, std::uint32_t id, const spa_pod* parameter);
    static void process(void* data);

private:
    void handleProcess();
    void handleDmabuf(pw_buffer* pipewireBuffer);
    void announceBufferParams();

    pw_thread_loop* loop_ = nullptr;
    pw_context* context_ = nullptr;
    pw_core* core_ = nullptr;
    pw_stream* stream_ = nullptr;
    spa_hook listener_{};
    spa_video_info_raw format_{};
    bool allowDmabuf_ = true;
    bool negotiatedDmabuf_ = false;
    FrameCallback callback_;
    std::shared_ptr<FramePool> pool_;
    std::shared_ptr<CaptureStreamHandle> handle_;
    std::atomic<std::uint64_t> sequence_ = 0;
    mutable std::mutex stateMutex_;
    std::condition_variable stateCondition_;
    pw_stream_state state_ = PW_STREAM_STATE_UNCONNECTED;
    std::string error_;
};

}  // namespace od
