#include "opendisplay/vaapi_encoder.hpp"

#include "opendisplay/frame_pool.hpp"
#include "opendisplay/log.hpp"

extern "C" {
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_drm.h>
#include <libavutil/frame.h>
}

#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string_view>
#include <thread>
#include <vector>

namespace {

constexpr int kWidth = 640;
constexpr int kHeight = 480;

bool hasOnlyFourByteStartCodes(const std::string_view bytes) {
    for (std::size_t index = 0; index + 2 < bytes.size(); ++index) {
        if (bytes[index] == 0 && bytes[index + 1] == 0 && bytes[index + 2] == 1
            && (index == 0 || bytes[index - 1] != 0)) {
            return false;
        }
    }
    return true;
}

bool hasNalType(const std::string_view bytes, const unsigned char type) {
    for (std::size_t index = 0; index + 4 < bytes.size(); ++index) {
        if (bytes[index] == 0 && bytes[index + 1] == 0 && bytes[index + 2] == 0
            && bytes[index + 3] == 1
            && (static_cast<unsigned char>(bytes[index + 4]) & 0x1fU) == type) {
            return true;
        }
    }
    return false;
}

struct Collected {
    std::mutex mutex;
    std::condition_variable condition;
    std::vector<od::EncodedFrame> frames;

    void add(od::EncodedFrame frame) {
        {
            std::lock_guard lock(mutex);
            frames.push_back(std::move(frame));
        }
        condition.notify_all();
    }

    bool waitFor(const std::size_t count, const std::chrono::seconds timeout) {
        std::unique_lock lock(mutex);
        return condition.wait_for(lock, timeout, [&] { return frames.size() >= count; });
    }
};

/// The upload path: pixels in system memory, encoded in-process. This is what
/// runs when the compositor will not hand over a GPU buffer.
void encodesFramesFromSystemMemory() {
    od::VaapiEncoder encoder;
    Collected collected;
    encoder.start(od::EncoderConfig{.kind = od::EncoderKind::Vaapi,
                                    .outputWidth = kWidth,
                                    .outputHeight = kHeight,
                                    .fps = 30,
                                    .bitrate = 4'000'000},
                  [&](od::EncodedFrame frame) { collected.add(std::move(frame)); });

    const auto pool = od::FramePool::create();
    const auto frameBytes = static_cast<std::size_t>(kWidth) * kHeight * 4;
    for (int index = 0; index < 8; ++index) {
        od::CapturedFrame frame;
        frame.format = od::VideoFormat{.width = kWidth, .height = kHeight,
                                       .stride = kWidth * 4, .fps = 30,
                                       .pixelFormat = od::VideoFormat::PixelFormat::Bgrx};
        frame.capturedAtMs = 5000 + index;
        frame.sequence = static_cast<unsigned>(index);
        frame.bytes = pool->acquire(frameBytes);
        // Vary the content so the encoder cannot coast on identical frames.
        std::memset(frame.bytes.data(), index * 24 + 7, frameBytes);
        encoder.submit(std::move(frame));
        std::this_thread::sleep_for(std::chrono::milliseconds(40));
    }
    const bool arrived = collected.waitFor(1, std::chrono::seconds(10));
    assert(!encoder.error());
    assert(arrived);

    std::lock_guard lock(collected.mutex);
    const auto& first = collected.frames.front();
    // The receiver only accepts four-byte delimiters, and needs parameter
    // sets alongside the IDR to start decoding.
    assert(hasOnlyFourByteStartCodes(first.annexB));
    assert(hasNalType(first.annexB, 7));  // SPS
    assert(hasNalType(first.annexB, 8));  // PPS
    assert(hasNalType(first.annexB, 5));  // IDR slice
    assert(first.keyframe);
    // Capture timestamps must survive the trip, or latency telemetry lies.
    assert(first.capturedAtMs >= 5000);
    assert(!encoder.lastFrameWasZeroCopy());
    printf("  upload path: %zu access unit(s), first %zu bytes\n", collected.frames.size(),
           first.annexB.size());
}

/// The zero-copy path. A VA-API surface exported to DRM PRIME stands in for
/// the compositor's buffer, which is the same thing PipeWire hands over.
void encodesFramesImportedFromADmabuf() {
    AVBufferRef* drmDevice = nullptr;
    if (av_hwdevice_ctx_create(&drmDevice, AV_HWDEVICE_TYPE_DRM, "/dev/dri/renderD128",
                               nullptr, 0) < 0) {
        printf("  dmabuf path: no DRM device, skipping\n");
        return;
    }
    AVBufferRef* vaapiDevice = nullptr;
    if (av_hwdevice_ctx_create_derived(&vaapiDevice, AV_HWDEVICE_TYPE_VAAPI, drmDevice, 0)
        < 0) {
        av_buffer_unref(&drmDevice);
        printf("  dmabuf path: no VA-API device, skipping\n");
        return;
    }
    AVBufferRef* frames = av_hwframe_ctx_alloc(vaapiDevice);
    auto* context = reinterpret_cast<AVHWFramesContext*>(frames->data);
    context->format = AV_PIX_FMT_VAAPI;
    context->sw_format = AV_PIX_FMT_BGR0;
    context->width = kWidth;
    context->height = kHeight;
    context->initial_pool_size = 2;
    assert(av_hwframe_ctx_init(frames) >= 0);

    AVFrame* surface = av_frame_alloc();
    assert(av_hwframe_get_buffer(frames, surface, 0) >= 0);
    AVFrame* exported = av_frame_alloc();
    exported->format = AV_PIX_FMT_DRM_PRIME;
    const int mapped = av_hwframe_map(exported, surface,
                                      AV_HWFRAME_MAP_READ | AV_HWFRAME_MAP_DIRECT);
    if (mapped < 0) {
        printf("  dmabuf path: cannot export a surface, skipping\n");
        av_frame_free(&exported);
        av_frame_free(&surface);
        av_buffer_unref(&frames);
        av_buffer_unref(&vaapiDevice);
        av_buffer_unref(&drmDevice);
        return;
    }
    const auto* descriptor = reinterpret_cast<const AVDRMFrameDescriptor*>(exported->data[0]);

    od::VaapiEncoder encoder;
    Collected collected;
    encoder.start(od::EncoderConfig{.kind = od::EncoderKind::Vaapi,
                                    .outputWidth = kWidth,
                                    .outputHeight = kHeight,
                                    .fps = 30,
                                    .bitrate = 4'000'000},
                  [&](od::EncodedFrame frame) { collected.add(std::move(frame)); });

    for (int index = 0; index < 4; ++index) {
        od::CapturedFrame frame;
        frame.format = od::VideoFormat{.width = kWidth, .height = kHeight,
                                       .stride = kWidth * 4, .fps = 30,
                                       .pixelFormat = od::VideoFormat::PixelFormat::Bgrx};
        frame.capturedAtMs = 9000 + index;
        od::DmabufFrame gpu;
        gpu.drmFormat = descriptor->layers[0].format;
        gpu.modifier = descriptor->objects[0].format_modifier;
        gpu.planeCount = static_cast<std::uint32_t>(descriptor->layers[0].nb_planes);
        for (int plane = 0; plane < descriptor->layers[0].nb_planes && plane < 4; ++plane) {
            gpu.planes[static_cast<std::size_t>(plane)] = od::DmabufPlane{
                .fd = descriptor->objects[descriptor->layers[0].planes[plane].object_index].fd,
                .offset = static_cast<std::uint32_t>(descriptor->layers[0].planes[plane].offset),
                .stride = static_cast<std::uint32_t>(descriptor->layers[0].planes[plane].pitch),
            };
        }
        frame.dmabuf = gpu;
        encoder.submit(std::move(frame));
        std::this_thread::sleep_for(std::chrono::milliseconds(40));
    }
    const bool arrived = collected.waitFor(1, std::chrono::seconds(10));
    if (const auto problem = encoder.error()) {
        printf("  dmabuf path: encoder reported '%s'\n", problem->c_str());
    }
    assert(arrived);
    {
        std::lock_guard lock(collected.mutex);
        const auto& first = collected.frames.front();
        assert(hasOnlyFourByteStartCodes(first.annexB));
        assert(hasNalType(first.annexB, 5));
        assert(first.keyframe);
        printf("  dmabuf path: %zu access unit(s), first %zu bytes, zero copy\n",
               collected.frames.size(), first.annexB.size());
    }
    // The frame really went through the import path, not the upload fallback.
    assert(encoder.lastFrameWasZeroCopy());
    encoder.stop();

    av_frame_free(&exported);
    av_frame_free(&surface);
    av_buffer_unref(&frames);
    av_buffer_unref(&vaapiDevice);
    av_buffer_unref(&drmDevice);
}

}  // namespace

int main() {
    od::verboseLogging = true;
    // Without a usable VA-API device there is nothing to assert; the
    // subprocess encoder covers that configuration.
    AVBufferRef* probe = nullptr;
    if (av_hwdevice_ctx_create(&probe, AV_HWDEVICE_TYPE_VAAPI, "/dev/dri/renderD128", nullptr,
                               0) < 0) {
        printf("no VA-API device available; skipping in-process encoder tests\n");
        return 0;
    }
    av_buffer_unref(&probe);

    encodesFramesFromSystemMemory();
    encodesFramesImportedFromADmabuf();
    return 0;
}
