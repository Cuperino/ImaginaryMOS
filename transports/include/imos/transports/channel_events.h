// SPDX-FileCopyrightText: 2026 Javier O. Cordero Pérez
// SPDX-License-Identifier: Apache-2.0
//
// Where a transport's pump delivers connectivity changes and inbound frames.
// PeerSession is the usual destination (every transport's pump(PeerSession&)
// overload); a host that also wants to see the traffic - to log it, to merge
// two transports as Ws40Endpoint does, or to hand it to a facade such as
// incs::Ncs - pumps into a sink of its own instead.

#pragma once

#include <cstdint>
#include <functional>
#include <span>

#include <imos/protocol.h>

namespace imos::transports {

struct ChannelEventSink {
    std::function<void(Channel)> connected;
    std::function<void(Channel)> disconnected;
    std::function<void(Channel, std::span<const std::uint8_t>)> frame;
};

}  // namespace imos::transports
