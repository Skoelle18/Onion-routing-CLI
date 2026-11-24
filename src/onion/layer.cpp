#include "onion/onion/layer.hpp"

#include "onion/error.hpp"

#include <algorithm>
#include <array>
#include <limits>

namespace onion::onion {
namespace {

constexpr std::array<std::uint8_t, 4> kMagic{'O', 'N', 'P', 'L'};
constexpr std::uint8_t kVersion = 1;

void append_u32(std::vector<std::uint8_t>& out, std::uint32_t value) {
    out.push_back(static_cast<std::uint8_t>(value >> 24));
    out.push_back(static_cast<std::uint8_t>(value >> 16));
    out.push_back(static_cast<std::uint8_t>(value >> 8));
    out.push_back(static_cast<std::uint8_t>(value));
}

std::uint32_t read_u32(const std::vector<std::uint8_t>& buf, std::size_t& offset) {
    if (offset + 4 > buf.size()) {
        throw ProtocolError("truncated 32-bit length in inner payload");
    }
    std::uint32_t value = static_cast<std::uint32_t>(buf[offset]) << 24;
    value |= static_cast<std::uint32_t>(buf[offset + 1]) << 16;
    value |= static_cast<std::uint32_t>(buf[offset + 2]) << 8;
    value |= static_cast<std::uint32_t>(buf[offset + 3]);
    offset += 4;
    return value;
}

}  // namespace

std::string to_string(InnerKind kind) {
    switch (kind) {
        case InnerKind::Forward:
            return "FORWARD";
        case InnerKind::Deliver:
            return "DELIVER";
    }
    return "UNKNOWN";
}

std::size_t serialized_size(const InnerPayload& payload) {
    return kMagic.size() + 1 + 1 + 1 + payload.next_addr.size() + 4 + payload.inner.size();
}

std::vector<std::uint8_t> serialize(const InnerPayload& payload) {
    if (payload.next_addr.size() > std::numeric_limits<std::uint8_t>::max()) {
        throw ProtocolError("next hop address too long");
    }
    if (payload.inner.size() > std::numeric_limits<std::uint32_t>::max()) {
        throw ProtocolError("inner payload too large");
    }

    std::vector<std::uint8_t> out;
    out.reserve(serialized_size(payload));
    out.insert(out.end(), kMagic.begin(), kMagic.end());
    out.push_back(kVersion);
    out.push_back(static_cast<std::uint8_t>(payload.kind));
    out.push_back(static_cast<std::uint8_t>(payload.next_addr.size()));
    out.insert(out.end(), payload.next_addr.begin(), payload.next_addr.end());
    append_u32(out, static_cast<std::uint32_t>(payload.inner.size()));
    out.insert(out.end(), payload.inner.begin(), payload.inner.end());
    return out;
}

InnerPayload deserialize(const std::vector<std::uint8_t>& buffer) {
    const std::size_t header_size = kMagic.size() + 1 + 1 + 1 + 4;
    if (buffer.size() < header_size) {
        throw ProtocolError("truncated inner payload header");
    }
    for (std::size_t i = 0; i < kMagic.size(); ++i) {
        if (buffer[i] != kMagic[i]) {
            throw ProtocolError("bad inner payload magic");
        }
    }

    std::size_t offset = kMagic.size();
    if (buffer[offset++] != kVersion) {
        throw ProtocolError("unsupported inner payload version");
    }
    const auto kind = static_cast<InnerKind>(buffer[offset++]);
    if (kind != InnerKind::Forward && kind != InnerKind::Deliver) {
        throw ProtocolError("unknown inner payload kind");
    }

    std::size_t addr_len = buffer[offset++];
    if (addr_len == 0 || buffer.size() - offset < addr_len + 4) {
        throw ProtocolError("invalid or truncated next hop address");
    }
    std::string next_addr(reinterpret_cast<const char*>(buffer.data() + offset), addr_len);
    offset += addr_len;

    std::uint32_t inner_len = read_u32(buffer, offset);
    if (buffer.size() - offset != inner_len) {
        throw ProtocolError("inner payload length mismatch");
    }

    InnerPayload result;
    result.kind = kind;
    result.next_addr = std::move(next_addr);
    result.inner.assign(buffer.begin() + static_cast<std::ptrdiff_t>(offset), buffer.end());
    return result;
}

}  // namespace onion::onion
