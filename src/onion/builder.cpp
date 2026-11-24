#include "onion/onion/builder.hpp"

#include "onion/crypto/envelope.hpp"
#include "onion/error.hpp"

#include <stdexcept>

namespace onion::onion {

std::vector<std::uint8_t> build_onion(const Path& path, const std::string& message) {
    if (path.relays.empty()) {
        throw std::invalid_argument("onion path must contain at least one relay");
    }
    if (!path.destination.public_key) {
        throw std::invalid_argument("destination public key is missing");
    }
    if (path.destination.addr.empty()) {
        throw std::invalid_argument("destination address is missing");
    }

    InnerPayload delivery;
    delivery.kind = InnerKind::Deliver;
    delivery.next_addr = path.destination.addr;
    delivery.inner.assign(message.begin(), message.end());

    auto encrypted = crypto::serialize(crypto::wrap(
        ::onion::onion::serialize(delivery), path.destination.public_key.get()));

    for (auto it = path.relays.rbegin(); it != path.relays.rend(); ++it) {
        if (!it->public_key) {
            throw std::invalid_argument("relay public key is missing for " + it->addr);
        }
        if (it->addr.empty()) {
            throw std::invalid_argument("relay address is missing");
        }

        InnerPayload forward;
        forward.kind = InnerKind::Forward;
        forward.inner = encrypted;
        if (it != path.relays.rbegin()) {
            forward.next_addr = (it - 1)->addr;
        } else {
            forward.next_addr = path.destination.addr;
        }

        encrypted = crypto::serialize(crypto::wrap(
            ::onion::onion::serialize(forward), it->public_key.get()));
    }
    return encrypted;
}

InnerPayload unwrap_layer(const std::vector<std::uint8_t>& envelope_bytes,
                          EVP_PKEY* private_key) {
    auto envelope = crypto::deserialize(envelope_bytes);
    auto plaintext = crypto::unwrap(envelope, private_key);
    return ::onion::onion::deserialize(plaintext);
}

}  // namespace onion::onion
