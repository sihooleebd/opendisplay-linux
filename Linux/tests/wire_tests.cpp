#include "opendisplay/wire.hpp"

#include <QJsonObject>

#include <array>
#include <cassert>
#include <cstdint>
#include <string>

int main() {
    const std::string payload = R"({"type":"ping","t":12})";
    const auto framed = od::wire::frame(payload);
    assert(framed.size() == payload.size() + 4);
    std::array<char, 4> header{framed[0], framed[1], framed[2], framed[3]};
    const auto decoded = od::wire::decodeLength(header);
    assert(decoded && *decoded == payload.size());
    assert(framed.substr(4) == payload);

    const auto hello = od::wire::parseJson(
        R"({"type":"hello","pixelsWide":2732,"pixelsHigh":2048,"scale":2,"device":"iPad","id":"test","pv":2})");
    assert(hello);
    const auto phone = od::wire::parseHello(*hello);
    assert(phone);
    assert(phone->pixelsWide == 2732);
    assert(phone->pixelsHigh == 2048);
    assert(phone->device == "iPad");
    assert(phone->protocolVersion == 2);

    assert(!od::wire::parseHello(QJsonObject{{"type", "hello"}}));
    assert(od::wire::containsAnnexBStartCode(std::string("abc\0\0\0\1xyz", 10)));
    assert(!od::wire::containsAnnexBStartCode("not h264"));

    // A start code split across the scan must still be found, and a buffer
    // whose zeros never complete one must not report a false positive.
    assert(od::wire::containsAnnexBStartCode(std::string("\0\0\0\0\0\1", 6)));
    assert(!od::wire::containsAnnexBStartCode(std::string("\0\0\0\0\0\0", 6)));
    assert(!od::wire::containsAnnexBStartCode(std::string("\0\0\1", 3)));

    od::EncodedFrame encoded{.capturedAtMs = 100, .annexB = std::string("\0\0\0\1x", 5)};
    const auto video = od::wire::videoPayload(encoded, 120);
    assert(video.starts_with("{\"cap\":100,\"snd\":120}"));
    assert(video.ends_with(encoded.annexB));

    // videoFrame is videoPayload already length-prefixed: the high-rate path
    // builds both halves at once, so it has to agree with the two-step form.
    const auto videoFramed = od::wire::videoFrame(encoded, 120);
    assert(videoFramed.size() == video.size() + 4);
    assert(videoFramed.substr(4) == video);
    const std::array<char, 4> videoHeader{videoFramed[0], videoFramed[1], videoFramed[2],
                                          videoFramed[3]};
    const auto videoLength = od::wire::decodeLength(videoHeader);
    assert(videoLength && *videoLength == video.size());
    return 0;
}
