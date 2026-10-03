#include "opendisplay/ffmpeg_encoder.hpp"
#include "opendisplay/log.hpp"
#include "opendisplay/wire.hpp"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string_view>
#include <thread>
#include <vector>

namespace {

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

}  // namespace

/// Feeding faster than the encoder drains must cost dropped frames, not a
/// growing queue, and must never stall. The flow-control credit is returned
/// when an access unit comes back; if it is ever leaked the writer falls back
/// to its timeout on every single frame, which caps throughput at one frame
/// per timeout no matter how fast the encoder actually is. The frame size here
/// is deliberately small so a healthy encoder clears it by a wide margin on
/// any machine, while the broken ceiling stays fixed by the timeout.
void boundsLatencyWhenCaptureOutrunsTheEncoder() {
    constexpr int width = 960;
    constexpr int height = 540;
    constexpr int fps = 60;
    constexpr auto duration = std::chrono::seconds(3);
    std::mutex mutex;
    std::vector<long long> latencies;
    od::FfmpegEncoder encoder;
    encoder.start(od::EncoderConfig{
        .kind = od::EncoderKind::Software,
        .outputWidth = width,
        .outputHeight = height,
        .fps = fps,
        .bitrate = 4'000'000,
    }, [&](od::EncodedFrame frame) {
        std::lock_guard lock(mutex);
        latencies.push_back(od::wallClockMs() - frame.capturedAtMs);
    });

    std::vector<char> pixels(static_cast<std::size_t>(width) * height * 4);
    const auto deadline = std::chrono::steady_clock::now() + duration;
    unsigned sequence = 0;
    while (std::chrono::steady_clock::now() < deadline) {
        od::CapturedFrame frame;
        frame.format = od::VideoFormat{.width = width, .height = height,
                                       .stride = width * 4, .fps = fps};
        frame.capturedAtMs = od::wallClockMs();
        frame.sequence = sequence++;
        // Vary the content so the encoder cannot coast on unchanged frames.
        for (std::size_t index = 0; index < pixels.size(); index += 97) {
            pixels[index] = static_cast<char>(sequence * 7 + index);
        }
        frame.bytes.assign(pixels.begin(), pixels.end());
        encoder.submit(std::move(frame));
    }
    encoder.stop();

    std::lock_guard lock(mutex);
    // One frame per timeout is what a leaked credit degrades to. Three seconds
    // of 50 ms timeouts is about 60 frames, so anything near that means the
    // writer is waiting rather than being released by returning access units.
    assert(latencies.size() > 90);
    // A bounded queue keeps the worst case near the encode time itself.
    const auto worst = *std::max_element(latencies.begin(), latencies.end());
    assert(worst < 1500);
    // Late frames must not be far staler than early ones: that is queue growth.
    const auto third = latencies.size() / 3;
    auto median = [&](const std::size_t from, const std::size_t to) {
        std::vector<long long> window(latencies.begin() + static_cast<long>(from),
                                      latencies.begin() + static_cast<long>(to));
        std::sort(window.begin(), window.end());
        return window[window.size() / 2];
    };
    assert(median(latencies.size() - third, latencies.size())
           <= median(0, third) * 3 + 250);
}

int main() {
    std::mutex mutex;
    std::condition_variable condition;
    std::vector<od::EncodedFrame> output;
    od::FfmpegEncoder encoder;
    encoder.start(od::EncoderConfig{
        .kind = od::EncoderKind::Software,
        .outputWidth = 64,
        .outputHeight = 64,
        .fps = 30,
        .bitrate = 300'000,
    }, [&](od::EncodedFrame frame) {
        {
            std::lock_guard lock(mutex);
            output.push_back(std::move(frame));
        }
        condition.notify_all();
    });

    for (int index = 0; index < 8; ++index) {
        od::CapturedFrame frame;
        frame.format = od::VideoFormat{.width = 64, .height = 64, .stride = 256,
                                       .fps = 30};
        frame.capturedAtMs = 1000 + index;
        frame.sequence = static_cast<unsigned>(index);
        frame.bytes.assign(64 * 64 * 4, static_cast<char>(index * 20));
        encoder.submit(std::move(frame));
        std::this_thread::sleep_for(std::chrono::milliseconds(40));
    }
    {
        std::unique_lock lock(mutex);
        condition.wait_for(lock, std::chrono::seconds(5), [&] { return !output.empty(); });
    }
    encoder.stop();
    assert(!output.empty());
    assert(od::wire::containsAnnexBStartCode(output.front().annexB));
    assert(output.front().keyframe);
    assert(hasNalType(output.front().annexB, 7));  // SPS
    assert(hasNalType(output.front().annexB, 8));  // PPS
    assert(hasNalType(output.front().annexB, 5));  // IDR slice
    for (const auto& encoded : output) {
        assert(hasOnlyFourByteStartCodes(encoded.annexB));
    }

    boundsLatencyWhenCaptureOutrunsTheEncoder();
    return 0;
}
