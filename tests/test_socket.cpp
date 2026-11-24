#include <gtest/gtest.h>

#include "onion/net/socket.hpp"

#include <atomic>
#include <chrono>
#include <exception>
#include <string>
#include <thread>

namespace {

namespace net = onion::net;
using namespace std::chrono_literals;

TEST(SocketTest, LoopbackPingPong) {
    auto server = net::listen_tcp(0);
    const auto port = net::local_port(server);
    ASSERT_NE(port, 0);

    std::atomic<net::FrameType> received{net::FrameType::Onion};
    std::string worker_error;
    std::string main_error;

    std::thread worker([&] {
        try {
            auto client = net::accept_connection(server);
            auto frame = net::read_frame(client, 3000ms);
            received.store(frame.type);
            net::write_frame(client, net::Frame{net::FrameType::Pong, {}}, 3000ms);
        } catch (const std::exception& ex) {
            worker_error = ex.what();
        }
    });

    try {
        net::ConnectOptions options;
        options.connect_timeout = 1500ms;
        options.retries = 0;
        auto client = net::connect_tcp(net::Address{"127.0.0.1", port}, options);

        net::write_frame(client, net::Frame{net::FrameType::Ping, {}}, 3000ms);
        auto reply = net::read_frame(client, 3000ms);
        EXPECT_EQ(reply.type, net::FrameType::Pong);
    } catch (const std::exception& ex) {
        main_error = ex.what();
    }

    worker.join();
    EXPECT_TRUE(main_error.empty()) << main_error;
    EXPECT_TRUE(worker_error.empty()) << worker_error;
    EXPECT_EQ(received.load(), net::FrameType::Ping);
}

TEST(SocketTest, ConnectToClosedPortFailsFast) {
    net::ConnectOptions options;
    options.connect_timeout = 300ms;
    options.retries = 0;

    EXPECT_THROW(net::connect_tcp(net::Address{"127.0.0.1", 1}, options),
                 std::exception);
}

}  // namespace
