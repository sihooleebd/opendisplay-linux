#pragma once

#include <spa/pod/builder.h>

#include <cstddef>
#include <cstdint>

namespace od {

/// Builds the raw-video capabilities accepted by the capture and encoder path.
/// The returned pod is owned by the caller-provided builder storage.
const spa_pod* buildPipeWireFormatOffer(spa_pod_builder& builder, int width, int height,
                                        int fps);

/// The same capabilities, but asking the producer for a GPU buffer.
///
/// Adding a modifier choice is what tells PipeWire this consumer can take a
/// DMA-BUF. It is offered alongside, and ahead of, the system-memory form, so
/// a producer that cannot share GPU buffers simply negotiates the other one.
/// The choice is left unfixated, as the protocol requires: the producer picks
/// a modifier and the consumer re-offers that single value.
const spa_pod* buildPipeWireDmabufOffer(spa_pod_builder& builder, int width, int height,
                                        int fps, const std::uint64_t* modifiers,
                                        std::size_t modifierCount);

/// Buffer requirements announced once a GPU format is agreed.
///
/// Only sent for the DMA-BUF path: system memory keeps PipeWire's defaults,
/// which is what the capture path has always relied on.
const spa_pod* buildPipeWireDmabufBufferParams(spa_pod_builder& builder);

}  // namespace od
