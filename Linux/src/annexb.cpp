#include "opendisplay/annexb.hpp"

#include <cstring>

namespace od::annexb {

std::size_t startCodeLength(const std::string_view bytes, const std::size_t position) {
    if (position + 3 < bytes.size() && bytes[position] == 0 && bytes[position + 1] == 0
        && bytes[position + 2] == 0 && bytes[position + 3] == 1) {
        return 4;
    }
    return 3;
}

std::size_t findStartCode(const std::string_view bytes, const std::size_t from) {
    if (bytes.size() < 3 || from + 2 >= bytes.size()) {
        return std::string::npos;
    }
    // Hunt zero bytes instead of testing a three-byte pattern at every offset.
    std::size_t index = from;
    while (index + 2 < bytes.size()) {
        const void* hit = std::memchr(bytes.data() + index, 0, bytes.size() - 2 - index);
        if (hit == nullptr) {
            return std::string::npos;
        }
        index = static_cast<std::size_t>(static_cast<const char*>(hit) - bytes.data());
        if (bytes[index + 1] == 0
            && (bytes[index + 2] == 1
                || (index + 3 < bytes.size() && bytes[index + 2] == 0
                    && bytes[index + 3] == 1))) {
            return index;
        }
        ++index;
    }
    return std::string::npos;
}

std::string normalize(const std::string_view accessUnit) {
    std::string output;
    output.reserve(accessUnit.size() + 8);
    std::size_t position = findStartCode(accessUnit, 0);
    if (position == std::string::npos) {
        return std::string(accessUnit);
    }
    while (position != std::string::npos) {
        const auto prefix = startCodeLength(accessUnit, position);
        const auto bodyStart = position + prefix;
        const auto next = findStartCode(accessUnit, bodyStart);
        const auto bodyEnd = next == std::string::npos ? accessUnit.size() : next;
        output.append("\0\0\0\1", 4);
        output.append(accessUnit.substr(bodyStart, bodyEnd - bodyStart));
        position = next;
    }
    return output;
}

bool containsKeyframe(const std::string_view accessUnit) {
    for (std::size_t position = findStartCode(accessUnit, 0); position != std::string::npos;) {
        const auto prefix = startCodeLength(accessUnit, position);
        if (position + prefix < accessUnit.size()
            && (static_cast<unsigned char>(accessUnit[position + prefix]) & 0x1fU) == 5) {
            return true;
        }
        position = findStartCode(accessUnit, position + prefix);
    }
    return false;
}

}  // namespace od::annexb
