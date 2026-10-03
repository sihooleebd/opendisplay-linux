#include "opendisplay/wire.hpp"

#include <QJsonDocument>
#include <QJsonValue>

#include <array>
#include <algorithm>
#include <cstring>

namespace od::wire {

std::string frame(const std::string_view payload) {
    const auto size = static_cast<std::uint32_t>(payload.size());
    std::string output(4, '\0');
    output[0] = static_cast<char>((size >> 24U) & 0xffU);
    output[1] = static_cast<char>((size >> 16U) & 0xffU);
    output[2] = static_cast<char>((size >> 8U) & 0xffU);
    output[3] = static_cast<char>(size & 0xffU);
    output.append(payload);
    return output;
}

std::optional<std::uint32_t> decodeLength(const std::span<const char, 4> header) {
    const auto size = (static_cast<std::uint32_t>(static_cast<unsigned char>(header[0])) << 24U)
        | (static_cast<std::uint32_t>(static_cast<unsigned char>(header[1])) << 16U)
        | (static_cast<std::uint32_t>(static_cast<unsigned char>(header[2])) << 8U)
        | static_cast<std::uint32_t>(static_cast<unsigned char>(header[3]));
    if (size == 0 || size >= maxControlSize) {
        return std::nullopt;
    }
    return size;
}

std::optional<QJsonObject> parseJson(const std::string_view payload) {
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(
        QByteArray(payload.data(), static_cast<qsizetype>(payload.size())), &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) {
        return std::nullopt;
    }
    return document.object();
}

std::optional<PhoneInfo> parseHello(const QJsonObject& object) {
    if (object.value("type").toString() != QStringLiteral("hello")) {
        return std::nullopt;
    }
    PhoneInfo info;
    info.pixelsWide = object.value("pixelsWide").toInt();
    info.pixelsHigh = object.value("pixelsHigh").toInt();
    info.scale = object.value("scale").toDouble(2.0);
    info.device = object.value("device").toString(QStringLiteral("device")).toStdString();
    info.installId = object.value("id").toString().toStdString();
    info.protocolVersion = object.value("pv").toInt(1);
    if (info.pixelsWide <= 0 || info.pixelsHigh <= 0) {
        return std::nullopt;
    }
    return info;
}

std::string welcome() {
    QJsonObject object{
        {"type", "welcome"},
        {"pv", protocolVersion},
        {"min", minSupportedPeer},
    };
    return QJsonDocument(object).toJson(QJsonDocument::Compact).toStdString();
}

std::string pong(const double phoneTime, const double hostTime) {
    QJsonObject object{{"type", "pong"}, {"t", phoneTime}, {"mt", hostTime}};
    return QJsonDocument(object).toJson(QJsonDocument::Compact).toStdString();
}

namespace {

std::string videoTelemetry(const EncodedFrame& encoded, const std::int64_t sentAtMs) {
    return std::string("{\"cap\":") + std::to_string(encoded.capturedAtMs)
        + ",\"snd\":" + std::to_string(sentAtMs) + "}";
}

void appendLengthPrefix(std::string& output, const std::size_t payloadSize) {
    const auto size = static_cast<std::uint32_t>(payloadSize);
    output.push_back(static_cast<char>((size >> 24U) & 0xffU));
    output.push_back(static_cast<char>((size >> 16U) & 0xffU));
    output.push_back(static_cast<char>((size >> 8U) & 0xffU));
    output.push_back(static_cast<char>(size & 0xffU));
}

}  // namespace

std::string videoPayload(const EncodedFrame& encoded, const std::int64_t sentAtMs) {
    const auto telemetry = videoTelemetry(encoded, sentAtMs);
    std::string output;
    output.reserve(telemetry.size() + encoded.annexB.size());
    output.append(telemetry);
    output.append(encoded.annexB);
    return output;
}

std::string videoFrame(const EncodedFrame& encoded, const std::int64_t sentAtMs) {
    const auto telemetry = videoTelemetry(encoded, sentAtMs);
    std::string output;
    output.reserve(4 + telemetry.size() + encoded.annexB.size());
    appendLengthPrefix(output, telemetry.size() + encoded.annexB.size());
    output.append(telemetry);
    output.append(encoded.annexB);
    return output;
}

bool containsAnnexBStartCode(const std::string_view bytes) {
    // Hunt zero bytes rather than comparing a four-byte needle at every
    // offset. A well-formed access unit opens with the delimiter, so the first
    // probe normally settles it instead of walking megabytes of payload.
    if (bytes.size() < 4) {
        return false;
    }
    const char* const begin = bytes.data();
    std::size_t index = 0;
    while (index + 3 < bytes.size()) {
        const void* hit = std::memchr(begin + index, 0, bytes.size() - 3 - index);
        if (hit == nullptr) {
            return false;
        }
        index = static_cast<std::size_t>(static_cast<const char*>(hit) - begin);
        if (begin[index + 1] == 0 && begin[index + 2] == 0 && begin[index + 3] == 1) {
            return true;
        }
        ++index;
    }
    return false;
}

}  // namespace od::wire
