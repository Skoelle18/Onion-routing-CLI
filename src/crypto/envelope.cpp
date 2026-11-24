#include "onion/crypto/envelope.hpp"

#include "onion/crypto/keys.hpp"
#include "onion/error.hpp"

#include <openssl/err.h>
#include <openssl/rand.h>
#include <openssl/rsa.h>

#include <algorithm>
#include <array>
#include <limits>
#include <stdexcept>

namespace onion::crypto {
namespace {

constexpr std::array<std::uint8_t, 4> kMagic{'O', 'N', 'L', 'V'};
constexpr std::uint8_t kVersion = 1;

void append_u16(std::vector<std::uint8_t>& out, std::uint16_t value) {
    out.push_back(static_cast<std::uint8_t>(value >> 8));
    out.push_back(static_cast<std::uint8_t>(value));
}

void append_u32(std::vector<std::uint8_t>& out, std::uint32_t value) {
    out.push_back(static_cast<std::uint8_t>(value >> 24));
    out.push_back(static_cast<std::uint8_t>(value >> 16));
    out.push_back(static_cast<std::uint8_t>(value >> 8));
    out.push_back(static_cast<std::uint8_t>(value));
}

std::uint16_t read_u16(const std::vector<std::uint8_t>& buf, std::size_t& offset) {
    if (offset + 2 > buf.size()) {
        throw ProtocolError("truncated 16-bit length in envelope");
    }
    std::uint16_t value = static_cast<std::uint16_t>(buf[offset]) << 8;
    value |= static_cast<std::uint16_t>(buf[offset + 1]);
    offset += 2;
    return value;
}

std::uint32_t read_u32(const std::vector<std::uint8_t>& buf, std::size_t& offset) {
    if (offset + 4 > buf.size()) {
        throw ProtocolError("truncated 32-bit length in envelope");
    }
    std::uint32_t value = static_cast<std::uint32_t>(buf[offset]) << 24;
    value |= static_cast<std::uint32_t>(buf[offset + 1]) << 16;
    value |= static_cast<std::uint32_t>(buf[offset + 2]) << 8;
    value |= static_cast<std::uint32_t>(buf[offset + 3]);
    offset += 4;
    return value;
}

void random_bytes(std::uint8_t* out, std::size_t len) {
    if (RAND_bytes(out, static_cast<int>(len)) != 1) {
        throw std::runtime_error(openssl_error("RAND_bytes"));
    }
}

std::vector<std::uint8_t> rsa_wrap(const std::vector<std::uint8_t>& key,
                                   EVP_PKEY* public_key) {
    if (public_key == nullptr) {
        throw std::invalid_argument("null RSA public key");
    }
    EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new(public_key, nullptr);
    if (ctx == nullptr) {
        throw std::runtime_error(openssl_error("EVP_PKEY_CTX_new"));
    }

    std::vector<std::uint8_t> result;
    bool ok = false;
    do {
        if (EVP_PKEY_encrypt_init(ctx) <= 0) {
            break;
        }
        if (EVP_PKEY_CTX_set_rsa_padding(ctx, RSA_PKCS1_OAEP_PADDING) <= 0) {
            break;
        }
        if (EVP_PKEY_CTX_set_rsa_oaep_md(ctx, EVP_sha256()) <= 0) {
            break;
        }
        if (EVP_PKEY_CTX_set_rsa_mgf1_md(ctx, EVP_sha256()) <= 0) {
            break;
        }
        std::size_t out_len = 0;
        if (EVP_PKEY_encrypt(ctx, nullptr, &out_len, key.data(), key.size()) <= 0 ||
            out_len == 0) {
            break;
        }
        result.resize(out_len);
        if (EVP_PKEY_encrypt(ctx, result.data(), &out_len, key.data(), key.size()) <= 0) {
            result.clear();
            break;
        }
        result.resize(out_len);
        ok = true;
    } while (false);

    EVP_PKEY_CTX_free(ctx);
    if (!ok) {
        throw std::runtime_error(openssl_error("RSA-OAEP wrapping"));
    }
    return result;
}

std::vector<std::uint8_t> rsa_unwrap(const std::vector<std::uint8_t>& wrapped,
                                     EVP_PKEY* private_key) {
    if (private_key == nullptr) {
        throw std::invalid_argument("null RSA private key");
    }
    EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new(private_key, nullptr);
    if (ctx == nullptr) {
        throw DecryptError("cannot initialise RSA private key context");
    }

    std::vector<std::uint8_t> result;
    bool ok = false;
    do {
        if (EVP_PKEY_decrypt_init(ctx) <= 0) {
            break;
        }
        if (EVP_PKEY_CTX_set_rsa_padding(ctx, RSA_PKCS1_OAEP_PADDING) <= 0) {
            break;
        }
        if (EVP_PKEY_CTX_set_rsa_oaep_md(ctx, EVP_sha256()) <= 0) {
            break;
        }
        if (EVP_PKEY_CTX_set_rsa_mgf1_md(ctx, EVP_sha256()) <= 0) {
            break;
        }
        std::size_t out_len = 0;
        if (EVP_PKEY_decrypt(ctx, nullptr, &out_len, wrapped.data(), wrapped.size()) <= 0 ||
            out_len == 0) {
            break;
        }
        result.resize(out_len);
        if (EVP_PKEY_decrypt(ctx, result.data(), &out_len, wrapped.data(), wrapped.size()) <= 0) {
            result.clear();
            break;
        }
        result.resize(out_len);
        ok = true;
    } while (false);

    EVP_PKEY_CTX_free(ctx);
    if (!ok || result.size() != kAesKeyBytes) {
        OPENSSL_cleanse(result.data(), result.size());
        throw DecryptError("RSA-OAEP unwrap failed");
    }
    return result;
}

}  // namespace

Envelope wrap(const std::vector<std::uint8_t>& plaintext, EVP_PKEY* public_key) {
    std::array<std::uint8_t, kAesKeyBytes> aes_key{};
    std::array<std::uint8_t, kGcmIvBytes> iv{};
    random_bytes(aes_key.data(), aes_key.size());
    random_bytes(iv.data(), iv.size());

    Envelope envelope;
    envelope.wrapped_key = rsa_wrap(std::vector<std::uint8_t>(aes_key.begin(), aes_key.end()),
                                    public_key);
    envelope.iv.assign(iv.begin(), iv.end());

    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (ctx == nullptr) {
        OPENSSL_cleanse(aes_key.data(), aes_key.size());
        throw std::runtime_error(openssl_error("EVP_CIPHER_CTX_new"));
    }

    bool ok = false;
    do {
        if (EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) != 1) {
            break;
        }
        if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN,
                                static_cast<int>(kGcmIvBytes), nullptr) != 1) {
            break;
        }
        if (EVP_EncryptInit_ex(ctx, nullptr, nullptr, aes_key.data(), iv.data()) != 1) {
            break;
        }
        std::vector<std::uint8_t> ciphertext(plaintext.size() + EVP_MAX_BLOCK_LENGTH);
        int out_len = 0;
        if (!plaintext.empty() &&
            EVP_EncryptUpdate(ctx, ciphertext.data(), &out_len, plaintext.data(),
                              static_cast<int>(plaintext.size())) != 1) {
            break;
        }
        int final_len = 0;
        if (EVP_EncryptFinal_ex(ctx, ciphertext.data() + out_len, &final_len) != 1) {
            break;
        }
        out_len += final_len;
        ciphertext.resize(static_cast<std::size_t>(out_len));

        std::array<std::uint8_t, kGcmTagBytes> tag{};
        if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG,
                                static_cast<int>(kGcmTagBytes), tag.data()) != 1) {
            break;
        }
        ciphertext.insert(ciphertext.end(), tag.begin(), tag.end());
        envelope.ciphertext = std::move(ciphertext);
        ok = true;
    } while (false);

    EVP_CIPHER_CTX_free(ctx);
    OPENSSL_cleanse(aes_key.data(), aes_key.size());
    if (!ok) {
        throw std::runtime_error(openssl_error("AES-256-GCM encryption"));
    }
    return envelope;
}

std::vector<std::uint8_t> unwrap(const Envelope& envelope, EVP_PKEY* private_key) {
    if (envelope.iv.size() != kGcmIvBytes) {
        throw ProtocolError("invalid GCM IV length");
    }
    if (envelope.ciphertext.size() < kGcmTagBytes) {
        throw ProtocolError("GCM ciphertext shorter than tag");
    }

    std::vector<std::uint8_t> aes_key = rsa_unwrap(envelope.wrapped_key, private_key);
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (ctx == nullptr) {
        OPENSSL_cleanse(aes_key.data(), aes_key.size());
        throw std::runtime_error(openssl_error("EVP_CIPHER_CTX_new"));
    }

    std::vector<std::uint8_t> plaintext;
    bool ok = false;
    do {
        if (EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) != 1) {
            break;
        }
        if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN,
                                static_cast<int>(kGcmIvBytes), nullptr) != 1) {
            break;
        }
        if (EVP_DecryptInit_ex(ctx, nullptr, nullptr, aes_key.data(), envelope.iv.data()) != 1) {
            break;
        }
        std::size_t ct_len = envelope.ciphertext.size() - kGcmTagBytes;
        std::vector<std::uint8_t> buffer(ct_len + EVP_MAX_BLOCK_LENGTH);
        int out_len = 0;
        if (ct_len > 0 &&
            EVP_DecryptUpdate(ctx, buffer.data(), &out_len, envelope.ciphertext.data(),
                              static_cast<int>(ct_len)) != 1) {
            break;
        }
        if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG,
                                static_cast<int>(kGcmTagBytes),
                                const_cast<std::uint8_t*>(envelope.ciphertext.data() + ct_len)) != 1) {
            break;
        }
        int final_len = 0;
        if (EVP_DecryptFinal_ex(ctx, buffer.data() + out_len, &final_len) != 1) {
            break;
        }
        out_len += final_len;
        buffer.resize(static_cast<std::size_t>(out_len));
        plaintext = std::move(buffer);
        ok = true;
    } while (false);

    EVP_CIPHER_CTX_free(ctx);
    OPENSSL_cleanse(aes_key.data(), aes_key.size());
    if (!ok) {
        ERR_clear_error();
        throw DecryptError("AES-256-GCM authentication failed");
    }
    return plaintext;
}

std::size_t serialized_size(const Envelope& envelope) {
    return kMagic.size() + 1 + 2 + envelope.wrapped_key.size() + 1 + envelope.iv.size() +
           4 + envelope.ciphertext.size();
}

std::vector<std::uint8_t> serialize(const Envelope& envelope) {
    std::vector<std::uint8_t> out;
    out.reserve(serialized_size(envelope));
    out.insert(out.end(), kMagic.begin(), kMagic.end());
    out.push_back(kVersion);
    if (envelope.wrapped_key.size() > std::numeric_limits<std::uint16_t>::max()) {
        throw ProtocolError("wrapped RSA key too large");
    }
    append_u16(out, static_cast<std::uint16_t>(envelope.wrapped_key.size()));
    out.insert(out.end(), envelope.wrapped_key.begin(), envelope.wrapped_key.end());
    out.push_back(static_cast<std::uint8_t>(envelope.iv.size()));
    out.insert(out.end(), envelope.iv.begin(), envelope.iv.end());
    if (envelope.ciphertext.size() > std::numeric_limits<std::uint32_t>::max()) {
        throw ProtocolError("ciphertext too large");
    }
    append_u32(out, static_cast<std::uint32_t>(envelope.ciphertext.size()));
    out.insert(out.end(), envelope.ciphertext.begin(), envelope.ciphertext.end());
    return out;
}

Envelope deserialize(const std::vector<std::uint8_t>& buffer, std::size_t offset) {
    if (offset > buffer.size()) {
        throw ProtocolError("envelope offset out of range");
    }
    if (buffer.size() - offset < kMagic.size() + 1 + 2) {
        throw ProtocolError("truncated envelope header");
    }
    for (std::size_t i = 0; i < kMagic.size(); ++i) {
        if (buffer[offset + i] != kMagic[i]) {
            throw ProtocolError("bad envelope magic");
        }
    }
    offset += kMagic.size();
    if (buffer[offset++] != kVersion) {
        throw ProtocolError("unsupported envelope version");
    }

    Envelope envelope;
    std::uint16_t wrapped_len = read_u16(buffer, offset);
    if (wrapped_len == 0 || buffer.size() - offset < wrapped_len) {
        throw ProtocolError("truncated wrapped key");
    }
    envelope.wrapped_key.assign(buffer.begin() + static_cast<std::ptrdiff_t>(offset),
                                buffer.begin() + static_cast<std::ptrdiff_t>(offset + wrapped_len));
    offset += wrapped_len;

    if (offset >= buffer.size()) {
        throw ProtocolError("truncated IV length");
    }
    std::uint8_t iv_len = buffer[offset++];
    if (iv_len != kGcmIvBytes || buffer.size() - offset < iv_len) {
        throw ProtocolError("invalid or truncated IV");
    }
    envelope.iv.assign(buffer.begin() + static_cast<std::ptrdiff_t>(offset),
                       buffer.begin() + static_cast<std::ptrdiff_t>(offset + iv_len));
    offset += iv_len;

    std::uint32_t ct_len = read_u32(buffer, offset);
    if (ct_len < kGcmTagBytes || buffer.size() - offset < ct_len) {
        throw ProtocolError("invalid or truncated ciphertext");
    }
    envelope.ciphertext.assign(
        buffer.begin() + static_cast<std::ptrdiff_t>(offset),
        buffer.begin() + static_cast<std::ptrdiff_t>(offset + ct_len));
    return envelope;
}

}  // namespace onion::crypto
