#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace onion::net {

constexpr std::size_t kMaxFrameBytes = 16u * 1024u * 1024u;

enum class FrameType : std::uint8_t {
    Onion = 0x01,
    Status = 0x02,
    Ping = 0x03,
    Pong = 0x04,
};

enum class StatusCode : std::uint8_t {
    Ok = 0x00,
    DecryptFailed = 0x01,
    UnreachableHop = 0x02,
    ProtocolError = 0x03,
    Timeout = 0x04,
};

struct Frame {
    FrameType type{FrameType::Ping};
    std::vector<std::uint8_t> body;
};

std::vector<std::uint8_t> encode_frame(const Frame& frame);
Frame decode_frame(const std::vector<std::uint8_t>& buffer, std::size_t offset = 0);
std::uint32_t framed_length(const std::vector<std::uint8_t>& buffer, std::size_t offset = 0);

Frame make_status(StatusCode code, const std::string& text = "");
std::pair<StatusCode, std::string> parse_status(const Frame& frame);
std::string status_name(StatusCode code);

}  // namespace onion::net
