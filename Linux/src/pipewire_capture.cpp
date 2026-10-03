#include "opendisplay/pipewire_capture.hpp"

#include "opendisplay/log.hpp"
#include "opendisplay/pipewire_format.hpp"

#include <spa/buffer/buffer.h>
#include <spa/param/format-utils.h>
#include <spa/param/param.h>
#include <spa/param/video/raw.h>
#include <spa/utils/result.h>

#include <drm_fourcc.h>

#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <mutex>
#include <optional>
#include <stdexcept>

namespace od {
namespace {

const pw_stream_events streamEvents = [] {
    pw_stream_events events{};
    events.version = PW_VERSION_STREAM_EVENTS;
    events.state_changed = PipeWireCapture::stateChanged;
    events.param_changed = PipeWireCapture::parameterChanged;
    events.process = PipeWireCapture::process;
    return events;
}();

std::optional<VideoFormat::PixelFormat> pixelFormat(const spa_video_format format) {
    switch (format) {
    case SPA_VIDEO_FORMAT_BGRA: return VideoFormat::PixelFormat::Bgra;
    case SPA_VIDEO_FORMAT_BGRx: return VideoFormat::PixelFormat::Bgrx;
    case SPA_VIDEO_FORMAT_RGBA: return VideoFormat::PixelFormat::Rgba;
    case SPA_VIDEO_FORMAT_RGBx: return VideoFormat::PixelFormat::Rgbx;
    default: return std::nullopt;
    }
}

std::uint32_t drmFormatFor(const spa_video_format format) {
    switch (format) {
    case SPA_VIDEO_FORMAT_BGRA: return DRM_FORMAT_ARGB8888;
    case SPA_VIDEO_FORMAT_BGRx: return DRM_FORMAT_XRGB8888;
    case SPA_VIDEO_FORMAT_RGBA: return DRM_FORMAT_ABGR8888;
    case SPA_VIDEO_FORMAT_RGBx: return DRM_FORMAT_XBGR8888;
    default: return 0;
    }
}

const char* pixelFormatName(const spa_video_format format) {
    switch (format) {
    case SPA_VIDEO_FORMAT_BGRA: return "BGRA";
    case SPA_VIDEO_FORMAT_BGRx: return "BGRx";
    case SPA_VIDEO_FORMAT_RGBA: return "RGBA";
    case SPA_VIDEO_FORMAT_RGBx: return "RGBx";
    default: return "unknown";
    }
}

}  // namespace

PipeWireCapture::~PipeWireCapture() { stop(); }

void PipeWireCapture::start(const int remoteFd, const std::uint32_t nodeId, const int width,
                            const int height, const int fps, FrameCallback callback,
                            const bool allowDmabuf) {
    stop();
    {
        std::lock_guard lock(stateMutex_);
        state_ = PW_STREAM_STATE_UNCONNECTED;
        error_.clear();
    }
    static std::once_flag initialized;
    std::call_once(initialized, [] { pw_init(nullptr, nullptr); });

    callback_ = std::move(callback);
    allowDmabuf_ = allowDmabuf;
    negotiatedDmabuf_ = false;
    pool_ = FramePool::create();
    handle_ = std::make_shared<CaptureStreamHandle>();
    loop_ = pw_thread_loop_new("opendisplay-capture", nullptr);
    if (loop_ == nullptr) {
        ::close(remoteFd);
        throw std::runtime_error("cannot create PipeWire thread loop");
    }
    context_ = pw_context_new(pw_thread_loop_get_loop(loop_), nullptr, 0);
    if (context_ == nullptr) {
        ::close(remoteFd);
        stop();
        throw std::runtime_error("cannot create PipeWire context");
    }
    core_ = pw_context_connect_fd(context_, remoteFd, nullptr, 0);
    if (core_ == nullptr) {
        ::close(remoteFd);
        stop();
        throw std::runtime_error("cannot connect to the portal PipeWire remote");
    }

    auto* properties = pw_properties_new(
        PW_KEY_MEDIA_TYPE, "Video",
        PW_KEY_MEDIA_CATEGORY, "Capture",
        PW_KEY_MEDIA_ROLE, "Screen",
        PW_KEY_APP_NAME, "OpenDisplay",
        nullptr);
    stream_ = pw_stream_new(core_, "OpenDisplay KDE capture", properties);
    if (stream_ == nullptr) {
        stop();
        throw std::runtime_error("cannot create PipeWire capture stream");
    }
    pw_stream_add_listener(stream_, &listener_, &streamEvents, this);
    handle_->loop = loop_;
    handle_->stream = stream_;
    handle_->alive.store(true);

    std::uint8_t storage[2048]{};
    spa_pod_builder builder = SPA_POD_BUILDER_INIT(storage, sizeof(storage));
    const spa_pod* parameters[2];
    std::uint32_t parameterCount = 0;
    if (allowDmabuf_) {
        // INVALID lets the producer keep its own tiling and report it back,
        // which is the layout that costs nothing to share; LINEAR is the
        // fallback every driver can import.
        static constexpr std::uint64_t modifiers[] = {DRM_FORMAT_MOD_INVALID,
                                                      DRM_FORMAT_MOD_LINEAR};
        parameters[parameterCount++] = buildPipeWireDmabufOffer(
            builder, width, height, fps, modifiers, std::size(modifiers));
    }
    parameters[parameterCount++] = buildPipeWireFormatOffer(builder, width, height, fps);

    // MAP_BUFFERS only affects system-memory buffers; a DMA-BUF is left
    // unmapped and reaches us as a file descriptor.
    const auto flags = static_cast<pw_stream_flags>(PW_STREAM_FLAG_AUTOCONNECT
        | PW_STREAM_FLAG_MAP_BUFFERS);
    const int result = pw_stream_connect(stream_, PW_DIRECTION_INPUT, nodeId, flags,
                                         parameters, parameterCount);
    if (result < 0) {
        stop();
        throw std::runtime_error(std::string("cannot connect PipeWire stream: ")
                                 + spa_strerror(result));
    }
    if (pw_thread_loop_start(loop_) < 0) {
        stop();
        throw std::runtime_error("cannot start PipeWire thread loop");
    }

    std::unique_lock lock(stateMutex_);
    const bool configured = stateCondition_.wait_for(lock, std::chrono::seconds(10), [this] {
        return state_ == PW_STREAM_STATE_PAUSED || state_ == PW_STREAM_STATE_STREAMING
            || state_ == PW_STREAM_STATE_ERROR || !error_.empty();
    });
    if (!configured || state_ == PW_STREAM_STATE_ERROR || !error_.empty()) {
        const std::string failure = !configured
            ? "timed out while negotiating a video format"
            : (error_.empty() ? "unknown stream error" : error_);
        lock.unlock();
        stop();
        throw std::runtime_error("PipeWire capture failed: " + failure);
    }
}

void PipeWireCapture::stop() {
    // Retire the handle before anything is destroyed, so a lease that outlives
    // the stream becomes a no-op instead of queueing onto freed memory.
    if (handle_) {
        handle_->alive.store(false);
        handle_->stream = nullptr;
        handle_->loop = nullptr;
    }
    if (loop_ != nullptr) {
        pw_thread_loop_stop(loop_);
    }
    if (stream_ != nullptr) {
        spa_hook_remove(&listener_);
        pw_stream_destroy(stream_);
        stream_ = nullptr;
    }
    if (core_ != nullptr) {
        pw_core_disconnect(core_);
        core_ = nullptr;
    }
    if (context_ != nullptr) {
        pw_context_destroy(context_);
        context_ = nullptr;
    }
    if (loop_ != nullptr) {
        pw_thread_loop_destroy(loop_);
        loop_ = nullptr;
    }
    callback_ = {};
    format_ = {};
    // Frames still in flight hold their own reference, so dropping ours here
    // only releases the retained spares.
    pool_.reset();
    handle_.reset();
}

std::optional<std::string> PipeWireCapture::error() const {
    std::lock_guard lock(stateMutex_);
    if (error_.empty()) {
        return std::nullopt;
    }
    return error_;
}

void PipeWireCapture::stateChanged(void* data, pw_stream_state, const pw_stream_state state,
                                   const char* error) {
    auto& self = *static_cast<PipeWireCapture*>(data);
    {
        std::lock_guard lock(self.stateMutex_);
        self.state_ = state;
        if (state == PW_STREAM_STATE_ERROR) {
            self.error_ = error != nullptr ? error : "unknown stream error";
        }
    }
    self.stateCondition_.notify_all();
    if (state == PW_STREAM_STATE_ERROR) {
        log(std::string("PipeWire stream error: ") + (error != nullptr ? error : "unknown"));
    } else if (state == PW_STREAM_STATE_STREAMING) {
        log("PipeWire capture is streaming");
    }
}

void PipeWireCapture::parameterChanged(void* data, const std::uint32_t id,
                                       const spa_pod* parameter) {
    auto& self = *static_cast<PipeWireCapture*>(data);
    if (parameter == nullptr || id != SPA_PARAM_Format) {
        return;
    }
    spa_video_info_raw negotiated{};
    if (spa_format_video_raw_parse(parameter, &negotiated) < 0
        || !pixelFormat(negotiated.format)) {
        constexpr auto message = "PipeWire returned an unsupported video format";
        log(message);
        self.format_ = {};
        {
            std::lock_guard lock(self.stateMutex_);
            self.error_ = message;
        }
        self.stateCondition_.notify_all();
        return;
    }
    self.format_ = negotiated;
    // A modifier property in the agreed format means the producer accepted
    // the GPU path. The pod still carries a choice at this point, so the
    // consumer confirms the single value the producer settled on.
    const bool dmabuf = self.allowDmabuf_
        && spa_pod_find_prop(parameter, nullptr, SPA_FORMAT_VIDEO_modifier) != nullptr;
    self.negotiatedDmabuf_ = dmabuf;
    log(std::string("PipeWire format: ") + std::to_string(self.format_.size.width) + "x"
        + std::to_string(self.format_.size.height) + " "
        + pixelFormatName(self.format_.format) + " @ "
        + std::to_string(self.format_.framerate.num) + "/"
        + std::to_string(self.format_.framerate.denom) + " fps, "
        + (dmabuf ? "GPU buffers (zero copy)" : "system memory"));
    self.announceBufferParams();
}

void PipeWireCapture::announceBufferParams() {
    // System memory needs nothing announced; PipeWire's defaults are what the
    // capture path used before GPU buffers existed, so leave them alone.
    if (stream_ == nullptr || !negotiatedDmabuf_) {
        return;
    }
    std::uint8_t storage[512]{};
    spa_pod_builder builder = SPA_POD_BUILDER_INIT(storage, sizeof(storage));
    const spa_pod* parameters[1];
    parameters[0] = buildPipeWireDmabufBufferParams(builder);
    pw_stream_update_params(stream_, parameters, 1);
}

bool PipeWireCapture::usingDmabuf() const {
    std::lock_guard lock(stateMutex_);
    return negotiatedDmabuf_;
}

void PipeWireCapture::process(void* data) {
    static_cast<PipeWireCapture*>(data)->handleProcess();
}

namespace {

/// Returns a capture buffer to the producer when the last holder drops it.
///
/// A GPU frame is not copied, so its buffer cannot be recycled until the
/// encoder has finished reading it. The requeue is bounced onto the PipeWire
/// loop because the last holder is usually the encoder thread.
struct BufferLease {
    std::shared_ptr<CaptureStreamHandle> handle;
    pw_buffer* buffer = nullptr;

    ~BufferLease() {
        if (!handle || buffer == nullptr || !handle->alive.load()) {
            return;
        }
        pw_thread_loop* loop = handle->loop;
        pw_stream* stream = handle->stream;
        if (loop == nullptr || stream == nullptr) {
            return;
        }
        struct Request {
            pw_stream* stream;
            pw_buffer* buffer;
        } request{stream, buffer};
        pw_loop_invoke(
            pw_thread_loop_get_loop(loop),
            [](spa_loop*, bool, std::uint32_t, const void* data, std::size_t,
               void*) -> int {
                const auto* queued = static_cast<const Request*>(data);
                pw_stream_queue_buffer(queued->stream, queued->buffer);
                return 0;
            },
            0, &request, sizeof(request), true, nullptr);
    }
};

}  // namespace

void PipeWireCapture::handleDmabuf(pw_buffer* pipewireBuffer) {
    auto* buffer = pipewireBuffer->buffer;
    const auto width = static_cast<int>(format_.size.width);
    const auto height = static_cast<int>(format_.size.height);
    const auto drmFormat = drmFormatFor(format_.format);
    if (drmFormat == 0 || buffer->n_datas > 4) {
        pw_stream_queue_buffer(stream_, pipewireBuffer);
        return;
    }

    DmabufFrame gpu;
    gpu.drmFormat = drmFormat;
    gpu.modifier = format_.modifier;
    gpu.planeCount = buffer->n_datas;
    for (std::uint32_t plane = 0; plane < buffer->n_datas; ++plane) {
        const auto& data = buffer->datas[plane];
        gpu.planes[plane] = DmabufPlane{
            .fd = static_cast<int>(data.fd),
            .offset = data.chunk != nullptr ? data.chunk->offset : 0,
            .stride = data.chunk != nullptr ? static_cast<std::uint32_t>(data.chunk->stride)
                                            : 0,
        };
    }

    CapturedFrame frame;
    frame.format = VideoFormat{
        .width = width,
        .height = height,
        .stride = width * 4,
        .fps = format_.framerate.denom > 0
            ? static_cast<int>(format_.framerate.num / format_.framerate.denom)
            : 60,
        .pixelFormat = *pixelFormat(format_.format),
    };
    frame.capturedAtMs = wallClockMs();
    frame.sequence = sequence_.fetch_add(1);
    frame.dmabuf = gpu;
    // Nothing is copied, so the buffer stays checked out until the encoder is
    // done with it.
    frame.gpuBufferLease = std::make_shared<BufferLease>(
        BufferLease{.handle = handle_, .buffer = pipewireBuffer});
    if (callback_) {
        callback_(std::move(frame));
    }
}

void PipeWireCapture::handleProcess() {
    pw_buffer* pipewireBuffer = pw_stream_dequeue_buffer(stream_);
    if (pipewireBuffer == nullptr) {
        return;
    }
    auto* buffer = pipewireBuffer->buffer;
    if (buffer->n_datas == 0 || buffer->datas[0].chunk == nullptr
        || format_.size.width == 0 || !pool_) {
        pw_stream_queue_buffer(stream_, pipewireBuffer);
        return;
    }

    if (buffer->datas[0].type == SPA_DATA_DmaBuf) {
        handleDmabuf(pipewireBuffer);
        return;
    }
    if (buffer->datas[0].data == nullptr) {
        pw_stream_queue_buffer(stream_, pipewireBuffer);
        return;
    }

    const auto& data = buffer->datas[0];
    const auto* chunk = data.chunk;
    const auto width = static_cast<int>(format_.size.width);
    const auto height = static_cast<int>(format_.size.height);
    const int sourceStride = chunk->stride > 0 ? chunk->stride : width * 4;
    const int targetStride = width * 4;
    const auto required = static_cast<std::uint64_t>(chunk->offset)
        + static_cast<std::uint64_t>(sourceStride) * static_cast<std::uint64_t>(height - 1)
        + static_cast<std::uint64_t>(targetStride);
    if (sourceStride < targetStride || required > data.maxsize) {
        pw_stream_queue_buffer(stream_, pipewireBuffer);
        return;
    }
    const auto* source = static_cast<const char*>(data.data) + chunk->offset;

    CapturedFrame frame;
    frame.format = VideoFormat{
        .width = width,
        .height = height,
        .stride = targetStride,
        .fps = format_.framerate.denom > 0
            ? static_cast<int>(format_.framerate.num / format_.framerate.denom)
            : 60,
        .pixelFormat = *pixelFormat(format_.format),
    };
    frame.capturedAtMs = wallClockMs();
    frame.sequence = sequence_.fetch_add(1);
    const auto frameBytes = static_cast<std::size_t>(targetStride)
        * static_cast<std::size_t>(height);
    frame.bytes = pool_->acquire(frameBytes);
    if (sourceStride == targetStride) {
        // The common case: the producer's rows are already packed, so the
        // frame is one contiguous run. A single memcpy is markedly faster than
        // the same bytes copied a row at a time.
        std::memcpy(frame.bytes.data(), source, frameBytes);
    } else {
        for (int row = 0; row < height; ++row) {
            std::memcpy(frame.bytes.data() + static_cast<std::size_t>(row)
                            * static_cast<std::size_t>(targetStride),
                        source + static_cast<std::size_t>(row)
                            * static_cast<std::size_t>(sourceStride),
                        static_cast<std::size_t>(targetStride));
        }
    }
    pw_stream_queue_buffer(stream_, pipewireBuffer);
    if (callback_) {
        callback_(std::move(frame));
    }
}

}  // namespace od
