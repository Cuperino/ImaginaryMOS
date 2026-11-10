// SPDX-FileCopyrightText: 2026 Javier O. Cordero Pérez
// SPDX-License-Identifier: Apache-2.0

#include "tcp28_socket.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <string_view>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <unistd.h>
#endif

namespace imos::transports::tcp28::detail {

namespace {

int lastErrorCode() {
#if defined(_WIN32)
    return WSAGetLastError();
#else
    return errno;
#endif
}

std::string errorTextFor(int code) {
#if defined(_WIN32)
    char buffer[256] = {};
    FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr,
                   static_cast<DWORD>(code), 0, buffer, sizeof(buffer), nullptr);
    std::string text{buffer};
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) {
        text.pop_back();
    }
    return text.empty() ? "winsock error " + std::to_string(code) : text;
#else
    return std::strerror(code);
#endif
}

/// getaddrinfo for a numeric-or-named host, one shot, first result wins.
Result<SocketHandle> openFirstResolved(const std::string& host, int port, bool forBind,
                                       sockaddr_storage& address, int& addressLength) {
    addrinfo hints = {};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    if (forBind) {
        hints.ai_flags = AI_PASSIVE;
    }
    const std::string service = std::to_string(port);
    addrinfo* results = nullptr;
    const int rc =
        ::getaddrinfo(host.empty() ? nullptr : host.c_str(), service.c_str(), &hints, &results);
    if (rc != 0 || results == nullptr) {
#if defined(_WIN32)
        const std::string reason = errorTextFor(rc);
#else
        const std::string reason = ::gai_strerror(rc);
#endif
        return makeError(ErrorCode::IoError, "cannot resolve " + host + ": " + reason);
    }
    const SocketHandle socket =
        ::socket(results->ai_family, results->ai_socktype, results->ai_protocol);
    if (socket == kInvalidSocket) {
        const std::string reason = errorTextFor(lastErrorCode());
        ::freeaddrinfo(results);
        return makeError(ErrorCode::IoError, "cannot create socket: " + reason);
    }
    std::memcpy(&address, results->ai_addr, results->ai_addrlen);
    addressLength = static_cast<int>(results->ai_addrlen);
    ::freeaddrinfo(results);
    return socket;
}

}  // namespace

void ensureNetSystem() {
#if defined(_WIN32)
    static const bool initialized = [] {
        WSADATA data;
        return WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }();
    (void)initialized;
#endif
}

int pollSockets(std::span<PollFd> fds, int timeoutMillis) {
#if defined(_WIN32)
    return WSAPoll(fds.data(), static_cast<ULONG>(fds.size()), timeoutMillis);
#else
    int rc = 0;
    do {
        rc = ::poll(fds.data(), fds.size(), timeoutMillis);
    } while (rc < 0 && errno == EINTR);
    return rc;
#endif
}

void setNonBlocking(SocketHandle socket) {
#if defined(_WIN32)
    u_long mode = 1;
    ::ioctlsocket(socket, FIONBIO, &mode);
#else
    const int flags = ::fcntl(socket, F_GETFL, 0);
    ::fcntl(socket, F_SETFL, flags | O_NONBLOCK);
#endif
}

void setNoDelay(SocketHandle socket) {
    const int one = 1;
    ::setsockopt(socket, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&one),
                 sizeof(one));
}

void closeSocket(SocketHandle socket) {
    if (socket == kInvalidSocket) {
        return;
    }
#if defined(_WIN32)
    ::closesocket(socket);
#else
    ::close(socket);
#endif
}

void shutdownWrite(SocketHandle socket) {
    if (socket == kInvalidSocket) {
        return;
    }
#if defined(_WIN32)
    ::shutdown(socket, SD_SEND);
#else
    ::shutdown(socket, SHUT_WR);
#endif
}

bool lastErrorWouldBlock() {
#if defined(_WIN32)
    return WSAGetLastError() == WSAEWOULDBLOCK;
#else
    return errno == EAGAIN || errno == EWOULDBLOCK;
#endif
}

bool lastErrorConnectInProgress() {
#if defined(_WIN32)
    const int code = WSAGetLastError();
    return code == WSAEWOULDBLOCK || code == WSAEINPROGRESS;
#else
    return errno == EINPROGRESS;
#endif
}

std::string lastErrorText() {
    return errorTextFor(lastErrorCode());
}

std::int64_t readSome(SocketHandle socket, std::span<std::uint8_t> buffer) {
#if defined(_WIN32)
    const int rc =
        ::recv(socket, reinterpret_cast<char*>(buffer.data()), static_cast<int>(buffer.size()), 0);
#else
    const ssize_t rc = ::recv(socket, buffer.data(), buffer.size(), 0);
#endif
    if (rc > 0) {
        return rc;
    }
    if (rc == 0) {
        return 0;
    }
    return lastErrorWouldBlock() ? -1 : -2;
}

std::int64_t writeSome(SocketHandle socket, std::span<const std::uint8_t> data) {
#if defined(_WIN32)
    const int rc = ::send(socket, reinterpret_cast<const char*>(data.data()),
                          static_cast<int>(data.size()), 0);
#else
    const ssize_t rc = ::send(socket, data.data(), data.size(), MSG_NOSIGNAL);
#endif
    if (rc >= 0) {
        return rc;
    }
    return lastErrorWouldBlock() ? -1 : -2;
}

Result<SocketHandle> beginConnect(const std::string& host, int port) {
    sockaddr_storage address = {};
    int addressLength = 0;
    Result<SocketHandle> opened =
        openFirstResolved(host, port, /*forBind=*/false, address, addressLength);
    if (!opened.ok()) {
        return opened;
    }
    const SocketHandle socket = opened.value();
    setNonBlocking(socket);
    setNoDelay(socket);
    const int rc = ::connect(socket, reinterpret_cast<const sockaddr*>(&address),
                             static_cast<socklen_t>(addressLength));
    if (rc == 0 || lastErrorConnectInProgress()) {
        return socket;
    }
    const std::string reason = lastErrorText();
    closeSocket(socket);
    return makeError(ErrorCode::IoError,
                     "connect to " + host + ":" + std::to_string(port) + " failed: " + reason);
}

int connectFinished(SocketHandle socket, std::string& error) {
    int soError = 0;
    socklen_t length = sizeof(soError);
    if (::getsockopt(socket, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&soError), &length) !=
        0) {
        error = lastErrorText();
        return -1;
    }
    if (soError == 0) {
        return 1;
    }
#if !defined(_WIN32)
    if (soError == EINPROGRESS) {
        return 0;
    }
#endif
    error = errorTextFor(soError);
    return -1;
}

Result<SocketHandle> listenOn(const std::string& address, int port, int& boundPort) {
    sockaddr_storage storage = {};
    int addressLength = 0;
    Result<SocketHandle> opened =
        openFirstResolved(address, port, /*forBind=*/true, storage, addressLength);
    if (!opened.ok()) {
        return opened;
    }
    const SocketHandle socket = opened.value();
    const int one = 1;
#if !defined(_WIN32)
    // Listeners restart across tests and reconnect cycles; on Windows,
    // SO_REUSEADDR means something dangerous (port hijack) and TIME_WAIT
    // rebinding already works, so POSIX only.
    ::setsockopt(socket, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&one),
                 sizeof(one));
#endif
    (void)one;
    if (::bind(socket, reinterpret_cast<const sockaddr*>(&storage),
               static_cast<socklen_t>(addressLength)) != 0) {
        const std::string reason = lastErrorText();
        closeSocket(socket);
        return makeError(ErrorCode::IoError,
                         "cannot bind " + address + ":" + std::to_string(port) + ": " + reason);
    }
    if (::listen(socket, SOMAXCONN) != 0) {
        const std::string reason = lastErrorText();
        closeSocket(socket);
        return makeError(ErrorCode::IoError, "cannot listen on " + address + ":" +
                                                 std::to_string(port) + ": " + reason);
    }
    sockaddr_storage bound = {};
    socklen_t boundLength = sizeof(bound);
    if (::getsockname(socket, reinterpret_cast<sockaddr*>(&bound), &boundLength) == 0) {
        if (bound.ss_family == AF_INET) {
            boundPort = ntohs(reinterpret_cast<const sockaddr_in*>(&bound)->sin_port);
        } else if (bound.ss_family == AF_INET6) {
            boundPort = ntohs(reinterpret_cast<const sockaddr_in6*>(&bound)->sin6_port);
        }
    }
    setNonBlocking(socket);
    return socket;
}

namespace {

std::string numericAddress(const sockaddr* address, socklen_t length) {
    char host[NI_MAXHOST] = {};
    if (::getnameinfo(address, length, host, sizeof(host), nullptr, 0, NI_NUMERICHOST) != 0) {
        return {};
    }
    std::string text{host};
    // "::ffff:192.0.2.1" and "192.0.2.1" are the same peer.
    constexpr std::string_view kMapped = "::ffff:";
    if (text.size() > kMapped.size() && text.compare(0, kMapped.size(), kMapped) == 0 &&
        text.find('.') != std::string::npos) {
        text.erase(0, kMapped.size());
    }
    return text;
}

}  // namespace

std::vector<std::string> resolveNumericAddresses(const std::string& host) {
    std::vector<std::string> out;
    addrinfo hints = {};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* results = nullptr;
    if (::getaddrinfo(host.c_str(), nullptr, &hints, &results) != 0 || results == nullptr) {
        return out;
    }
    for (const addrinfo* it = results; it != nullptr; it = it->ai_next) {
        std::string text = numericAddress(it->ai_addr, static_cast<socklen_t>(it->ai_addrlen));
        if (!text.empty() && std::find(out.begin(), out.end(), text) == out.end()) {
            out.push_back(std::move(text));
        }
    }
    ::freeaddrinfo(results);
    return out;
}

SocketHandle acceptOne(SocketHandle listener, std::string* remoteAddress) {
    sockaddr_storage remote = {};
    socklen_t remoteLength = sizeof(remote);
    const SocketHandle accepted =
        ::accept(listener, reinterpret_cast<sockaddr*>(&remote), &remoteLength);
    if (accepted == kInvalidSocket) {
        return kInvalidSocket;
    }
    if (remoteAddress != nullptr) {
        *remoteAddress = numericAddress(reinterpret_cast<const sockaddr*>(&remote), remoteLength);
    }
    setNonBlocking(accepted);
    setNoDelay(accepted);
    return accepted;
}

WakePipe::WakePipe() {
    // The pipe is a member of the endpoint's state, so it is built before
    // the endpoint's constructor body runs: bring the network up here, or
    // the first endpoint in a process gets no wake pipe on Windows.
    ensureNetSystem();
#if defined(_WIN32)
    // No pipes in WSAPoll's world: emulate with a loopback TCP pair.
    int port = 0;
    Result<SocketHandle> listener = listenOn("127.0.0.1", 0, port);
    if (!listener.ok()) {
        return;
    }
    Result<SocketHandle> writer = beginConnect("127.0.0.1", port);
    if (!writer.ok()) {
        closeSocket(listener.value());
        return;
    }
    // Loopback connects complete immediately for practical purposes; poll
    // briefly for the accept side.
    for (int attempt = 0; attempt < 100; ++attempt) {
        const SocketHandle accepted = acceptOne(listener.value());
        if (accepted != kInvalidSocket) {
            readEnd_ = accepted;
            break;
        }
        Sleep(1);
    }
    closeSocket(listener.value());
    if (readEnd_ == kInvalidSocket) {
        closeSocket(writer.value());
        return;
    }
    writeEnd_ = writer.value();
#else
    int fds[2] = {kInvalidSocket, kInvalidSocket};
    if (::pipe(fds) == 0) {
        readEnd_ = fds[0];
        writeEnd_ = fds[1];
        isPipe_ = true;
        setNonBlocking(readEnd_);
        setNonBlocking(writeEnd_);
    }
#endif
}

WakePipe::~WakePipe() {
    closeSocket(writeEnd_);
#if defined(_WIN32)
    closeSocket(readEnd_);
#else
    if (readEnd_ != kInvalidSocket) {
        ::close(readEnd_);
    }
#endif
}

void WakePipe::wake() {
    if (writeEnd_ == kInvalidSocket) {
        return;
    }
    const std::uint8_t byte = 1;
#if defined(_WIN32)
    ::send(writeEnd_, reinterpret_cast<const char*>(&byte), 1, 0);
#else
    if (isPipe_) {
        [[maybe_unused]] const ssize_t rc = ::write(writeEnd_, &byte, 1);
    }
#endif
}

void WakePipe::drain() {
    if (readEnd_ == kInvalidSocket) {
        return;
    }
    std::uint8_t buffer[64];
#if defined(_WIN32)
    while (::recv(readEnd_, reinterpret_cast<char*>(buffer), sizeof(buffer), 0) > 0) {
    }
#else
    while (::read(readEnd_, buffer, sizeof(buffer)) > 0) {
    }
#endif
}

}  // namespace imos::transports::tcp28::detail
