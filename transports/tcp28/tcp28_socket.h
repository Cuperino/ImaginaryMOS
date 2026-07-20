// SPDX-FileCopyrightText: 2026 Javier O. Cordero Pérez
// SPDX-License-Identifier: Apache-2.0
//
// Minimal cross-platform socket layer for the 2.8 TCP transport (plan
// §5.2): POSIX sockets / Winsock2, non-blocking, level-triggered readiness
// via poll()/WSAPoll. Deliberately not IXWebSocket (that stays on
// WebSocket/HTTP duty) and deliberately tiny — just what tcp28_endpoint.cpp
// needs. Internal; not installed.

#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <imos/error.h>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#endif

namespace imos::transports::tcp28::detail {

#if defined(_WIN32)
using SocketHandle = SOCKET;
inline constexpr SocketHandle kInvalidSocket = INVALID_SOCKET;
using PollFd = WSAPOLLFD;
#else
using SocketHandle = int;
inline constexpr SocketHandle kInvalidSocket = -1;
using PollFd = pollfd;
#endif

/// One-time platform network bring-up (WSAStartup on Windows; no-op
/// elsewhere). Never torn down — process lifetime, same policy as ws40.
void ensureNetSystem();

/// poll()/WSAPoll with a millisecond timeout. Returns the ready count or
/// negative on error (EINTR is retried internally).
int pollSockets(std::span<PollFd> fds, int timeoutMillis);

void setNonBlocking(SocketHandle socket);
void setNoDelay(SocketHandle socket);
void closeSocket(SocketHandle socket);
/// Half-close the write side (the first step of the four-way shutdown).
void shutdownWrite(SocketHandle socket);

/// True when the last socket call failed because it would block.
bool lastErrorWouldBlock();
/// True when a non-blocking connect() reported "in progress".
bool lastErrorConnectInProgress();
std::string lastErrorText();

/// Read into `buffer`; returns bytes read, 0 on orderly EOF, -1 on
/// would-block, -2 on hard error.
std::int64_t readSome(SocketHandle socket, std::span<std::uint8_t> buffer);
/// Write from `data`; returns bytes written (possibly short), -1 on
/// would-block, -2 on hard error.
std::int64_t writeSome(SocketHandle socket, std::span<const std::uint8_t> data);

/// Resolve `host:port` and begin a non-blocking connect. On success the
/// socket is connecting (or connected); completion is observed via
/// POLLOUT + getsockopt(SO_ERROR) — see connectFinished().
Result<SocketHandle> beginConnect(const std::string& host, int port);
/// After POLLOUT on a connecting socket: 1 connected, 0 still in progress,
/// -1 failed (error text via lastErrorText-style message in `error`).
int connectFinished(SocketHandle socket, std::string& error);

/// Bind + listen on `address:port` (port 0 → ephemeral). Returns the
/// listening socket; `boundPort` reports the resolved port.
Result<SocketHandle> listenOn(const std::string& address, int port, int& boundPort);
/// Accept one pending connection, non-blocking; kInvalidSocket when none.
SocketHandle acceptOne(SocketHandle listener);

/// A self-wake pipe the I/O thread polls alongside the sockets: wake()
/// makes the read end readable, drain() consumes pending wakes. pipe(2) on
/// POSIX; a loopback socket pair on Windows.
class WakePipe {
public:
    WakePipe();
    ~WakePipe();
    WakePipe(const WakePipe&) = delete;
    WakePipe& operator=(const WakePipe&) = delete;

    [[nodiscard]] bool ok() const noexcept { return readEnd_ != kInvalidSocket; }
    [[nodiscard]] SocketHandle readHandle() const noexcept { return readEnd_; }
    void wake();
    void drain();

private:
    SocketHandle readEnd_ = kInvalidSocket;
    SocketHandle writeEnd_ = kInvalidSocket;
#if !defined(_WIN32)
    bool isPipe_ = false;
#endif
};

}  // namespace imos::transports::tcp28::detail
