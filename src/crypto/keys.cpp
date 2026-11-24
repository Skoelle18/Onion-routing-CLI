#include "onion/crypto/keys.hpp"

#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <stdexcept>

#ifdef _WIN32
#include <io.h>
#else
#include <sys/stat.h>
#endif

namespace onion::crypto {
namespace {

int set_key_permissions(const std::string& path, int mode) {
#ifdef _WIN32
    return ::_chmod(path.c_str(), mode);
#else
    return ::chmod(path.c_str(), mode);
#endif
}

}  // namespace

void PkeyDeleter::operator()(EVP_PKEY* key) const noexcept {
    EVP_PKEY_free(key);
}

std::string openssl_error(const std::string& context) {
    std::string result = context;
    unsigned long code = ERR_get_error();
    if (code == 0) {
        return result + ": no OpenSSL error available";
    }
    char buf[256];
    ERR_error_string_n(code, buf, sizeof(buf));
    return result + ": " + buf;
}

PkeyPtr generate_rsa(int bits) {
    if (bits < 2048) {
        throw std::invalid_argument("RSA key size must be at least 2048 bits");
    }

    EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr);
    if (ctx == nullptr) {
        throw std::runtime_error(openssl_error("EVP_PKEY_CTX_new_id"));
    }

    PkeyPtr result;
    bool ok = false;
    do {
        if (EVP_PKEY_keygen_init(ctx) <= 0) {
            break;
        }
        if (EVP_PKEY_CTX_set_rsa_keygen_bits(ctx, bits) <= 0) {
            break;
        }
        EVP_PKEY* raw = nullptr;
        if (EVP_PKEY_keygen(ctx, &raw) <= 0 || raw == nullptr) {
            break;
        }
        result.reset(raw);
        ok = true;
    } while (false);

    EVP_PKEY_CTX_free(ctx);
    if (!ok) {
        throw std::runtime_error(openssl_error("RSA key generation"));
    }
    return result;
}

PkeyPtr load_private_key(const std::string& path) {
    std::FILE* fp = std::fopen(path.c_str(), "rb");
    if (fp == nullptr) {
        throw std::runtime_error("cannot open private key '" + path + "': " +
                                 std::strerror(errno));
    }
    EVP_PKEY* raw = PEM_read_PrivateKey(fp, nullptr, nullptr, nullptr);
    std::fclose(fp);
    if (raw == nullptr) {
        throw std::runtime_error(openssl_error("cannot read private key '" + path + "'"));
    }
    return PkeyPtr(raw);
}

PkeyPtr load_public_key(const std::string& path) {
    std::FILE* fp = std::fopen(path.c_str(), "rb");
    if (fp == nullptr) {
        throw std::runtime_error("cannot open public key '" + path + "': " +
                                 std::strerror(errno));
    }
    EVP_PKEY* raw = PEM_read_PUBKEY(fp, nullptr, nullptr, nullptr);
    std::fclose(fp);
    if (raw == nullptr) {
        throw std::runtime_error(openssl_error("cannot read public key '" + path + "'"));
    }
    return PkeyPtr(raw);
}

void write_private_key(const PkeyPtr& key, const std::string& path) {
    if (!key) {
        throw std::invalid_argument("null private key");
    }
    std::FILE* fp = std::fopen(path.c_str(), "wb");
    if (fp == nullptr) {
        throw std::runtime_error("cannot write private key '" + path + "': " +
                                 std::strerror(errno));
    }
    int rc = PEM_write_PrivateKey(fp, key.get(), nullptr, nullptr, 0, nullptr, nullptr);
    std::fclose(fp);
    if (rc != 1) {
        throw std::runtime_error(openssl_error("cannot write private key '" + path + "'"));
    }
    if (set_key_permissions(path, 0600) != 0) {
        throw std::runtime_error("cannot set private key permissions on '" + path + "': " +
                                 std::strerror(errno));
    }
}

void write_public_key(const PkeyPtr& key, const std::string& path) {
    if (!key) {
        throw std::invalid_argument("null public key");
    }
    std::FILE* fp = std::fopen(path.c_str(), "wb");
    if (fp == nullptr) {
        throw std::runtime_error("cannot write public key '" + path + "': " +
                                 std::strerror(errno));
    }
    int rc = PEM_write_PUBKEY(fp, key.get());
    std::fclose(fp);
    if (rc != 1) {
        throw std::runtime_error(openssl_error("cannot write public key '" + path + "'"));
    }
    set_key_permissions(path, 0644);
}

int key_size_bytes(const PkeyPtr& key) {
    if (!key) {
        throw std::invalid_argument("null key");
    }
    int size = EVP_PKEY_get_size(key.get());
    if (size <= 0) {
        throw std::runtime_error(openssl_error("EVP_PKEY_get_size"));
    }
    return size;
}

}  // namespace onion::crypto
