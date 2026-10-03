#pragma once

#include "opendisplay/frame_pool.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace od {

enum class TransportKind { Auto, Wifi, Usb };
enum class CaptureMode { Extend, Mirror };
enum class EncoderKind { Auto, Vaapi, Nvenc, Software };
enum class CompositorKind { Auto, Kde, Hyprland };
enum class ExtendDirection { Left, Right, Top, Bottom };
enum class AlignDirection { Left, Right, Top, Bottom, Center };

struct Size {
    int width = 0;
    int height = 0;
};

struct Rect {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
};

struct PhysicalSize {
    double widthMm = 0;
    double heightMm = 0;
};

struct DisplayOptions {
    std::string referenceMonitor;
    ExtendDirection extendTo = ExtendDirection::Right;
    AlignDirection alignTo = AlignDirection::Bottom;
    std::optional<Size> virtualResolution;
    std::optional<double> virtualScale;
    std::optional<Rect> referenceGeometry;
    std::optional<Size> referenceResolution;
    std::optional<double> referenceScale;
    std::optional<PhysicalSize> referencePhysicalSize;
    std::optional<PhysicalSize> receiverPhysicalSize;
    std::optional<int> refreshRate;
};

struct PhoneInfo {
    int pixelsWide = 0;
    int pixelsHigh = 0;
    double scale = 2.0;
    std::string device = "device";
    std::string installId;
    int protocolVersion = 1;
};

struct Endpoint {
    TransportKind kind = TransportKind::Wifi;
    std::string name;
    std::string host;
    std::uint16_t port = 9000;
    std::string udid;
    int usbHandle = -1;
};

struct VideoFormat {
    enum class PixelFormat { Bgra, Bgrx, Rgba, Rgbx };

    int width = 0;
    int height = 0;
    int stride = 0;
    int fps = 60;
    PixelFormat pixelFormat = PixelFormat::Bgra;
};

struct DmabufPlane {
    int fd = -1;
    std::uint32_t offset = 0;
    std::uint32_t stride = 0;
};

/// A frame the compositor left on the GPU.
///
/// The descriptors are borrowed, not owned: they stay valid only while the
/// capture buffer they came from is still checked out, which is what
/// CapturedFrame::gpuBufferLease tracks.
struct DmabufFrame {
    std::uint32_t drmFormat = 0;
    std::uint64_t modifier = 0;
    std::uint32_t planeCount = 0;
    std::array<DmabufPlane, 4> planes{};
};

struct CapturedFrame {
    VideoFormat format;
    std::int64_t capturedAtMs = 0;
    std::uint64_t sequence = 0;
    /// Pixels in system memory. Empty when the frame stayed on the GPU.
    FrameBuffer bytes;
    /// Set instead of `bytes` when the compositor handed over a GPU buffer.
    std::optional<DmabufFrame> dmabuf;
    /// Returns the capture buffer to the compositor when dropped. Held until
    /// the GPU has finished reading the frame, so it must outlive the encode.
    std::shared_ptr<void> gpuBufferLease;
};

struct EncodedFrame {
    std::int64_t capturedAtMs = 0;
    bool keyframe = false;
    std::string annexB;
};

struct EncoderConfig {
    EncoderKind kind = EncoderKind::Auto;
    std::string vaapiDevice = "/dev/dri/renderD128";
    /// Forces an exact encode size. Left at zero, the encoder tracks whatever
    /// the compositor actually captures, scaled by `outputScale`.
    ///
    /// Tracking matters: the virtual output is nudged to produce integer
    /// logical geometry, so it rarely lands on the receiver's native pixels.
    /// Encoding at the receiver's size instead would resample every frame to
    /// cover a handful of pixels -- paying for a full scaling pass and
    /// softening text -- when the receiver can cover the difference with its
    /// own display scaler for nothing.
    int outputWidth = 0;
    int outputHeight = 0;
    double outputScale = 1.0;
    int fps = 60;
    int bitrate = 18'000'000;
};

struct Options {
    TransportKind transport = TransportKind::Auto;
    CaptureMode mode = CaptureMode::Extend;
    EncoderKind encoder = EncoderKind::Auto;
    CompositorKind compositor = CompositorKind::Auto;
    std::string host;
    std::uint16_t port = 9000;
    std::string serviceName;
    std::string udid;
    std::string vaapiDevice = "/dev/dri/renderD128";
    int fps = 60;
    int bitrate = 18'000'000;
    double scale = 1.0;
    DisplayOptions display;
    bool input = true;
    /// Lets capture hand the encoder a compositor GPU buffer instead of a
    /// copy. Falls back on its own when the pieces are not available.
    bool zeroCopy = true;
    bool listDevices = false;
    bool verbose = false;
};

}  // namespace od
