#pragma once

#include "opendisplay/types.hpp"

#include <algorithm>
#include <cmath>
#include <functional>
#include <memory>
#include <string>

namespace od {

/// H.264 encoder fed by the capture thread.
///
/// Implementations drop stale frames rather than queue them: the capture
/// thread only ever replaces a single pending frame, so a slow encoder costs
/// frames instead of unbounded latency.
class Encoder {
public:
    using FrameCallback = std::function<void(EncodedFrame)>;

    Encoder() = default;
    virtual ~Encoder() = default;
    Encoder(const Encoder&) = delete;
    Encoder& operator=(const Encoder&) = delete;

    virtual void start(EncoderConfig config, FrameCallback callback) = 0;
    virtual void submit(CapturedFrame frame) = 0;
    virtual void requestKeyframe() = 0;
    virtual void stop() = 0;
    [[nodiscard]] virtual std::string selectedEncoder() const = 0;
    /// True when this encoder can take a compositor GPU buffer directly. Only
    /// then is it worth asking PipeWire for one.
    [[nodiscard]] virtual bool acceptsDmabuf() const = 0;
};

/// Picks the in-process VA-API encoder when the configuration allows it,
/// falling back to the FFmpeg subprocess. Never throws: a failure to bring up
/// VA-API degrades to the subprocess rather than failing the session.
std::unique_ptr<Encoder> makeEncoder(const EncoderConfig& config, bool allowZeroCopy);

/// Encode dimensions for a given captured format. H.264 needs even numbers,
/// and a zero override means "track the capture".
inline Size encodeSizeFor(const EncoderConfig& config, const VideoFormat& input) {
    const double scale = config.outputScale > 0 ? config.outputScale : 1.0;
    const int width = config.outputWidth > 0
        ? config.outputWidth
        : static_cast<int>(std::lround(input.width * scale));
    const int height = config.outputHeight > 0
        ? config.outputHeight
        : static_cast<int>(std::lround(input.height * scale));
    return Size{.width = std::max(2, width) & ~1, .height = std::max(2, height) & ~1};
}

}  // namespace od
