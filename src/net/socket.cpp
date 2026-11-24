#include "onion/net/socket.hpp"

#include "onion/error.hpp"

#ifndef _WIN32
#include <arpa/inet.h>
#include <netinet/tcp.h>
#endif

#include <algorithm>
#include <chrono>
#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace onion::net {
namespace {

constexpr std::chrono::milliseconds kIoTimeout{5000};

bool retryable_io_error() {
#ifdef _WIN32
    const int code = ::WSAGetLastError();
    return code == WSAEINTR || code == WSAEWOULDBLOCK;
#else
    return errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK || errno == EINPROGRESS;
#endif
}

bool wait_for_event(platform::Handle handle, short events, std::chrono::milliseconds timeout,
                    const std::string& context) {
    platform::PollFd pfd{};
    pfd.fd = handle;
    pfd.events = events;

    platform::ensure_networking();
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (true) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            throw TimeoutError(context + ": timed out after " +
                               std::to_string(timeout.count()) + " ms");
        }
        const auto remaining =
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
        const int wait_ms = static_cast<int>(std::max<long long>(1, remaining.count()));
        const int rc = platform::poll_fds(&pfd, 1, wait_ms);
        if (rc > 0) {
            if ((pfd.revents & events) != 0) {
                return true;
            }
            if ((pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
                throw NetworkError(context + ": socket error");
            }
            continue;
        }
        if (rc < 0 && retryable_io_error()) {
            continue;
        }
        if (rc < 0) {
            throw NetworkError(platform::system_error(context + ": poll"));
        }
    }
}

void connect_once(const Address& address, std::chrono::milliseconds timeout, bool keepalive,
                  Socket& result) {
    platform::ensure_networking();

    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_ADDRCONFIG;

    addrinfo* list = nullptr;
    const int rc = ::getaddrinfo(address.host.c_str(), std::to_string(address.port).c_str(),
                                 &hints, &list);
    if (rc != 0 || list == nullptr) {
        throw NetworkError("cannot resolve '" + address.host + "': " + gai_strerror(rc));
    }

    std::string last_error = "no usable address";
    bool timed_out = false;
    for (addrinfo* item = list; item != nullptr; item = item->ai_next) {
        platform::Handle fd = ::socket(item->ai_family, item->ai_socktype, item->ai_protocol);
        if (fd == platform::kInvalidHandle) {
            last_error = platform::system_error("socket");
            continue;
        }

        try {
            if (!platform::set_nonblocking(fd, true)) {
                last_error = platform::system_error("set_nonblocking");
                platform::close_handle(fd);
                continue;
            }
            int connect_rc = ::connect(fd, item->ai_addr,
                                       static_cast<platform::SockLen>(item->ai_addrlen));
            if (connect_rc == 0 || retryable_io_error()) {
                if (connect_rc != 0) {
                    wait_for_event(fd, POLLOUT, timeout,
                                   "connect to " + format_address(address));
                }
                int so_error = 0;
                if (!platform::get_socket_error(fd, so_error)) {
                    last_error = platform::system_error("getsockopt(SO_ERROR)");
                    platform::close_handle(fd);
                    continue;
                }
                if (so_error != 0) {
#ifdef _WIN32
                    ::WSASetLastError(so_error);
#endif
                    last_error = platform::system_error("connect to " + format_address(address));
                    platform::close_handle(fd);
                    continue;
                }
                if (!platform::set_nonblocking(fd, false)) {
                    last_error = platform::system_error("set_nonblocking");
                    platform::close_handle(fd);
                    continue;
                }
                Socket socket(fd);
                fd = platform::kInvalidHandle;
                if (keepalive) {
                    platform::set_keepalive(socket.get(), true);
                }
                result = std::move(socket);
                ::freeaddrinfo(list);
                return;
            }
            last_error = platform::system_error("connect to " + format_address(address));
        } catch (const TimeoutError&) {
            last_error = "connect to " + format_address(address) + " timed out";
            timed_out = true;
        } catch (const NetworkError& ex) {
            last_error = ex.what();
        }
        if (fd != platform::kInvalidHandle) {
            platform::close_handle(fd);
        }
    }

    ::freeaddrinfo(list);
    if (timed_out) {
        throw TimeoutError(last_error);
    }
    throw NetworkError(last_error);
}

}  // namespace

Socket::~Socket() {
    close();
}

Socket::Socket(Socket&& other) noexcept : handle_(other.handle_) {
    other.handle_ = platform::kInvalidHandle;
}

Socket& Socket::operator=(Socket&& other) noexcept {
    if (this != &other) {
        close();
        handle_ = other.handle_;
        other.handle_ = platform::kInvalidHandle;
    }
    return *this;
}

void Socket::close() noexcept {
    if (handle_ != platform::kInvalidHandle) {
        platform::close_handle(handle_);
        handle_ = platform::kInvalidHandle;
    }
}

void set_keepalive(const Socket& socket, bool enabled) {
    platform::ensure_networking();
    if (!platform::set_keepalive(socket.get(), enabled)) {
        throw NetworkError(platform::system_error("setsockopt(SO_KEEPALIVE)"));
    }
}

Socket connect_tcp(const Address& address, const ConnectOptions& options) {
    if (options.retries < 0) {
        throw std::invalid_argument("retries must be non-negative");
    }

    std::string last_error = "connection failed";
    bool last_was_timeout = false;
    for (int attempt = 0; attempt <= options.retries; ++attempt) {
        try {
            Socket socket;
            connect_once(address, options.connect_timeout, options.keepalive, socket);
            return socket;
        } catch (const TimeoutError& ex) {
            last_error = ex.what();
            last_was_timeout = true;
        } catch (const NetworkError& ex) {
            last_error = ex.what();
            last_was_timeout = false;
        }
        if (attempt < options.retries) {
            const auto delay = options.backoff * (1 << std::min(attempt, 10));
            std::this_thread::sleep_for(delay);
        }
    }
    const std::string prefix = last_was_timeout ? "timeout connecting to " : "unreachable hop ";
    if (last_was_timeout) {
        throw TimeoutError(prefix + format_address(address) + ": " + last_error);
    }
    throw NetworkError(prefix + format_address(address) + ": " + last_error);
}

Socket listen_tcp(std::uint16_t port, int backlog) {
    platform::ensure_networking();

    platform::Handle fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd == platform::kInvalidHandle) {
        throw NetworkError(platform::system_error("socket"));
    }
    int reuse = 1;
    if (!platform::set_option(fd, SOL_SOCKET, SO_REUSEADDR, reuse)) {
        const std::string message = platform::system_error("setsockopt(SO_REUSEADDR)");
        platform::close_handle(fd);
        throw NetworkError(message);
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr),
               static_cast<platform::SockLen>(sizeof(addr))) != 0) {
        const std::string message =
            platform::system_error("bind to 0.0.0.0:" + std::to_string(port));
        platform::close_handle(fd);
        throw NetworkError(message);
    }
    if (::listen(fd, backlog) != 0) {
        const std::string message = platform::system_error("listen");
        platform::close_handle(fd);
        throw NetworkError(message);
    }
    return Socket(fd);
}

Socket accept_connection(const Socket& server) {
    platform::ensure_networking();
    platform::Handle fd = ::accept(server.get(), nullptr, nullptr);
    if (fd == platform::kInvalidHandle) {
#ifndef _WIN32
        if (errno == EINTR) {
            return accept_connection(server);
        }
#endif
        throw NetworkError(platform::system_error("accept"));
    }
    return Socket(fd);
}

std::uint16_t local_port(const Socket& socket) {
    platform::ensure_networking();
    sockaddr_storage storage{};
    platform::SockLen length = static_cast<platform::SockLen>(sizeof(storage));
    if (!platform::get_local_name(socket.get(), storage, length)) {
        throw NetworkError(platform::system_error("getsockname"));
    }
    if (storage.ss_family == AF_INET) {
        return ntohs(reinterpret_cast<sockaddr_in*>(&storage)->sin_port);
    }
    if (storage.ss_family == AF_INET6) {
        return ntohs(reinterpret_cast<sockaddr_in6*>(&storage)->sin6_port);
    }
    throw ProtocolError("socket is not an IP socket");
}

std::string peer_address(const Socket& socket) {
    platform::ensure_networking();
    sockaddr_storage storage{};
    platform::SockLen length = static_cast<platform::SockLen>(sizeof(storage));
    if (!platform::get_peer_name(socket.get(), storage, length)) {
        return "unknown";
    }

    char host[INET6_ADDRSTRLEN]{};
    Address address;
    if (storage.ss_family == AF_INET) {
        auto* in = reinterpret_cast<sockaddr_in*>(&storage);
        inet_ntop(AF_INET, &in->sin_addr, host, sizeof(host));
        address.port = ntohs(in->sin_port);
    } else if (storage.ss_family == AF_INET6) {
        auto* in = reinterpret_cast<sockaddr_in6*>(&storage);
        inet_ntop(AF_INET6, &in->sin6_addr, host, sizeof(host));
        address.port = ntohs(in->sin6_port);
    } else {
        return "unknown";
    }
    address.host = host;
    return format_address(address);
}

std::vector<std::uint8_t> read_exact(const Socket& socket, std::size_t length,
                                     std::chrono::milliseconds timeout) {
    std::vector<std::uint8_t> buffer(length);
    std::size_t received = 0;
    while (received < length) {
        wait_for_event(socket.get(), POLLIN, timeout, "read from " + peer_address(socket));
        const long long rc = platform::recv_data(socket.get(), buffer.data() + received,
                                                 length - received);
        if (rc > 0) {
            received += static_cast<std::size_t>(rc);
            continue;
        }
        if (rc == 0) {
            throw NetworkError("connection closed by " + peer_address(socket) +
                               " while reading frame");
        }
        if (retryable_io_error()) {
            continue;
        }
        throw NetworkError(platform::system_error("recv from " + peer_address(socket)));
    }
    return buffer;
}

void write_all(const Socket& socket, const std::uint8_t* data, std::size_t length,
               std::chrono::milliseconds timeout) {
    std::size_t sent = 0;
    while (sent < length) {
        wait_for_event(socket.get(), POLLOUT, timeout, "write to " + peer_address(socket));
        const long long rc = platform::send_data(socket.get(), data + sent, length - sent);
        if (rc > 0) {
            sent += static_cast<std::size_t>(rc);
            continue;
        }
        if (retryable_io_error()) {
            continue;
        }
        throw NetworkError(platform::system_error("send to " + peer_address(socket)));
    }
}

Frame read_frame(const Socket& socket, std::chrono::milliseconds timeout) {
    auto prefix = read_exact(socket, 4, timeout);
    const std::uint32_t payload_len = framed_length(prefix);
    auto payload = read_exact(socket, payload_len, timeout);
    prefix.insert(prefix.end(), payload.begin(), payload.end());
    return decode_frame(prefix);
}

void write_frame(const Socket& socket, const Frame& frame,
                 std::chrono::milliseconds timeout) {
    const auto bytes = encode_frame(frame);
    write_all(socket, bytes.data(), bytes.size(), timeout);
}

}  // namespace onion::net
