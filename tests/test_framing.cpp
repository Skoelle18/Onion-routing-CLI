#include <gtest/gtest.h>

#include "onion/error.hpp"
#include "onion/net/address.hpp"
#include "onion/net/framing.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace {

namespace net = onion::net;

TEST(FramingTest, OnionFrameRoundTrip) {
    net::Frame frame;
    frame.type = net::FrameType::Onion;
    frame.body = {0, 1, 2, 3, 255, 128};

    auto bytes = net::encode_frame(frame);
    EXPECT_EQ(net::framed_length(bytes), bytes.size() - 4);

    auto decoded = net::decode_frame(bytes);
    EXPECT_EQ(decoded.type, net::FrameType::Onion);
    EXPECT_EQ(decoded.body, frame.body);
}

TEST(FramingTest, PingAndPongFrames) {
    net::Frame ping;
    ping.type = net::FrameType::Ping;
    EXPECT_EQ(net::decode_frame(net::encode_frame(ping)).type, net::FrameType::Ping);

    net::Frame pong;
    pong.type = net::FrameType::Pong;
    EXPECT_EQ(net::decode_frame(net::encode_frame(pong)).type, net::FrameType::Pong);
}

TEST(FramingTest, TruncatedFramesAreRejected) {
    EXPECT_THROW(net::decode_frame({}), onion::ProtocolError);
    EXPECT_THROW(net::decode_frame({0, 0}), onion::ProtocolError);

    net::Frame frame;
    frame.type = net::FrameType::Onion;
    frame.body = {1, 2, 3};
    auto bytes = net::encode_frame(frame);
    bytes.pop_back();
    EXPECT_THROW(net::decode_frame(bytes), onion::ProtocolError);
}

TEST(FramingTest, OversizedLengthIsRejected) {
    std::vector<std::uint8_t> prefix{0xFF, 0xFF, 0xFF, 0xFF};
    EXPECT_THROW(net::framed_length(prefix), onion::ProtocolError);
}

TEST(FramingTest, BadMagicAndTypeAreRejected) {
    net::Frame frame;
    frame.type = net::FrameType::Ping;
    auto bytes = net::encode_frame(frame);
    bytes[4] ^= 0x01;
    EXPECT_THROW(net::decode_frame(bytes), onion::ProtocolError);

    auto bad_type = net::encode_frame(frame);
    bad_type[8] = 0x99;
    EXPECT_THROW(net::decode_frame(bad_type), onion::ProtocolError);
}

TEST(FramingTest, StatusRoundTrip) {
    auto frame = net::make_status(net::StatusCode::DecryptFailed, "bad tag");
    auto decoded = net::decode_frame(net::encode_frame(frame));
    auto [code, text] = net::parse_status(decoded);
    EXPECT_EQ(code, net::StatusCode::DecryptFailed);
    EXPECT_EQ(text, "bad tag");
    EXPECT_EQ(net::status_name(code), "DECRYPT_FAILED");
}

TEST(FramingTest, MalformedStatusIsRejected) {
    net::Frame frame;
    frame.type = net::FrameType::Status;
    frame.body = {0, 0};
    EXPECT_THROW(net::parse_status(frame), onion::ProtocolError);

    frame.body = {0, 0, 5, 'a'};
    EXPECT_THROW(net::parse_status(frame), onion::ProtocolError);

    frame.type = net::FrameType::Ping;
    EXPECT_THROW(net::parse_status(frame), onion::ProtocolError);
}

TEST(AddressTest, ParsesIpv4AndHostnames) {
    auto ipv4 = net::parse_address("100.64.0.2:9001");
    EXPECT_EQ(ipv4.host, "100.64.0.2");
    EXPECT_EQ(ipv4.port, 9001);

    auto host = net::parse_address("relay.example:1234");
    EXPECT_EQ(host.host, "relay.example");
    EXPECT_EQ(host.port, 1234);
}

TEST(AddressTest, ParsesBracketedIpv6) {
    auto address = net::parse_address("[::1]:9000");
    EXPECT_EQ(address.host, "::1");
    EXPECT_EQ(address.port, 9000);
    EXPECT_EQ(net::format_address(address), "[::1]:9000");
}

TEST(AddressTest, RejectsMalformedAddresses) {
    EXPECT_THROW(net::parse_address(""), onion::ProtocolError);
    EXPECT_THROW(net::parse_address("100.64.0.2"), onion::ProtocolError);
    EXPECT_THROW(net::parse_address("100.64.0.2:0"), onion::ProtocolError);
    EXPECT_THROW(net::parse_address("100.64.0.2:70000"), onion::ProtocolError);
    EXPECT_THROW(net::parse_address("100.64.0.2:abc"), onion::ProtocolError);
    EXPECT_THROW(net::parse_address("::1:9000"), onion::ProtocolError);
    EXPECT_THROW(net::parse_address("[::1:9000"), onion::ProtocolError);
}

}  // namespace
