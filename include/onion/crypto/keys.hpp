#pragma once

#include <openssl/evp.h>

#include <memory>
#include <string>

namespace onion::crypto {

struct PkeyDeleter {
    void operator()(EVP_PKEY* key) const noexcept;
};

using PkeyPtr = std::unique_ptr<EVP_PKEY, PkeyDeleter>;

PkeyPtr generate_rsa(int bits);

PkeyPtr load_private_key(const std::string& path);
PkeyPtr load_public_key(const std::string& path);

void write_private_key(const PkeyPtr& key, const std::string& path);
void write_public_key(const PkeyPtr& key, const std::string& path);

int key_size_bytes(const PkeyPtr& key);

std::string openssl_error(const std::string& context);

}  // namespace onion::crypto
