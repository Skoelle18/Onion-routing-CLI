#pragma once

#include <openssl/evp.h>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace onion::crypto {

constexpr std::size_t kAesKeyBytes = 32;
constexpr std::size_t kGcmIvBytes = 12;
constexpr std::size_t kGcmTagBytes = 16;

struct Envelope {
    std::vector<std::uint8_t> wrapped_key;
    std::vector<std::uint8_t> iv;
    std::vector<std::uint8_t> ciphertext;  // ciphertext || GCM tag
};

Envelope wrap(const std::vector<std::uint8_t>& plaintext, EVP_PKEY* public_key);
std::vector<std::uint8_t> unwrap(const Envelope& envelope, EVP_PKEY* private_key);

std::vector<std::uint8_t> serialize(const Envelope& envelope);
Envelope deserialize(const std::vector<std::uint8_t>& buffer, std::size_t offset = 0);
std::size_t serialized_size(const Envelope& envelope);

}  // namespace onion::crypto
