#include "onion/net/framing.hpp"

#include "onion/error.hpp"

#include <algorithm>
#include <array>
#include <limits>

namespace onion::net {
namespace {

constexpr std::array<std::uint8_t, 4> kMagic{'O', 'N', 'F', '1'};
constexpr std::size_t kHeaderBytes = kMagic.size() + 1;

void append_u32(std::vector<std::uint8_t>& out, std::uint32_t value) {
    out.push_back(static_cast<std::uint8_t>(value >> 24));
    out.push_back(static_cast<std::uint8_t>(value >> 16));
    out.push_back(static_cast<std::uint8_t>(value >> 8));
    out.push_back(static_cast<std::uint8_t>(value));
}

std::uint16_t read_u16(const std::vector<std::uint8_t>& buf, std::size_t& offset) {
    if (offset + 2 > buf.size()) {
        throw ProtocolError("truncated 16-bit value in frame");
    }
    std::uint16_t value = static_cast<std::uint16_t>(buf[offset]) << 8;
    value |= static_cast<std::uint16_t>(buf[offset + 1]);
    offset += 2;
    return value;
}

}  // namespace

std::uint32_t framed_length(const std::vector<std::uint8_t>& buffer, std::size_t offset) {
    if (offset > buffer.size() || buffer.size() - offset < 4) {
        throw ProtocolError("truncated frame length prefix");
    }
    std::uint32_t value = static_cast<std::uint32_t>(buffer[offset]) << 24;
    value |= static_cast<std::uint32_t>(buffer[offset + 1]) << 16;
    value |= static_cast<std::uint32_t>(buffer[offset + 2]) << 8;
    value |= static_cast<std::uint32_t>(buffer[offset + 3]);
    if (value > kMaxFrameBytes) {
        throw ProtocolError("frame exceeds maximum size");
    }
    return value;
}

std::vector<std::uint8_t> encode_frame(const Frame& frame) {
    const std::size_t payload_size = kHeaderBytes + frame.body.size();
    if (payload_size > kMaxFrameBytes) {
        throw ProtocolError("frame exceeds maximum size");
    }

    std::vector<std::uint8_t> out;
    out.reserve(4 + payload_size);
    append_u32(out, static_cast<std::uint32_t>(payload_size));
    out.insert(out.end(), kMagic.begin(), kMagic.end());
    out.push_back(static_cast<std::uint8_t>(frame.type));
    out.insert(out.end(), frame.body.begin(), frame.body.end());
    return out;
}

Frame decode_frame(const std::vector<std::uint8_t>& buffer, std::size_t offset) {
    std::uint32_t payload_len = framed_length(buffer, offset);
    offset += 4;
    if (payload_len < kHeaderBytes || buffer.size() - offset < payload_len) {
        throw ProtocolError("truncated frame payload");
    }
    for (std::size_t i = 0; i < kMagic.size(); ++i) {
        if (buffer[offset + i] != kMagic[i]) {
            throw ProtocolError("bad frame magic");
        }
    }
    const auto type = static_cast<FrameType>(buffer[offset + kMagic.size()]);
    if (type != FrameType::Onion && type != FrameType::Status && type != FrameType::Ping &&
        type != FrameType::Pong) {
        throw ProtocolError("unknown frame type");
    }

    Frame frame;
    frame.type = type;
    frame.body.assign(
        buffer.begin() + static_cast<std::ptrdiff_t>(offset + kHeaderBytes),
        buffer.begin() + static_cast<std::ptrdiff_t>(offset + payload_len));
    return frame;
}

Frame make_status(StatusCode code, const std::string& text) {
    if (text.size() > std::numeric_limits<std::uint16_t>::max()) {
        throw ProtocolError("status text too long");
    }
    Frame frame;
    frame.type = FrameType::Status;
    frame.body.push_back(static_cast<std::uint8_t>(code));
    const auto len = static_cast<std::uint16_t>(text.size());
    frame.body.push_back(static_cast<std::uint8_t>(len >> 8));
    frame.body.push_back(static_cast<std::uint8_t>(len));
    frame.body.insert(frame.body.end(), text.begin(), text.end());
    return frame;
}

std::pair<StatusCode, std::string> parse_status(const Frame& frame) {
    if (frame.type != FrameType::Status) {
        throw ProtocolError("expected STATUS frame");
    }
    if (frame.body.size() < 3) {
        throw ProtocolError("truncated STATUS body");
    }
    std::size_t offset = 1;
    std::uint16_t text_len = read_u16(frame.body, offset);
    if (frame.body.size() - offset != text_len) {
        throw ProtocolError("STATUS text length mismatch");
    }
    auto code = static_cast<StatusCode>(frame.body[0]);
    if (code > StatusCode::Timeout) {
        throw ProtocolError("unknown STATUS code");
    }
    std::string text(frame.body.begin() + static_cast<std::ptrdiff_t>(offset),
                     frame.body.end());
    return {code, text};
}

std::string status_name(StatusCode code) {
    switch (code) {
        case StatusCode::Ok:
            return "OK";
        case StatusCode::DecryptFailed:
            return "DECRYPT_FAILED";
        case StatusCode::UnreachableHop:
            return "UNREACHABLE_HOP";
        case StatusCode::ProtocolError:
            return "PROTOCOL_ERROR";
        case StatusCode::Timeout:
            return "TIMEOUT";
    }
    return "UNKNOWN";
}

}  // namespace onion::net
