#include "onion/net/platform.hpp"

#include "onion/error.hpp"

#ifdef _WIN32
#include <mstcpip.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <netinet/tcp.h>
#include <mutex>
#endif

#include <algorithm>
#include <cstring>
#include <limits>
#include <mutex>

namespace onion::net::platform {
namespace {

#ifdef _WIN32
std::once_flag wsa_flag;
#endif

long long clamp_length(std::size_t length) {
    return static_cast<long long>(std::min<std::size_t>(
        length, static_cast<std::size_t>(std::numeric_limits<int>::max())));
}

}  // namespace

void ensure_networking() {
#ifdef _WIN32
    std::call_once(wsa_flag, [] {
        WSADATA data{};
        if (::WSAStartup(MAKEWORD(2, 2), &data) != 0) {
            throw NetworkError("WSAStartup failed");
        }
    });
#endif
}

std::string system_error(const std::string& context) {
#ifdef _WIN32
    const DWORD code = ::WSAGetLastError();
    if (code == 0) {
        return context + ": no WinSock error available";
    }
    LPSTR message = nullptr;
    const DWORD count = ::FormatMessageA(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
            FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, code, 0, reinterpret_cast<LPSTR>(&message), 0, nullptr);
    std::string text;
    if (count > 0 && message != nullptr) {
        text.assign(message, count);
        while (!text.empty() && (text.back() == '\r' || text.back() == '\n')) {
            text.pop_back();
        }
        ::LocalFree(message);
    } else {
        text = "WinSock error " + std::to_string(code);
    }
    return context + ": " + text;
#else
    return context + ": " + std::strerror(errno);
#endif
}

bool close_handle(Handle handle) noexcept {
    if (handle == kInvalidHandle) {
        return true;
    }
#ifdef _WIN32
    return ::closesocket(handle) == 0;
#else
    return ::close(handle) == 0;
#endif
}

bool set_nonblocking(Handle handle, bool enabled) {
#ifdef _WIN32
    u_long mode = enabled ? 1UL : 0UL;
    return ::ioctlsocket(handle, FIONBIO, &mode) == 0;
#else
    int flags = ::fcntl(handle, F_GETFL, 0);
    if (flags < 0) {
        return false;
    }
    const int desired = enabled ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK);
    return ::fcntl(handle, F_SETFL, desired) == 0;
#endif
}

bool set_keepalive(Handle handle, bool enabled) {
    if (!set_option(handle, SOL_SOCKET, SO_KEEPALIVE, enabled ? 1 : 0)) {
        return false;
    }
#ifdef _WIN32
    if (enabled) {
        tcp_keepalive values{};
        values.onoff = 1;
        values.keepalivetime = 30000;
        values.keepaliveinterval = 5000;
        DWORD bytes_returned = 0;
        ::WSAIoctl(handle, SIO_KEEPALIVE_VALS, &values, sizeof(values), nullptr, 0,
                   &bytes_returned, nullptr, nullptr);
    }
#else
    if (enabled) {
        int idle = 30;
        int interval = 5;
        int count = 3;
        set_option(handle, IPPROTO_TCP, TCP_KEEPIDLE, idle);
        set_option(handle, IPPROTO_TCP, TCP_KEEPINTVL, interval);
        set_option(handle, IPPROTO_TCP, TCP_KEEPCNT, count);
    }
#endif
    return true;
}

int poll_fds(PollFd* fds, unsigned long count, int timeout_ms) {
#ifdef _WIN32
    return ::WSAPoll(fds, count, timeout_ms);
#else
    return ::poll(fds, count, timeout_ms);
#endif
}

long long send_data(Handle handle, const void* data, std::size_t length) {
#ifdef _WIN32
    const int rc = ::send(handle, static_cast<const char*>(data),
                          static_cast<int>(clamp_length(length)), 0);
    return rc == SOCKET_ERROR ? -1 : static_cast<long long>(rc);
#else
    const ssize_t rc = ::send(handle, data, clamp_length(length), MSG_NOSIGNAL);
    return static_cast<long long>(rc);
#endif
}

long long recv_data(Handle handle, void* data, std::size_t length) {
#ifdef _WIN32
    const int rc = ::recv(handle, static_cast<char*>(data),
                          static_cast<int>(clamp_length(length)), 0);
    return rc == SOCKET_ERROR ? -1 : static_cast<long long>(rc);
#else
    return static_cast<long long>(::recv(handle, data, clamp_length(length), 0));
#endif
}

bool get_socket_error(Handle handle, int& value) {
#ifdef _WIN32
    int length = static_cast<int>(sizeof(value));
    return ::getsockopt(handle, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&value),
                        &length) == 0;
#else
    socklen_t length = static_cast<socklen_t>(sizeof(value));
    return ::getsockopt(handle, SOL_SOCKET, SO_ERROR, &value, &length) == 0;
#endif
}

bool get_peer_name(Handle handle, sockaddr_storage& storage, SockLen& length) {
#ifdef _WIN32
    return ::getpeername(handle, reinterpret_cast<sockaddr*>(&storage), &length) == 0;
#else
    return ::getpeername(handle, reinterpret_cast<sockaddr*>(&storage), &length) == 0;
#endif
}

bool get_local_name(Handle handle, sockaddr_storage& storage, SockLen& length) {
#ifdef _WIN32
    return ::getsockname(handle, reinterpret_cast<sockaddr*>(&storage), &length) == 0;
#else
    return ::getsockname(handle, reinterpret_cast<sockaddr*>(&storage), &length) == 0;
#endif
}

}  // namespace onion::net::platform
