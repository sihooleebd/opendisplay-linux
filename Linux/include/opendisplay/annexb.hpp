#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace od::annexb {

/// Length of the start code at `position`, which is 4 for 00 00 00 01 and 3
/// for 00 00 01. Callers must already know a start code begins there.
std::size_t startCodeLength(std::string_view bytes, std::size_t position);

/// Offset of the next start code at or after `from`, or npos.
std::size_t findStartCode(std::string_view bytes, std::size_t from);

/// Rewrites every three-byte start code as a four-byte one.
///
/// FFmpeg's Annex-B output mixes both widths. The existing iOS receiver
/// recognizes only the four-byte form, matching the macOS sender, so every
/// NAL is normalized before transmission regardless of which encoder produced
/// it.
std::string normalize(std::string_view accessUnit);

/// True when the access unit carries an IDR slice (NAL type 5).
bool containsKeyframe(std::string_view accessUnit);

}  // namespace od::annexb
