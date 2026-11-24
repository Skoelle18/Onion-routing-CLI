#pragma once

#include "onion/crypto/keys.hpp"
#include "onion/onion/layer.hpp"

#include <string>
#include <vector>

namespace onion::onion {

struct Hop {
    std::string addr;
    crypto::PkeyPtr public_key;
};

struct Path {
    std::vector<Hop> relays;
    Hop destination;
};

// Builds the onion from the destination backwards, so relay[0] receives the
// outermost envelope and the destination receives the innermost one.
std::vector<std::uint8_t> build_onion(const Path& path, const std::string& message);

// Unwraps one serialized encrypted layer with the given private RSA key.
InnerPayload unwrap_layer(const std::vector<std::uint8_t>& envelope_bytes,
                          EVP_PKEY* private_key);

}  // namespace onion::onion
