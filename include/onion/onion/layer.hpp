#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace onion::onion {

enum class InnerKind : std::uint8_t {
    Forward = 0x01,
    Deliver = 0x02,
};

struct InnerPayload {
    InnerKind kind{InnerKind::Forward};
    std::string next_addr;
    std::vector<std::uint8_t> inner;
};

std::vector<std::uint8_t> serialize(const InnerPayload& payload);
InnerPayload deserialize(const std::vector<std::uint8_t>& buffer);
std::size_t serialized_size(const InnerPayload& payload);

std::string to_string(InnerKind kind);

}  // namespace onion::onion
