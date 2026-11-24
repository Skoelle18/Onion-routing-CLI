#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#ifndef POLLIN
#define POLLIN 0x0001
#endif
#ifndef POLLOUT
#define POLLOUT 0x0004
#endif
#ifndef POLLERR
#define POLLERR 0x0008
#endif
#ifndef POLLHUP
#define POLLHUP 0x0010
#endif
#ifndef POLLNVAL
#define POLLNVAL 0x0020
#endif
#else
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace onion::net::platform {

#ifdef _WIN32
using Handle = SOCKET;
using SockLen = int;
using PollFd = WSAPOLLFD;
inline constexpr Handle kInvalidHandle = INVALID_SOCKET;
#else
using Handle = int;
using SockLen = socklen_t;
using PollFd = pollfd;
inline constexpr Handle kInvalidHandle = -1;
#endif

void ensure_networking();
std::string system_error(const std::string& context);

bool close_handle(Handle handle) noexcept;
bool set_nonblocking(Handle handle, bool enabled);
bool set_keepalive(Handle handle, bool enabled);

int poll_fds(PollFd* fds, unsigned long count, int timeout_ms);
long long send_data(Handle handle, const void* data, std::size_t length);
long long recv_data(Handle handle, void* data, std::size_t length);
bool get_socket_error(Handle handle, int& value);
bool get_peer_name(Handle handle, sockaddr_storage& storage, SockLen& length);
bool get_local_name(Handle handle, sockaddr_storage& storage, SockLen& length);

template <typename T>
bool set_option(Handle handle, int level, int name, const T& value) {
#ifdef _WIN32
    return ::setsockopt(handle, level, name, reinterpret_cast<const char*>(&value),
                        static_cast<int>(sizeof(value))) == 0;
#else
    return ::setsockopt(handle, level, name, &value, sizeof(value)) == 0;
#endif
}

}  // namespace onion::net::platform
