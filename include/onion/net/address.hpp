#pragma once

#include <cstdint>
#include <string>

namespace onion::net {

struct Address {
    std::string host;
    std::uint16_t port{0};
};

Address parse_address(const std::string& text);
std::string format_address(const Address& address);

}  // namespace onion::net
