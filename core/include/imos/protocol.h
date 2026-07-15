// SPDX-FileCopyrightText: 2026 Javier O. Cordero Pérez
// SPDX-License-Identifier: Apache-2.0
//
// Protocol-wide vocabulary shared by the codec, session, and transports:
// the channel scheme (plan §3.1/§4.4) and message-envelope shapes.

#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

namespace imos {

/// Logical channel of a peer relationship, mapping 1:1 onto the legacy port
/// scheme: mom = lower port 10540 (media-object metadata), ro = upper port
/// 10541 (running orders), aux = query port 10542 (search family).
enum class Channel : std::uint8_t { Mom = 0, Ro = 1, Aux = 2 };

inline constexpr std::size_t kChannelCount = 3;

constexpr std::string_view channelName(Channel channel) noexcept {
    switch (channel) {
        case Channel::Mom:
            return "mom";
        case Channel::Ro:
            return "ro";
        case Channel::Aux:
            return "aux";
    }
    return "";
}

constexpr std::uint16_t channelLegacyPort(Channel channel) noexcept {
    switch (channel) {
        case Channel::Mom:
            return 10540;
        case Channel::Ro:
            return 10541;
        case Channel::Aux:
            return 10542;
    }
    return 0;
}

constexpr std::optional<Channel> channelFromName(std::string_view name) noexcept {
    if (name == "mom") {
        return Channel::Mom;
    }
    if (name == "ro") {
        return Channel::Ro;
    }
    if (name == "aux") {
        return Channel::Aux;
    }
    return std::nullopt;
}

/// Which channel a message type belongs on, from the spec's per-message
/// "Port" annotations. Profile 0 messages are valid on any channel; the
/// plug-in family is host↔plug-in traffic, not network traffic.
enum class ChannelAffinity : std::uint8_t { Mom, Ro, Aux, AnyChannel, NotNetwork };

/// The single channel implied by an affinity, if there is exactly one.
constexpr std::optional<Channel> channelFor(ChannelAffinity affinity) noexcept {
    switch (affinity) {
        case ChannelAffinity::Mom:
            return Channel::Mom;
        case ChannelAffinity::Ro:
            return Channel::Ro;
        case ChannelAffinity::Aux:
            return Channel::Aux;
        case ChannelAffinity::AnyChannel:
        case ChannelAffinity::NotNetwork:
            break;
    }
    return std::nullopt;
}

/// Envelope shape inside <mos>: Full = mosID, ncsID, messageID; IdsOnly =
/// mosID, ncsID (keepAlive carries no messageID per spec §4.1.1); None =
/// bare wrapper (plug-in host messages).
enum class EnvelopeKind : std::uint8_t { Full, IdsOnly, None };

}  // namespace imos
