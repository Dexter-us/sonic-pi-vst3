// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <climits>
#include <cstddef>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace SonicPiVst3::SocketPlatform {

#if defined(_WIN32)

using Socket = SOCKET;
using SocketLength = int;
using PollDescriptor = WSAPOLLFD;
inline constexpr Socket kInvalidSocket = INVALID_SOCKET;
inline constexpr short kPollRead = POLLRDNORM;
inline constexpr short kPollWrite = POLLWRNORM;
inline constexpr short kPollError = POLLERR;
inline constexpr short kPollHangup = POLLHUP;
inline constexpr short kPollInvalid = POLLNVAL;

inline bool initialize() noexcept {
    static const bool initialized = [] {
        WSADATA data{};
        return WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }();
    return initialized;
}

inline int lastError() noexcept { return WSAGetLastError(); }
inline bool interrupted(int error) noexcept { return error == WSAEINTR; }
inline bool wouldBlock(int error) noexcept { return error == WSAEWOULDBLOCK; }

inline Socket create(int family, int type, int protocol) noexcept {
    if (!initialize()) return kInvalidSocket;
    return ::socket(family, type, protocol);
}

inline bool setNonBlocking(Socket socket) noexcept {
    u_long enabled = 1;
    return ioctlsocket(socket, FIONBIO, &enabled) == 0;
}

inline void close(Socket socket) noexcept {
    if (socket != kInvalidSocket) closesocket(socket);
}

inline int setOption(Socket socket, int level, int option,
                     const void* value, int valueBytes) noexcept {
    return setsockopt(socket, level, option,
                      static_cast<const char*>(value), valueBytes);
}

inline Socket accept(Socket socket, sockaddr* address,
                     SocketLength* addressBytes) noexcept {
    return ::accept(socket, address, addressBytes);
}

inline std::ptrdiff_t receive(Socket socket, void* data,
                              std::size_t capacity) noexcept {
    const int bytes = static_cast<int>(
        std::min<std::size_t>(capacity, static_cast<std::size_t>(INT_MAX)));
    const int result = ::recv(socket, static_cast<char*>(data), bytes, 0);
    return result == SOCKET_ERROR ? -1 : result;
}

inline std::ptrdiff_t send(Socket socket, const void* data,
                           std::size_t bytes, int flags) noexcept {
    const int count = static_cast<int>(
        std::min<std::size_t>(bytes, static_cast<std::size_t>(INT_MAX)));
    const int result = ::send(socket, static_cast<const char*>(data), count, flags);
    return result == SOCKET_ERROR ? -1 : result;
}

inline std::ptrdiff_t sendTo(Socket socket, const void* data,
                             std::size_t bytes, int flags,
                             const sockaddr* address,
                             SocketLength addressBytes) noexcept {
    const int count = static_cast<int>(
        std::min<std::size_t>(bytes, static_cast<std::size_t>(INT_MAX)));
    const int result = ::sendto(socket, static_cast<const char*>(data), count,
                                flags, address, addressBytes);
    return result == SOCKET_ERROR ? -1 : result;
}

inline int poll(PollDescriptor* descriptors, std::size_t count,
                int timeoutMs) noexcept {
    return WSAPoll(descriptors, static_cast<ULONG>(count), timeoutMs);
}

#else

using Socket = int;
using SocketLength = socklen_t;
using PollDescriptor = pollfd;
inline constexpr Socket kInvalidSocket = -1;
inline constexpr short kPollRead = POLLIN;
inline constexpr short kPollWrite = POLLOUT;
inline constexpr short kPollError = POLLERR;
inline constexpr short kPollHangup = POLLHUP;
inline constexpr short kPollInvalid = POLLNVAL;

inline bool initialize() noexcept { return true; }
inline int lastError() noexcept { return errno; }
inline bool interrupted(int error) noexcept { return error == EINTR; }
inline bool wouldBlock(int error) noexcept {
    return error == EAGAIN || error == EWOULDBLOCK;
}

inline Socket create(int family, int type, int protocol) noexcept {
    return ::socket(family, type, protocol);
}

inline bool setNonBlocking(Socket socket) noexcept {
    const int flags = fcntl(socket, F_GETFL, 0);
    return flags >= 0 && fcntl(socket, F_SETFL, flags | O_NONBLOCK) == 0;
}

inline void close(Socket socket) noexcept {
    if (socket != kInvalidSocket) ::close(socket);
}

inline int setOption(Socket socket, int level, int option,
                     const void* value, int valueBytes) noexcept {
    return setsockopt(socket, level, option, value,
                      static_cast<socklen_t>(valueBytes));
}

inline Socket accept(Socket socket, sockaddr* address,
                     SocketLength* addressBytes) noexcept {
    return ::accept(socket, address, addressBytes);
}

inline std::ptrdiff_t receive(Socket socket, void* data,
                              std::size_t capacity) noexcept {
    return ::recv(socket, data, capacity, 0);
}

inline std::ptrdiff_t send(Socket socket, const void* data,
                           std::size_t bytes, int flags) noexcept {
    return ::send(socket, data, bytes, flags);
}

inline std::ptrdiff_t sendTo(Socket socket, const void* data,
                             std::size_t bytes, int flags,
                             const sockaddr* address,
                             SocketLength addressBytes) noexcept {
    return ::sendto(socket, data, bytes, flags, address, addressBytes);
}

inline int poll(PollDescriptor* descriptors, std::size_t count,
                int timeoutMs) noexcept {
    return ::poll(descriptors, static_cast<nfds_t>(count), timeoutMs);
}

#endif

} // namespace SonicPiVst3::SocketPlatform
