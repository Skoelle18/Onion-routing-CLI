#pragma once

#include "onion/net/address.hpp"
#include "onion/net/framing.hpp"
#include "onion/net/platform.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace onion::net {

class Socket {
public:
    explicit Socket(platform::Handle handle = platform::kInvalidHandle) noexcept
        : handle_(handle) {}
    ~Socket();

    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;

    Socket(Socket&& other) noexcept;
    Socket& operator=(Socket&& other) noexcept;

    platform::Handle get() const noexcept { return handle_; }
    bool valid() const noexcept { return handle_ != platform::kInvalidHandle; }
    void close() noexcept;

private:
    platform::Handle handle_;
};

struct ConnectOptions {
    std::chrono::milliseconds connect_timeout{2000};
    int retries{2};
    std::chrono::milliseconds backoff{200};
    bool keepalive{true};
};

Socket connect_tcp(const Address& address, const ConnectOptions& options = {});
Socket listen_tcp(std::uint16_t port, int backlog = 16);
Socket accept_connection(const Socket& server);
std::uint16_t local_port(const Socket& socket);

std::string peer_address(const Socket& socket);
void set_keepalive(const Socket& socket, bool enabled = true);

std::vector<std::uint8_t> read_exact(const Socket& socket, std::size_t length,
                                     std::chrono::milliseconds timeout);
void write_all(const Socket& socket, const std::uint8_t* data, std::size_t length,
               std::chrono::milliseconds timeout);

Frame read_frame(const Socket& socket, std::chrono::milliseconds timeout);
void write_frame(const Socket& socket, const Frame& frame,
                 std::chrono::milliseconds timeout);

}  // namespace onion::net
