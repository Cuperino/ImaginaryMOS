// SPDX-FileCopyrightText: 2026 Javier O. Cordero Pérez
// SPDX-License-Identifier: Apache-2.0
//
// Profile 7 — MOS RO / Content List Modification, NCS side. The NCS validates
// roReqStoryAction from the MOS, checks capabilities (P7 advertised in
// listMachInfo), consults the lease table, mutates RunningOrderAuthor's
// desired state, commits the result (which flows to all devices via P2/P4),
// and answers roAck. Lease policy is pluggable; the default single-holder
// implementation exposes a TTL-driven acquire/release API. Fully sans-I/O:
// clock time is passed in; the NCS-side layer never reads it.

#pragma once

#include <deque>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include <imos/error.h>
#include <imos/profiles/profile0.h>
#include <imos/profiles/profile7.h>
#include <imos/session/peer_session.h>
#include <imos/time_format.h>
#include <incs/incs_export.h>

namespace incs {

class RunningOrderAuthor;

/// Pluggable NCS lease policy. Sans-I/O: `now` is passed in per call, nothing
/// reads a clock. Default: single-holder, fixed TTL; acquire() covers both
/// first-grant and same-holder renewal.
class IMOS_INCS_API ILeasePolicy {
public:
    virtual ~ILeasePolicy() = default;
    /// Try to acquire or renew a lease. Returns error if held by a different
    /// holder or if the lease expired (requires release before re-acquire by a
    /// different holder).
    virtual imos::Result<void> acquire(std::string_view roID, std::string_view storyID,
                                       std::string_view holder, int durationSeconds,
                                       imos::MonotonicTime now) = 0;
    /// Release a lease (no-op if not held or held by a different holder).
    virtual void release(std::string_view roID, std::string_view storyID,
                         std::string_view holder) = 0;
};

/// Default NCS lease policy: single holder per story, fixed TTL from
/// durationSeconds, no renewal interval — acquire resets the expiry.
class IMOS_INCS_API SingleHolderLeasePolicy : public ILeasePolicy {
public:
    imos::Result<void> acquire(std::string_view roID, std::string_view storyID,
                               std::string_view holder, int durationSeconds,
                               imos::MonotonicTime now) override;
    void release(std::string_view roID, std::string_view storyID, std::string_view holder) override;

private:
    struct LeaseEntry {
        std::string holder;
        imos::MonotonicTime expiry;
    };
    std::map<std::pair<std::string, std::string>, LeaseEntry> leases_;  // {roID, storyID} → entry
};

/// One resolved roReqStoryAction, for simulator/audit assertions.
struct StoryActionAudit {
    std::string roID, storyID, username, detail;
    imos::profiles::StoryAction operation;
    bool accepted = false;
};

/// NCS-side Profile 7 authority. Parses roReqStoryAction, checks the P7
/// capability flag (via peer0's PeerMachineInfo) under CapabilityPolicy,
/// consults the lease table, mutates RunningOrderAuthor's desired state,
/// commits (P2 echo) and/or sendStory()s (P4 echo), answers roAck.
class IMOS_INCS_API Profile7NcsResponder {
public:
    enum class CapabilityPolicy : std::uint8_t { Strict, Lenient };  // §4.1 duality

    Profile7NcsResponder(RunningOrderAuthor* author, ILeasePolicy* leases,
                         const imos::profiles::Profile0Responder* peer0, imos::PeerSession* session,
                         imos::IClock* clock);

    void setCapabilityPolicy(CapabilityPolicy policy) noexcept;
    [[nodiscard]] CapabilityPolicy capabilityPolicy() const noexcept { return capabilityPolicy_; }

    /// Change the lease policy (default: SingleHolderLeasePolicy). The responder
    /// holds a pointer; the host is responsible for lifetime management.
    void setLeasePolicy(ILeasePolicy* policy) noexcept { leases_ = policy; }

    /// Consume one session event: inbound MessageReceived roReqStoryAction.
    bool handle(const imos::SessionEvent& event);

    /// Next resolved roReqStoryAction, for audit/simulator assertions.
    [[nodiscard]] std::optional<StoryActionAudit> nextAudit();

private:
    /// Inserts every story of a NEW (spec §3.9.1: "one or more stories to be
    /// inserted"), in order, before the request's target; returns the first
    /// story's ID — the one the roAck carries in roStatus.
    imos::Result<std::string> applyNew(const imos::profiles::StoryActionRequest& req,
                                       const std::vector<imos::codec::XmlNode>& stories);
    imos::Result<void> applyUpdate(const imos::profiles::StoryActionRequest& req);
    imos::Result<void> applyDelete(const imos::profiles::StoryActionRequest& req);
    imos::Result<void> applyMove(const imos::profiles::StoryActionRequest& req);

    /// Generate a unique storyID for NEW operations when the device sends an
    /// empty one. Format: "P7-<n>", collision-checked against desired RO.
    std::string nextGeneratedStoryID(std::string_view roID);

    RunningOrderAuthor* author_;
    ILeasePolicy* leases_;
    const imos::profiles::Profile0Responder* peer0_;
    imos::PeerSession* session_;
    imos::IClock* clock_;
    CapabilityPolicy capabilityPolicy_ = CapabilityPolicy::Lenient;
    std::size_t nextGeneratedStoryIdCounter_ = 0;
    std::deque<StoryActionAudit> audits_;
};

}  // namespace incs
