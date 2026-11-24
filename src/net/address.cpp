#include "onion/net/address.hpp"

#include "onion/error.hpp"

#include <array>
#include <cctype>
#include <limits>
#include <string>

namespace onion::net {
namespace {

bool is_all_digits(const std::string& text) {
    if (text.empty()) {
        return false;
    }
    for (unsigned char ch : text) {
        if (!std::isdigit(ch)) {
            return false;
        }
    }
    return true;
}

}  // namespace

Address parse_address(const std::string& text) {
    const std::size_t colon = text.rfind(':');
    if (colon == std::string::npos || colon == 0 || colon + 1 == text.size()) {
        throw ProtocolError("address must be 'host:port': '" + text + "'");
    }

    std::string host = text.substr(0, colon);
    const std::string port_text = text.substr(colon + 1);

    if (host.front() == '[') {
        if (host.back() != ']' || host.size() <= 2) {
            throw ProtocolError("malformed bracketed IPv6 address: '" + text + "'");
        }
        host = host.substr(1, host.size() - 2);
    } else if (host.find(':') != std::string::npos) {
        throw ProtocolError("IPv6 addresses must be bracketed: '" + text + "'");
    }

    if (host.empty()) {
        throw ProtocolError("empty host in address: '" + text + "'");
    }
    if (!is_all_digits(port_text)) {
        throw ProtocolError("invalid port in address: '" + text + "'");
    }

    unsigned long value = 0;
    for (char ch : port_text) {
        value = value * 10 + static_cast<unsigned long>(ch - '0');
        if (value > std::numeric_limits<std::uint16_t>::max()) {
            throw ProtocolError("port out of range: '" + text + "'");
        }
    }
    if (value == 0) {
        throw ProtocolError("port must be greater than zero: '" + text + "'");
    }
    return Address{host, static_cast<std::uint16_t>(value)};
}

std::string format_address(const Address& address) {
    std::string host = address.host;
    if (host.find(':') != std::string::npos) {
        host = "[" + host + "]";
    }
    return host + ":" + std::to_string(address.port);
}

}  // namespace onion::net
