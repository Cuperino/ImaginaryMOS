// SPDX-FileCopyrightText: 2026 Javier O. Cordero Pérez
// SPDX-License-Identifier: Apache-2.0
//
// incs::Ncs — the Newsroom Computer System (NCS) role facade (plan §4.6). The
// mirror of imos::Device over the same sans-I/O core: it owns one PeerSession
// for the configured peer plus the Profile 0 machine-info responder and the
// Profile 1 ObjectDirectory (the NCS's mirror of the peer's object catalog),
// and dispatches inbound session events through them. Powers the NRCS
// simulator used for M8 interop. Driven pull-mode; no library-owned threads.
//
// M3 grows the former version-stub into real code; the RO authoring API and
// object search land in later milestones without changing this shape.

#pragma once

#include <deque>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <imos/codec/message.h>
#include <imos/profiles/profile0.h>
#include <imos/profiles/profile1.h>
#include <imos/session/peer_session.h>
#include <incs/incs_export.h>

namespace incs {

/// NCS configuration: the session parameters for the one peer relationship
/// plus the machine facts this NCS declares (deviceType is forced to NCS).
struct NcsConfig {
    imos::SessionConfig session;
    imos::profiles::MachineInfo machineInfo;
};

class IMOS_INCS_API Ncs {
public:
    Ncs(NcsConfig config, imos::PeerSession::Dependencies deps);

    Ncs(const Ncs&) = delete;
    Ncs& operator=(const Ncs&) = delete;
    Ncs(Ncs&&) = delete;
    Ncs& operator=(Ncs&&) = delete;

    // Host glue: connectivity changes and inbound frames pushed in.
    void onChannelConnected(imos::Channel channel);
    void onChannelDisconnected(imos::Channel channel);
    void onFrameReceived(imos::Channel channel, std::span<const std::uint8_t> frame);

    void poll();
    [[nodiscard]] std::optional<imos::MonotonicTime> nextDeadline() const;

    // Profile 1 object directory.
    imos::Result<imos::Seq> requestObject(std::string_view objID);  ///< mosReqObj
    imos::Result<imos::Seq> requestAll(int pauseSeconds);           ///< mosReqAll
    [[nodiscard]] const imos::codec::Message* object(std::string_view objID) const;
    [[nodiscard]] std::vector<std::string> objectIDs() const;

    /// Next high-level directory change (ingest/delete/miss/sync-start).
    [[nodiscard]] std::optional<imos::profiles::DirectoryEvent> nextEvent();

    /// Direct session access for host transport glue (e.g. pump()).
    [[nodiscard]] imos::PeerSession& session() noexcept { return session_; }

private:
    void dispatch();

    imos::PeerSession session_;
    imos::profiles::Profile0Responder profile0_;
    imos::profiles::ObjectDirectory directory_;
    std::deque<imos::SessionEvent> appEvents_;
};

}  // namespace incs
