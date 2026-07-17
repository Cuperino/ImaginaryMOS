// SPDX-FileCopyrightText: 2026 Javier O. Cordero Pérez
// SPDX-License-Identifier: Apache-2.0
//
// incs::Ncs — the Newsroom Computer System (NCS) role facade (plan §4.6). The
// mirror of imos::Device over the same sans-I/O core: it owns one PeerSession
// for the configured peer plus the Profile 0 machine-info responder, the
// Profile 1 ObjectDirectory (the NCS's mirror of the peer's object catalog),
// the Profile 2 running-order authoring/diff API with its responder (roReq
// answers, roElementStat ingest), and the Profile 4 responder (roReqAll →
// headers-only roListAll) plus story origination (sendStory), and
// dispatches inbound session events through them. Powers the NRCS simulator
// used for M8 interop. Driven pull-mode; no library-owned threads.
//
// M3 grew the former version-stub into real code; M4 added Profile 4;
// object search lands in a later milestone without changing this shape.

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
#include <imos/profiles/profile2.h>
#include <imos/profiles/profile4.h>
#include <imos/session/peer_session.h>
#include <incs/incs_export.h>
#include <incs/profile7_ncs.h>
#include <incs/running_order_author.h>

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

    // Profile 0: machine info.
    imos::Result<imos::Seq> requestMachineInfo();
    [[nodiscard]] const imos::profiles::PeerMachineInfo* peerMachineInfo() const;

    // Profile 1 object directory.
    imos::Result<imos::Seq> requestObject(std::string_view objID);  ///< mosReqObj
    imos::Result<imos::Seq> requestAll(int pauseSeconds);           ///< mosReqAll
    [[nodiscard]] const imos::codec::Message* object(std::string_view objID) const;
    [[nodiscard]] std::vector<std::string> objectIDs() const;

    /// Next high-level directory change (ingest/delete/miss/sync-start).
    [[nodiscard]] std::optional<imos::profiles::DirectoryEvent> nextEvent();

    // Profile 2 running orders.

    /// The running-order authoring and diff API: edit the desired state,
    /// then author().commit(roID) to enqueue the minimal message sequence;
    /// drain author().nextEvent() for per-message roAck outcomes.
    [[nodiscard]] RunningOrderAuthor& author() noexcept { return author_; }
    [[nodiscard]] const RunningOrderAuthor& author() const noexcept { return author_; }

    /// Send one story's full script to the device (Profile 4 roStorySend).
    /// The body must have been authored first — author().setStoryBody() or
    /// setAgencyStoryBody() — and the RO becomes story-send-linked. The
    /// device's roAck resolves through author().nextEvent().
    imos::Result<imos::Seq> sendStory(std::string_view roID, std::string_view storyID);

    // Profile 7: story action requests (device-originated).

    /// Set the lease policy for roReqStoryAction handling (default: single-holder TTL).
    void setLeasePolicy(ILeasePolicy* policy) noexcept;
    /// Capability gating for roReqStoryAction: Strict NACKs peers that never
    /// advertised Profile 7 in listMachInfo; Lenient (default) accepts and
    /// records an audit entry regardless (§4.1 strict/lenient duality).
    void setStoryActionCapabilityPolicy(Profile7NcsResponder::CapabilityPolicy policy) noexcept;
    /// Next resolved roReqStoryAction, for audit/simulator assertions.
    [[nodiscard]] std::optional<StoryActionAudit> nextStoryActionAudit();

    /// Originate an NCS-side roElementStat (2.8.5+; flows into the device's
    /// model). <time> is stamped from the injected clock.
    imos::Result<imos::Seq> sendElementStat(const imos::profiles::ElementStatusReport& report);

    /// Next ingested device-originated roElementStat, in arrival order.
    [[nodiscard]] std::optional<imos::profiles::ElementStatusReport> nextElementStatus();

    /// Direct session access for host transport glue (e.g. pump()).
    [[nodiscard]] imos::PeerSession& session() noexcept { return session_; }

private:
    void dispatch();

    imos::PeerSession session_;
    imos::profiles::Profile0Responder profile0_;
    imos::profiles::ObjectDirectory directory_;
    RunningOrderAuthor author_;
    imos::profiles::Profile2NcsResponder profile2_;
    imos::profiles::Profile4NcsResponder profile4_;
    SingleHolderLeasePolicy defaultLeasePolicy_;
    Profile7NcsResponder profile7_;
    std::deque<imos::SessionEvent> appEvents_;
};

}  // namespace incs
