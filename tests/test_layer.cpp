#include <gtest/gtest.h>

#include "onion/crypto/keys.hpp"
#include "onion/error.hpp"
#include "onion/onion/builder.hpp"
#include "onion/onion/layer.hpp"

#include <openssl/evp.h>

#include <string>
#include <utility>
#include <vector>

namespace {

namespace crypto = onion::crypto;
namespace routing = onion::onion;

routing::Hop make_hop(const std::string& addr, crypto::PkeyPtr key) {
    return routing::Hop{addr, std::move(key)};
}

void expect_rejected(const std::vector<std::uint8_t>& bytes, EVP_PKEY* key) {
    try {
        (void)routing::unwrap_layer(bytes, key);
        FAIL() << "tampered onion was accepted";
    } catch (const onion::DecryptError&) {
        // expected
    } catch (const onion::ProtocolError&) {
        // expected
    }
}

TEST(LayerTest, InnerPayloadSerializationRoundTrip) {
    routing::InnerPayload payload;
    payload.kind = routing::InnerKind::Forward;
    payload.next_addr = "[::1]:9001";
    payload.inner = {0, 1, 2, 255, 7};

    auto bytes = routing::serialize(payload);
    EXPECT_EQ(bytes.size(), routing::serialized_size(payload));
    auto parsed = routing::deserialize(bytes);
    EXPECT_EQ(parsed.kind, routing::InnerKind::Forward);
    EXPECT_EQ(parsed.next_addr, "[::1]:9001");
    EXPECT_EQ(parsed.inner, payload.inner);
    EXPECT_EQ(routing::to_string(parsed.kind), "FORWARD");
}

TEST(LayerTest, BuildsAndUnwrapsThreeLayers) {
    auto r1 = crypto::generate_rsa(2048);
    auto r2 = crypto::generate_rsa(2048);
    auto r3 = crypto::generate_rsa(2048);
    auto dest = crypto::generate_rsa(2048);

    EVP_PKEY* r1_raw = r1.get();
    EVP_PKEY* r2_raw = r2.get();
    EVP_PKEY* r3_raw = r3.get();
    EVP_PKEY* dest_raw = dest.get();

    routing::Path path;
    path.relays.push_back(make_hop("100.64.0.2:9001", std::move(r1)));
    path.relays.push_back(make_hop("100.64.0.3:9001", std::move(r2)));
    path.relays.push_back(make_hop("100.64.0.4:9001", std::move(r3)));
    path.destination = make_hop("100.64.0.5:9000", std::move(dest));

    const std::string message = "hello from host 1";
    auto onion = routing::build_onion(path, message);

    auto layer1 = routing::unwrap_layer(onion, r1_raw);
    EXPECT_EQ(layer1.kind, routing::InnerKind::Forward);
    EXPECT_EQ(layer1.next_addr, "100.64.0.3:9001");

    auto layer2 = routing::unwrap_layer(layer1.inner, r2_raw);
    EXPECT_EQ(layer2.kind, routing::InnerKind::Forward);
    EXPECT_EQ(layer2.next_addr, "100.64.0.4:9001");

    auto layer3 = routing::unwrap_layer(layer2.inner, r3_raw);
    EXPECT_EQ(layer3.kind, routing::InnerKind::Forward);
    EXPECT_EQ(layer3.next_addr, "100.64.0.5:9000");

    auto final_layer = routing::unwrap_layer(layer3.inner, dest_raw);
    EXPECT_EQ(final_layer.kind, routing::InnerKind::Deliver);
    EXPECT_EQ(final_layer.next_addr, "100.64.0.5:9000");
    EXPECT_EQ(final_layer.inner, std::vector<std::uint8_t>(message.begin(), message.end()));
}

TEST(LayerTest, RelayCannotDecryptInnerEnvelope) {
    auto r1 = crypto::generate_rsa(2048);
    auto r2 = crypto::generate_rsa(2048);
    auto dest = crypto::generate_rsa(2048);

    EVP_PKEY* r1_raw = r1.get();
    routing::Path path;
    path.relays.push_back(make_hop("100.64.0.2:9001", std::move(r1)));
    path.relays.push_back(make_hop("100.64.0.3:9001", std::move(r2)));
    path.destination = make_hop("100.64.0.5:9000", std::move(dest));

    auto onion = routing::build_onion(path, "secret");
    auto layer1 = routing::unwrap_layer(onion, r1_raw);
    EXPECT_THROW(routing::unwrap_layer(layer1.inner, r1_raw), onion::DecryptError);
}

TEST(LayerTest, WrongKeyFailsAtFirstHop) {
    auto r1 = crypto::generate_rsa(2048);
    auto wrong = crypto::generate_rsa(2048);
    auto dest = crypto::generate_rsa(2048);

    EVP_PKEY* wrong_raw = wrong.get();
    routing::Path path;
    path.relays.push_back(make_hop("100.64.0.2:9001", std::move(r1)));
    path.destination = make_hop("100.64.0.5:9000", std::move(dest));

    auto onion = routing::build_onion(path, "secret");
    EXPECT_THROW(routing::unwrap_layer(onion, wrong_raw), onion::DecryptError);
}

TEST(LayerTest, EveryTamperedByteIsRejected) {
    auto relay = crypto::generate_rsa(2048);
    auto dest = crypto::generate_rsa(2048);
    EVP_PKEY* relay_raw = relay.get();

    routing::Path path;
    path.relays.push_back(make_hop("127.0.0.1:9001", std::move(relay)));
    path.destination = make_hop("127.0.0.1:9000", std::move(dest));

    auto onion = routing::build_onion(path, "tamper me");
    ASSERT_FALSE(onion.empty());

    for (std::size_t i = 0; i < onion.size(); ++i) {
        auto tampered = onion;
        tampered[i] ^= 0x01;
        expect_rejected(tampered, relay_raw);
    }
}

TEST(LayerTest, EmptyPathIsRejected) {
    auto dest = crypto::generate_rsa(2048);
    routing::Path path;
    path.destination = make_hop("127.0.0.1:9000", std::move(dest));
    EXPECT_THROW(routing::build_onion(path, "x"), std::invalid_argument);
}

}  // namespace
