#include "opendisplay/vaapi_encoder.hpp"

#include "opendisplay/annexb.hpp"
#include "opendisplay/log.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavcodec/bsf.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_drm.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
}

#include <drm_fourcc.h>

#include <unistd.h>

#include <cstdio>

#include <algorithm>
#include <chrono>
#include <stdexcept>
#include <vector>

namespace od {
namespace {

std::string averror(const int code) {
    char buffer[AV_ERROR_MAX_STRING_SIZE]{};
    av_strerror(code, buffer, sizeof(buffer));
    return buffer;
}

AVPixelFormat softwareFormat(const VideoFormat::PixelFormat format) {
    switch (format) {
    case VideoFormat::PixelFormat::Bgra: return AV_PIX_FMT_BGRA;
    case VideoFormat::PixelFormat::Bgrx: return AV_PIX_FMT_BGR0;
    case VideoFormat::PixelFormat::Rgba: return AV_PIX_FMT_RGBA;
    case VideoFormat::PixelFormat::Rgbx: return AV_PIX_FMT_RGB0;
    }
    return AV_PIX_FMT_BGRA;
}

}  // namespace

void VaapiEncoder::StageTimings::add(const double milliseconds) {
    samples_[next_] = milliseconds;
    next_ = (next_ + 1) % capacity;
    count_ = std::min(count_ + 1, capacity);
}

double VaapiEncoder::StageTimings::percentile(const double fraction) const {
    if (count_ == 0) {
        return 0.0;
    }
    std::vector<double> sorted(samples_.begin(),
                               samples_.begin() + static_cast<std::ptrdiff_t>(count_));
    std::sort(sorted.begin(), sorted.end());
    const auto index = std::min(
        sorted.size() - 1,
        static_cast<std::size_t>(fraction * static_cast<double>(sorted.size())));
    return sorted[index];
}

void VaapiEncoder::reportTimings() {
    if (!verboseLogging || encodeTimings_.count() == 0) {
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    if (lastReport_.time_since_epoch().count() != 0
        && now - lastReport_ < std::chrono::seconds(5)) {
        return;
    }
    lastReport_ = now;
    const auto frames = encodeTimings_.count();
    const auto format = [](const char* name, const StageTimings& timings) {
        char line[96]{};
        std::snprintf(line, sizeof(line), "%s p50 %.1f p95 %.1f", name,
                      timings.percentile(0.50), timings.percentile(0.95));
        return std::string(line);
    };
    debug("Encoder stages (ms over " + std::to_string(frames) + " frames): "
          + format("surface", mapTimings_) + ", " + format("convert", filterTimings_)
          + ", " + format("encode", encodeTimings_));
    // Start the next window clean, so each line describes the last few
    // seconds instead of everything since the stream opened.
    mapTimings_.reset();
    filterTimings_.reset();
    encodeTimings_.reset();
}

VaapiEncoder::~VaapiEncoder() { stop(); }

void VaapiEncoder::fail(const std::string& message) {
    std::lock_guard lock(mutex_);
    if (error_.empty()) {
        error_ = message;
    }
    running_ = false;
}

std::optional<std::string> VaapiEncoder::error() const {
    std::lock_guard lock(mutex_);
    if (error_.empty()) {
        return std::nullopt;
    }
    return error_;
}

bool VaapiEncoder::lastFrameWasZeroCopy() const {
    std::lock_guard lock(mutex_);
    return zeroCopy_;
}

void VaapiEncoder::openDevice() {
    // Open DRM and derive VA-API from it, rather than opening VA-API
    // directly: only a VA-API device backed by the same DRM device can import
    // the compositor's buffers without a copy.
    int result = av_hwdevice_ctx_create(&drmDevice_, AV_HWDEVICE_TYPE_DRM,
                                        config_.vaapiDevice.c_str(), nullptr, 0);
    if (result < 0) {
        throw std::runtime_error("cannot open DRM device " + config_.vaapiDevice + ": "
                                 + averror(result));
    }
    result = av_hwdevice_ctx_create_derived(&vaapiDevice_, AV_HWDEVICE_TYPE_VAAPI, drmDevice_,
                                            0);
    if (result < 0) {
        throw std::runtime_error("cannot derive a VA-API device: " + averror(result));
    }
}

void VaapiEncoder::openGraphAndEncoder(const VideoFormat& input) {
    closeGraphAndEncoder();
    inputFormat_ = input;
    const auto output = encodeSizeFor(config_, input);
    const int outputWidth = output.width;
    const int outputHeight = output.height;

    inputFrames_ = av_hwframe_ctx_alloc(vaapiDevice_);
    if (inputFrames_ == nullptr) {
        throw std::runtime_error("cannot allocate a VA-API frame pool");
    }
    auto* frames = reinterpret_cast<AVHWFramesContext*>(inputFrames_->data);
    frames->format = AV_PIX_FMT_VAAPI;
    frames->sw_format = softwareFormat(input.pixelFormat);
    frames->width = input.width;
    frames->height = input.height;
    // Surfaces are only needed for the upload path; imported DMA-BUFs bring
    // their own. A small pool covers the frame in hand plus the one encoding.
    frames->initial_pool_size = 4;
    int result = av_hwframe_ctx_init(inputFrames_);
    if (result < 0) {
        throw std::runtime_error("cannot initialize the VA-API frame pool: " + averror(result));
    }

    graph_ = avfilter_graph_alloc();
    if (graph_ == nullptr) {
        throw std::runtime_error("cannot allocate a filter graph");
    }
    // A hardware buffer source must learn its frames context before it is
    // initialized, so it cannot be built from an argument string.
    graphSource_ = avfilter_graph_alloc_filter(graph_, avfilter_get_by_name("buffer"), "in");
    if (graphSource_ == nullptr) {
        throw std::runtime_error("cannot allocate the filter source");
    }
    auto* parameters = av_buffersrc_parameters_alloc();
    parameters->format = AV_PIX_FMT_VAAPI;
    parameters->width = input.width;
    parameters->height = input.height;
    parameters->time_base = AVRational{1, std::max(1, config_.fps)};
    parameters->frame_rate = AVRational{std::max(1, config_.fps), 1};
    parameters->hw_frames_ctx = inputFrames_;
    result = av_buffersrc_parameters_set(graphSource_, parameters);
    av_free(parameters);
    if (result < 0) {
        throw std::runtime_error("cannot configure the filter source: " + averror(result));
    }
    result = avfilter_init_dict(graphSource_, nullptr);
    if (result < 0) {
        throw std::runtime_error("cannot initialize the filter source: " + averror(result));
    }

    // Scale and convert to NV12 on the GPU. Doing either on the CPU costs more
    // than the encode itself on this class of hardware.
    const std::string scaleArgs = "w=" + std::to_string(outputWidth) + ":h="
        + std::to_string(outputHeight) + ":format=nv12";
    AVFilterContext* scale = nullptr;
    result = avfilter_graph_create_filter(&scale, avfilter_get_by_name("scale_vaapi"), "scale",
                                          scaleArgs.c_str(), nullptr, graph_);
    if (result < 0) {
        throw std::runtime_error("cannot create scale_vaapi: " + averror(result));
    }
    result = avfilter_graph_create_filter(&graphSink_, avfilter_get_by_name("buffersink"),
                                          "out", nullptr, nullptr, graph_);
    if (result < 0) {
        throw std::runtime_error("cannot create the filter sink: " + averror(result));
    }
    if ((result = avfilter_link(graphSource_, 0, scale, 0)) < 0
        || (result = avfilter_link(scale, 0, graphSink_, 0)) < 0
        || (result = avfilter_graph_config(graph_, nullptr)) < 0) {
        throw std::runtime_error("cannot configure the filter graph: " + averror(result));
    }

    const AVCodec* encoder = avcodec_find_encoder_by_name("h264_vaapi");
    if (encoder == nullptr) {
        throw std::runtime_error("FFmpeg does not provide h264_vaapi");
    }
    codec_ = avcodec_alloc_context3(encoder);
    if (codec_ == nullptr) {
        throw std::runtime_error("cannot allocate the encoder context");
    }
    codec_->width = outputWidth;
    codec_->height = outputHeight;
    codec_->time_base = AVRational{1, std::max(1, config_.fps)};
    codec_->framerate = AVRational{std::max(1, config_.fps), 1};
    codec_->pix_fmt = AV_PIX_FMT_VAAPI;
    codec_->bit_rate = config_.bitrate;
    codec_->rc_max_rate = config_.bitrate;
    codec_->rc_buffer_size = std::max(config_.bitrate / 2, 1);
    codec_->max_b_frames = 0;
    codec_->gop_size = std::max(config_.fps * 60, config_.fps);
    codec_->hw_frames_ctx = av_buffer_ref(av_buffersink_get_hw_frames_ctx(graphSink_));
    if (codec_->hw_frames_ctx == nullptr) {
        throw std::runtime_error("the filter graph produced no hardware frames context");
    }
    // async_depth=1 keeps the encoder from withholding output until further
    // frames arrive, which is what makes a synchronous encode viable.
    av_opt_set_int(codec_->priv_data, "async_depth", 1, 0);
    result = avcodec_open2(codec_, encoder, nullptr);
    if (result < 0) {
        throw std::runtime_error("cannot open h264_vaapi: " + averror(result));
    }
    log("Encoding " + std::to_string(input.width) + 'x' + std::to_string(input.height)
        + " to " + std::to_string(outputWidth) + 'x' + std::to_string(outputHeight)
        + (input.width == outputWidth && input.height == outputHeight
               ? " (convert only, no rescale)"
               : " (rescaling)"));

    // The same bitstream filters the subprocess used, so the receiver sees an
    // identical stream: access-unit delimiters present, and parameter sets
    // repeated on every keyframe for a decoder that joins late.
    result = av_bsf_list_parse_str("h264_metadata=aud=insert,dump_extra=freq=keyframe",
                                   &bitstream_);
    if (result < 0) {
        throw std::runtime_error("cannot build the bitstream filter: " + averror(result));
    }
    result = avcodec_parameters_from_context(bitstream_->par_in, codec_);
    if (result < 0) {
        throw std::runtime_error("cannot configure the bitstream filter: " + averror(result));
    }
    bitstream_->time_base_in = codec_->time_base;
    result = av_bsf_init(bitstream_);
    if (result < 0) {
        throw std::runtime_error("cannot initialize the bitstream filter: " + averror(result));
    }

    packet_ = av_packet_alloc();
    if (packet_ == nullptr) {
        throw std::runtime_error("cannot allocate a packet");
    }
}

void VaapiEncoder::closeGraphAndEncoder() {
    if (bitstream_ != nullptr) {
        av_bsf_free(&bitstream_);
    }
    if (codec_ != nullptr) {
        avcodec_free_context(&codec_);
    }
    if (graph_ != nullptr) {
        avfilter_graph_free(&graph_);
        graphSource_ = nullptr;
        graphSink_ = nullptr;
    }
    if (uploadStaging_ != nullptr) {
        av_frame_free(&uploadStaging_);
    }
    if (inputFrames_ != nullptr) {
        av_buffer_unref(&inputFrames_);
    }
    if (packet_ != nullptr) {
        av_packet_free(&packet_);
    }
    inputFormat_ = {};
}

AVFrame* VaapiEncoder::surfaceFor(const CapturedFrame& frame) {
    if (frame.dmabuf) {
        // Zero copy: describe the compositor's buffer and map it straight into
        // a VA-API surface. Nothing is read or written by the CPU.
        const auto& source = *frame.dmabuf;
        // The mapped surface keeps a reference to this frame, so the
        // descriptor has to outlive the call and be owned by it. A stack
        // descriptor corrupts the heap the moment the surface is used.
        auto* owned = static_cast<AVDRMFrameDescriptor*>(av_mallocz(sizeof(AVDRMFrameDescriptor)));
        if (owned == nullptr) {
            return nullptr;
        }
        AVDRMFrameDescriptor& descriptor = *owned;
        descriptor.nb_layers = 1;
        descriptor.layers[0].format = source.drmFormat;
        descriptor.layers[0].nb_planes = static_cast<int>(std::min(source.planeCount, 4U));
        for (int plane = 0; plane < descriptor.layers[0].nb_planes; ++plane) {
            const auto& incoming = source.planes[static_cast<std::size_t>(plane)];
            // Planes may share one dmabuf or arrive on separate fds, so each
            // distinct fd becomes one object and planes point at it.
            int object = -1;
            for (int existing = 0; existing < descriptor.nb_objects; ++existing) {
                if (descriptor.objects[existing].fd == incoming.fd) {
                    object = existing;
                    break;
                }
            }
            if (object < 0) {
                object = descriptor.nb_objects++;
                descriptor.objects[object].fd = incoming.fd;
                // libva needs the real buffer size to import; a dmabuf
                // reports it by seeking to the end.
                const auto measured = ::lseek(incoming.fd, 0, SEEK_END);
                descriptor.objects[object].size = measured > 0
                    ? static_cast<std::size_t>(measured)
                    : static_cast<std::size_t>(incoming.stride)
                        * static_cast<std::size_t>(frame.format.height);
                descriptor.objects[object].format_modifier = source.modifier;
            }
            descriptor.layers[0].planes[plane].object_index = object;
            descriptor.layers[0].planes[plane].offset = incoming.offset;
            descriptor.layers[0].planes[plane].pitch = incoming.stride;
        }

        AVFrame* drm = av_frame_alloc();
        if (drm == nullptr) {
            av_free(owned);
            return nullptr;
        }
        drm->format = AV_PIX_FMT_DRM_PRIME;
        drm->width = frame.format.width;
        drm->height = frame.format.height;
        drm->data[0] = reinterpret_cast<std::uint8_t*>(owned);
        drm->buf[0] = av_buffer_create(
            reinterpret_cast<std::uint8_t*>(owned), sizeof(AVDRMFrameDescriptor),
            [](void*, std::uint8_t* data) { av_free(data); }, nullptr, 0);
        if (drm->buf[0] == nullptr) {
            av_free(owned);
            av_frame_free(&drm);
            return nullptr;
        }

        AVFrame* surface = av_frame_alloc();
        if (surface == nullptr) {
            av_frame_free(&drm);
            return nullptr;
        }
        surface->format = AV_PIX_FMT_VAAPI;
        surface->width = frame.format.width;
        surface->height = frame.format.height;
        surface->hw_frames_ctx = av_buffer_ref(inputFrames_);
        const int result = av_hwframe_map(surface, drm,
                                          AV_HWFRAME_MAP_READ | AV_HWFRAME_MAP_DIRECT);
        av_frame_free(&drm);
        if (result < 0) {
            debug("DMA-BUF import failed, falling back to upload: " + averror(result));
            av_frame_free(&surface);
            return nullptr;
        }
        return surface;
    }

    // Upload path: still better than a pipe, but it does cross the bus.
    if (frame.bytes.empty()) {
        return nullptr;
    }
    if (uploadStaging_ == nullptr) {
        uploadStaging_ = av_frame_alloc();
        if (uploadStaging_ == nullptr) {
            return nullptr;
        }
    }
    uploadStaging_->format = softwareFormat(frame.format.pixelFormat);
    uploadStaging_->width = frame.format.width;
    uploadStaging_->height = frame.format.height;
    // Point libav at the captured bytes rather than copying them in: the
    // upload reads straight from the capture buffer.
    uploadStaging_->data[0] = reinterpret_cast<std::uint8_t*>(
        const_cast<char*>(frame.bytes.data()));
    uploadStaging_->linesize[0] = frame.format.stride;

    AVFrame* surface = av_frame_alloc();
    if (surface == nullptr) {
        return nullptr;
    }
    int result = av_hwframe_get_buffer(inputFrames_, surface, 0);
    if (result < 0) {
        av_frame_free(&surface);
        return nullptr;
    }
    result = av_hwframe_transfer_data(surface, uploadStaging_, 0);
    if (result < 0) {
        debug("VA-API upload failed: " + averror(result));
        av_frame_free(&surface);
        return nullptr;
    }
    return surface;
}

void VaapiEncoder::drainPackets() {
    for (;;) {
        int result = avcodec_receive_packet(codec_, packet_);
        if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) {
            return;
        }
        if (result < 0) {
            fail("encoder failed: " + averror(result));
            return;
        }
        result = av_bsf_send_packet(bitstream_, packet_);
        av_packet_unref(packet_);
        if (result < 0) {
            fail("bitstream filter rejected a packet: " + averror(result));
            return;
        }
        for (;;) {
            result = av_bsf_receive_packet(bitstream_, packet_);
            if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) {
                break;
            }
            if (result < 0) {
                fail("bitstream filter failed: " + averror(result));
                return;
            }
            std::string accessUnit = annexb::normalize(std::string_view(
                reinterpret_cast<const char*>(packet_->data),
                static_cast<std::size_t>(packet_->size)));
            const bool keyframe = annexb::containsKeyframe(accessUnit);
            av_packet_unref(packet_);
            if (callback_ && !accessUnit.empty()) {
                callback_(EncodedFrame{.capturedAtMs = pendingCapturedAtMs_,
                                       .keyframe = keyframe,
                                       .annexB = std::move(accessUnit)});
            }
        }
    }
}

void VaapiEncoder::encodeOne(CapturedFrame frame) {
    const bool formatChanged = codec_ != nullptr
        && (frame.format.width != inputFormat_.width
            || frame.format.height != inputFormat_.height
            || frame.format.pixelFormat != inputFormat_.pixelFormat);
    if (codec_ == nullptr || formatChanged) {
        openGraphAndEncoder(frame.format);
    }

    using Clock = std::chrono::steady_clock;
    const auto elapsedMs = [](const Clock::time_point from) {
        return std::chrono::duration<double, std::milli>(Clock::now() - from).count();
    };

    const auto mapStart = Clock::now();
    AVFrame* surface = surfaceFor(frame);
    if (surface == nullptr) {
        return;
    }
    mapTimings_.add(elapsedMs(mapStart));
    {
        std::lock_guard lock(mutex_);
        zeroCopy_ = frame.dmabuf.has_value();
    }
    surface->pts = nextPts_++;
    pendingCapturedAtMs_ = frame.capturedAtMs;

    const auto filterStart = Clock::now();
    int result = av_buffersrc_add_frame_flags(graphSource_, surface,
                                              AV_BUFFERSRC_FLAG_KEEP_REF);
    av_frame_free(&surface);
    if (result < 0) {
        fail("cannot push a frame into the filter graph: " + averror(result));
        return;
    }

    AVFrame* converted = av_frame_alloc();
    if (converted == nullptr) {
        return;
    }
    result = av_buffersink_get_frame(graphSink_, converted);
    if (result < 0) {
        av_frame_free(&converted);
        if (result != AVERROR(EAGAIN)) {
            fail("cannot read from the filter graph: " + averror(result));
        }
        return;
    }
    filterTimings_.add(elapsedMs(filterStart));
    bool forceKeyframe = false;
    {
        std::lock_guard lock(mutex_);
        forceKeyframe = restartRequested_;
        restartRequested_ = false;
    }
    if (forceKeyframe) {
        converted->pict_type = AV_PICTURE_TYPE_I;
    }
    const auto encodeStart = Clock::now();
    result = avcodec_send_frame(codec_, converted);
    av_frame_free(&converted);
    if (result < 0) {
        fail("encoder rejected a frame: " + averror(result));
        return;
    }
    // The capture buffer is released when `frame` goes out of scope below,
    // once the GPU work that reads it has been queued behind the encode.
    drainPackets();
    encodeTimings_.add(elapsedMs(encodeStart));
    reportTimings();
}

void VaapiEncoder::run() {
    for (;;) {
        CapturedFrame frame;
        {
            std::unique_lock lock(mutex_);
            condition_.wait(lock, [&] { return !running_ || pending_.has_value(); });
            if (!running_) {
                break;
            }
            frame = std::move(*pending_);
            pending_.reset();
        }
        try {
            encodeOne(std::move(frame));
        } catch (const std::exception& problem) {
            fail(problem.what());
            break;
        }
    }
    // The graph and encoder are only ever touched from this thread, so they
    // are torn down here rather than in stop().
    closeGraphAndEncoder();
}

void VaapiEncoder::probe(const EncoderConfig& config) {
    config_ = config;
    openDevice();
    av_buffer_unref(&vaapiDevice_);
    av_buffer_unref(&drmDevice_);
}

void VaapiEncoder::start(EncoderConfig config, FrameCallback callback) {
    stop();
    config_ = std::move(config);
    callback_ = std::move(callback);
    nextPts_ = 0;
    {
        std::lock_guard lock(mutex_);
        running_ = true;
        restartRequested_ = false;
        error_.clear();
    }
    // Bring the device up on this thread so a machine without a usable VA-API
    // device fails start() and the caller can fall back, rather than failing
    // silently on the worker a moment later.
    try {
        openDevice();
    } catch (...) {
        std::lock_guard lock(mutex_);
        running_ = false;
        throw;
    }
    worker_ = std::thread(&VaapiEncoder::run, this);
    log("Using in-process h264_vaapi");
}

void VaapiEncoder::submit(CapturedFrame frame) {
    {
        std::lock_guard lock(mutex_);
        if (!running_) {
            return;
        }
        pending_ = std::move(frame);
    }
    condition_.notify_one();
}

void VaapiEncoder::requestKeyframe() {
    std::lock_guard lock(mutex_);
    restartRequested_ = true;
}

void VaapiEncoder::stop() {
    {
        std::lock_guard lock(mutex_);
        running_ = false;
        pending_.reset();
    }
    condition_.notify_all();
    if (worker_.joinable()) {
        worker_.join();
    }
    if (vaapiDevice_ != nullptr) {
        av_buffer_unref(&vaapiDevice_);
    }
    if (drmDevice_ != nullptr) {
        av_buffer_unref(&drmDevice_);
    }
    callback_ = {};
}

}  // namespace od
