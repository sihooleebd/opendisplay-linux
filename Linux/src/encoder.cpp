#include "opendisplay/encoder.hpp"

#include "opendisplay/ffmpeg_encoder.hpp"
#include "opendisplay/log.hpp"
#include "opendisplay/vaapi_encoder.hpp"

#include <unistd.h>

namespace od {

std::unique_ptr<Encoder> makeEncoder(const EncoderConfig& config, const bool allowZeroCopy) {
    const bool wantsVaapi = config.kind == EncoderKind::Vaapi
        || (config.kind == EncoderKind::Auto
            && ::access(config.vaapiDevice.c_str(), R_OK | W_OK) == 0);
    if (wantsVaapi && allowZeroCopy) {
        auto candidate = std::make_unique<VaapiEncoder>();
        try {
            // start() opens the device, so a machine that cannot do VA-API
            // reports it here rather than failing silently mid-stream.
            candidate->probe(config);
            return candidate;
        } catch (const std::exception& problem) {
            log(std::string("In-process VA-API unavailable (") + problem.what()
                + "); using the FFmpeg subprocess");
        }
    }
    return std::make_unique<FfmpegEncoder>();
}

}  // namespace od
