#include <gtest/gtest.h>

#include "onion/crypto/envelope.hpp"
#include "onion/crypto/keys.hpp"
#include "onion/error.hpp"

#ifdef _WIN32
#include <process.h>
#define ONION_TEST_PID _getpid
#else
#include <unistd.h>
#define ONION_TEST_PID getpid
#endif

#include <filesystem>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;
namespace crypto = onion::crypto;

struct KeyFiles {
    fs::path dir;
    fs::path priv;
    fs::path pub;

    explicit KeyFiles(const std::string& name) {
        dir = fs::temp_directory_path() / ("onion_crypto_" + name + "_" +
                                           std::to_string(ONION_TEST_PID()));
        fs::create_directories(dir);
        priv = dir / "key.pem";
        pub = dir / "key.pub.pem";
    }

    ~KeyFiles() {
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
};

TEST(CryptoTest, GeneratesAndPersistsRsa2048Keys) {
    KeyFiles files("generate");
    auto key = crypto::generate_rsa(2048);
    ASSERT_TRUE(key);
    EXPECT_EQ(crypto::key_size_bytes(key), 256);

    crypto::write_private_key(key, files.priv.string());
    crypto::write_public_key(key, files.pub.string());

    auto loaded_priv = crypto::load_private_key(files.priv.string());
    auto loaded_pub = crypto::load_public_key(files.pub.string());
    ASSERT_TRUE(loaded_priv);
    ASSERT_TRUE(loaded_pub);
    EXPECT_EQ(crypto::key_size_bytes(loaded_priv), 256);
    EXPECT_EQ(crypto::key_size_bytes(loaded_pub), 256);

#ifndef _WIN32
    auto status = fs::status(files.priv);
    EXPECT_EQ(status.permissions() & fs::perms::owner_write, fs::perms::owner_write);
#endif
}

TEST(CryptoTest, RejectsUndersizedKeyRequest) {
    EXPECT_THROW(crypto::generate_rsa(1024), std::invalid_argument);
}

TEST(CryptoTest, WrapUnwrapRoundTrip) {
    auto key = crypto::generate_rsa(2048);
    const std::vector<std::uint8_t> plaintext{'o', 'n', 'i', 'o', 'n', 0, 1, 2};

    auto envelope = crypto::wrap(plaintext, key.get());
    EXPECT_EQ(envelope.iv.size(), crypto::kGcmIvBytes);
    EXPECT_EQ(envelope.wrapped_key.size(), 256);
    EXPECT_EQ(envelope.ciphertext.size(), plaintext.size() + crypto::kGcmTagBytes);
    EXPECT_EQ(crypto::unwrap(envelope, key.get()), plaintext);
}

TEST(CryptoTest, WrongKeyFailsToUnwrap) {
    auto right = crypto::generate_rsa(2048);
    auto wrong = crypto::generate_rsa(2048);
    auto envelope = crypto::wrap({1, 2, 3}, right.get());
    EXPECT_THROW(crypto::unwrap(envelope, wrong.get()), onion::DecryptError);
}

TEST(CryptoTest, TamperedWrappedKeyIsRejected) {
    auto key = crypto::generate_rsa(2048);
    auto envelope = crypto::wrap({1, 2, 3, 4}, key.get());
    envelope.wrapped_key[10] ^= 0x01;
    EXPECT_THROW(crypto::unwrap(envelope, key.get()), onion::DecryptError);
}

TEST(CryptoTest, TamperedCiphertextIsRejected) {
    auto key = crypto::generate_rsa(2048);
    auto envelope = crypto::wrap({1, 2, 3, 4}, key.get());
    envelope.ciphertext[0] ^= 0x01;
    EXPECT_THROW(crypto::unwrap(envelope, key.get()), onion::DecryptError);
}

TEST(CryptoTest, TamperedTagIsRejected) {
    auto key = crypto::generate_rsa(2048);
    auto envelope = crypto::wrap({1, 2, 3, 4}, key.get());
    envelope.ciphertext.back() ^= 0x01;
    EXPECT_THROW(crypto::unwrap(envelope, key.get()), onion::DecryptError);
}

TEST(CryptoTest, TamperedIvIsRejected) {
    auto key = crypto::generate_rsa(2048);
    auto envelope = crypto::wrap({1, 2, 3, 4}, key.get());
    envelope.iv[0] ^= 0x01;
    EXPECT_THROW(crypto::unwrap(envelope, key.get()), onion::DecryptError);
}

TEST(CryptoTest, EnvelopeSerializationRoundTrip) {
    auto key = crypto::generate_rsa(2048);
    auto envelope = crypto::wrap({9, 8, 7, 6, 5}, key.get());
    auto bytes = crypto::serialize(envelope);
    EXPECT_EQ(bytes.size(), crypto::serialized_size(envelope));

    auto parsed = crypto::deserialize(bytes);
    EXPECT_EQ(parsed.wrapped_key, envelope.wrapped_key);
    EXPECT_EQ(parsed.iv, envelope.iv);
    EXPECT_EQ(parsed.ciphertext, envelope.ciphertext);
    EXPECT_EQ(crypto::unwrap(parsed, key.get()),
              std::vector<std::uint8_t>({9, 8, 7, 6, 5}));
}

TEST(CryptoTest, MalformedEnvelopeIsRejected) {
    EXPECT_THROW(crypto::deserialize({}), onion::ProtocolError);
    auto key = crypto::generate_rsa(2048);
    auto bytes = crypto::serialize(crypto::wrap({1}, key.get()));
    bytes[0] ^= 0x01;
    EXPECT_THROW(crypto::deserialize(bytes), onion::ProtocolError);
}

}  // namespace
