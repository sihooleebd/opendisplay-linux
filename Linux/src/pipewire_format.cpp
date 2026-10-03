#include "opendisplay/pipewire_format.hpp"

#include <spa/buffer/buffer.h>
#include <spa/param/format-utils.h>
#include <spa/param/param.h>
#include <spa/param/video/raw.h>

#include <algorithm>
#include <cstdint>

namespace od {

const spa_pod* buildPipeWireFormatOffer(spa_pod_builder& builder, const int width,
                                        const int height, const int fps) {
    const auto requestedWidth = static_cast<std::uint32_t>(std::max(1, width));
    const auto requestedHeight = static_cast<std::uint32_t>(std::max(1, height));
    const auto requestedFps = static_cast<std::uint32_t>(std::max(1, fps));
    const spa_rectangle requestedSize = SPA_RECTANGLE(requestedWidth, requestedHeight);
    const spa_rectangle minimumSize = SPA_RECTANGLE(1, 1);
    const spa_rectangle maximumSize = SPA_RECTANGLE(16384, 16384);
    const spa_fraction requestedRate = SPA_FRACTION(requestedFps, 1);
    const spa_fraction minimumRate = SPA_FRACTION(0, 1);
    const spa_fraction maximumRate = SPA_FRACTION(std::max(requestedFps, 240U), 1);

    return static_cast<spa_pod*>(spa_pod_builder_add_object(
        &builder,
        SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat,
        SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video),
        SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
        // KWin commonly exposes BGRx, while other producers use BGRA/RGBx/RGBA.
        // Every advertised layout is four bytes per pixel and is supported by
        // the FFmpeg input mapping.
        SPA_FORMAT_VIDEO_format, SPA_POD_CHOICE_ENUM_Id(
            5, SPA_VIDEO_FORMAT_BGRx, SPA_VIDEO_FORMAT_BGRx, SPA_VIDEO_FORMAT_BGRA,
            SPA_VIDEO_FORMAT_RGBx, SPA_VIDEO_FORMAT_RGBA),
        // The portal decides the actual virtual-output mode. The encoder scales
        // that mode to the receiver's requested output dimensions.
        SPA_FORMAT_VIDEO_size,
        SPA_POD_CHOICE_RANGE_Rectangle(&requestedSize, &minimumSize, &maximumSize),
        SPA_FORMAT_VIDEO_framerate,
        SPA_POD_CHOICE_RANGE_Fraction(&requestedRate, &minimumRate, &maximumRate)));
}

const spa_pod* buildPipeWireDmabufOffer(spa_pod_builder& builder, const int width,
                                        const int height, const int fps,
                                        const std::uint64_t* modifiers,
                                        const std::size_t modifierCount) {
    const auto requestedWidth = static_cast<std::uint32_t>(std::max(1, width));
    const auto requestedHeight = static_cast<std::uint32_t>(std::max(1, height));
    const auto requestedFps = static_cast<std::uint32_t>(std::max(1, fps));
    const spa_rectangle requestedSize = SPA_RECTANGLE(requestedWidth, requestedHeight);
    const spa_rectangle minimumSize = SPA_RECTANGLE(1, 1);
    const spa_rectangle maximumSize = SPA_RECTANGLE(16384, 16384);
    const spa_fraction requestedRate = SPA_FRACTION(requestedFps, 1);
    const spa_fraction minimumRate = SPA_FRACTION(0, 1);
    const spa_fraction maximumRate = SPA_FRACTION(std::max(requestedFps, 240U), 1);

    spa_pod_frame objectFrame{};
    spa_pod_builder_push_object(&builder, &objectFrame, SPA_TYPE_OBJECT_Format,
                                SPA_PARAM_EnumFormat);
    spa_pod_builder_add(&builder,
                        SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video),
                        SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
                        // NV12 is deliberately not offered, despite the encoder
                        // wanting it and zero-copy making multi-plane buffers
                        // workable. Measured against xdg-desktop-portal-hyprland:
                        // listing NV12 first still negotiates BGRA, because a
                        // wlroots compositor shares its framebuffer rather than
                        // converting for a consumer. The colour conversion has to
                        // happen on this side either way.
                        SPA_FORMAT_VIDEO_format,
                        SPA_POD_CHOICE_ENUM_Id(5, SPA_VIDEO_FORMAT_BGRx,
                                               SPA_VIDEO_FORMAT_BGRx, SPA_VIDEO_FORMAT_BGRA,
                                               SPA_VIDEO_FORMAT_RGBx, SPA_VIDEO_FORMAT_RGBA),
                        0);

    if (modifierCount > 0 && modifiers != nullptr) {
        // MANDATORY marks the property as required for a DMA-BUF negotiation;
        // DONT_FIXATE leaves the producer free to choose from the list, which
        // it then reports back for the consumer to confirm.
        spa_pod_builder_prop(&builder, SPA_FORMAT_VIDEO_modifier,
                             SPA_POD_PROP_FLAG_MANDATORY | SPA_POD_PROP_FLAG_DONT_FIXATE);
        spa_pod_frame choiceFrame{};
        spa_pod_builder_push_choice(&builder, &choiceFrame, SPA_CHOICE_Enum, 0);
        // A choice repeats its preferred value first.
        spa_pod_builder_long(&builder, static_cast<std::int64_t>(modifiers[0]));
        for (std::size_t index = 0; index < modifierCount; ++index) {
            spa_pod_builder_long(&builder, static_cast<std::int64_t>(modifiers[index]));
        }
        spa_pod_builder_pop(&builder, &choiceFrame);
    }

    spa_pod_builder_add(&builder,
                        SPA_FORMAT_VIDEO_size,
                        SPA_POD_CHOICE_RANGE_Rectangle(&requestedSize, &minimumSize,
                                                       &maximumSize),
                        SPA_FORMAT_VIDEO_framerate,
                        SPA_POD_CHOICE_RANGE_Fraction(&requestedRate, &minimumRate,
                                                      &maximumRate),
                        0);
    return static_cast<const spa_pod*>(spa_pod_builder_pop(&builder, &objectFrame));
}

const spa_pod* buildPipeWireDmabufBufferParams(spa_pod_builder& builder) {
    // Only the count and the memory type are the consumer's to ask for.
    //
    // Constraining blocks, size or stride here makes the producer reject the
    // allocation outright ("error alloc buffers"): the plane count belongs to
    // the chosen modifier -- a tiled or compression-control layout needs more
    // than one -- and a GPU buffer has no single CPU size or stride to state.
    return static_cast<const spa_pod*>(spa_pod_builder_add_object(
        &builder,
        SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,
        // Enough buffers that the producer keeps rendering while one is held
        // for the GPU to finish reading.
        SPA_PARAM_BUFFERS_buffers, SPA_POD_CHOICE_RANGE_Int(8, 3, 16),
        SPA_PARAM_BUFFERS_dataType, SPA_POD_CHOICE_FLAGS_Int(1 << SPA_DATA_DmaBuf)));
}

}  // namespace od
