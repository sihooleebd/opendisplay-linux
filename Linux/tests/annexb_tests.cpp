#include "opendisplay/annexb.hpp"

#include <cassert>
#include <string>

namespace {

std::string sc4() { return std::string("\0\0\0\1", 4); }
std::string sc3() { return std::string("\0\0\1", 3); }

void findsStartCodes() {
    const auto bytes = sc4() + "abc" + sc3() + "de";
    assert(od::annexb::findStartCode(bytes, 0) == 0);
    assert(od::annexb::startCodeLength(bytes, 0) == 4);
    const auto second = od::annexb::findStartCode(bytes, 4);
    assert(second == 7);
    assert(od::annexb::startCodeLength(bytes, second) == 3);
    assert(od::annexb::findStartCode("no start code here", 0) == std::string::npos);
}

void normalizesEveryDelimiterToFourBytes() {
    // A stream mixing both widths, which is what FFmpeg emits. The receiver
    // only accepts the four-byte form.
    const auto mixed = sc3() + std::string("\x67", 1) + sc4() + std::string("\x68", 1)
        + sc3() + std::string("\x65", 1);
    const auto normalized = od::annexb::normalize(mixed);
    const auto expected = sc4() + std::string("\x67", 1) + sc4() + std::string("\x68", 1)
        + sc4() + std::string("\x65", 1);
    assert(normalized == expected);

    // Already-normalized input must come back unchanged.
    assert(od::annexb::normalize(expected) == expected);

    // Payload bytes are preserved exactly, including embedded zeros.
    const auto payload = sc3() + std::string("\x41\x00\x42\x00\x00\x43", 6);
    const auto widened = od::annexb::normalize(payload);
    assert(widened == sc4() + std::string("\x41\x00\x42\x00\x00\x43", 6));

    // Input with no start code at all is passed through rather than dropped.
    assert(od::annexb::normalize("plain") == "plain");
}

void detectsKeyframes() {
    // NAL type 5 is an IDR slice; type 1 is a non-IDR slice.
    assert(od::annexb::containsKeyframe(sc4() + std::string("\x65", 1)));
    assert(!od::annexb::containsKeyframe(sc4() + std::string("\x61", 1)));
    // SPS (7) and PPS (8) ahead of the IDR must still be seen as a keyframe.
    const auto withHeaders = sc4() + std::string("\x67", 1) + sc4() + std::string("\x68", 1)
        + sc4() + std::string("\x65", 1);
    assert(od::annexb::containsKeyframe(withHeaders));
    // A three-byte delimiter before the IDR counts too.
    assert(od::annexb::containsKeyframe(sc3() + std::string("\x65", 1)));
    assert(!od::annexb::containsKeyframe("no nals"));
}

}  // namespace

int main() {
    findsStartCodes();
    normalizesEveryDelimiterToFourBytes();
    detectsKeyframes();
    return 0;
}
