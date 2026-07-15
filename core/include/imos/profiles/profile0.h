// SPDX-FileCopyrightText: 2026 Javier O. Cordero Pérez
// SPDX-License-Identifier: Apache-2.0
//
// Profile 0 — Basic Communication (plan §3.2, milestone M2). keepAlive and
// heartbeat are session machinery (PeerSession handles cadence, echo, and
// loop avoidance); this module covers the machine-information exchange:
// composing listMachInfo from a declarative MachineInfo, answering inbound
// reqMachInfo, and parsing a peer's listMachInfo — including the
// supported-profiles list that version/dialect negotiation reads. Both roles
// (imos and incs) use the same service; only the deviceType differs.

#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>

#include <imos/codec/message.h>
#include <imos/core_export.h>
#include <imos/error.h>
#include <imos/session/peer_session.h>

namespace imos::profiles {

inline constexpr int kProfileCount = 8;

enum class DeviceType : std::uint8_t { Mos, Ncs };

[[nodiscard]] IMOS_CORE_API std::string_view deviceTypeName(DeviceType type) noexcept;

/// The static facts a device declares about itself in listMachInfo
/// (spec §4.1.4): manufacturer/model/revisions, identity, and which MOS
/// profiles it implements.
struct MachineInfo {
    std::string manufacturer;
    std::string model;
    std::string hwRev;
    std::string swRev;
    std::string dom;  ///< date of manufacture
    std::string serialNumber;
    std::string id;  ///< machine ID; typically the mosID or ncsID
    DeviceType deviceType = DeviceType::Mos;
    std::array<bool, kProfileCount> profiles{};  ///< profiles[n] → Profile n
    std::string mosRev = "4.0";                  ///< MOS Protocol revision claimed
    /// Operation start time for <opTime>, if the host tracks one.
    std::optional<WallClockTime> opTime;
};

/// What a peer declared about itself, parsed from its listMachInfo.
struct PeerMachineInfo {
    MachineInfo info;
    std::string time;    ///< the message's <time>, verbatim
    std::string opTime;  ///< verbatim <opTime> (may be empty)
    bool hasSupportedProfiles = false;
};

/// Build a listMachInfo message from `info`. <time> is stamped from `now`;
/// <opTime> is emitted when info.opTime is set. The envelope (mosID/ncsID/
/// messageID) is left to the session, which echoes the request's messageID
/// when the message answers a reqMachInfo.
[[nodiscard]] IMOS_CORE_API codec::Message buildListMachInfo(const MachineInfo& info,
                                                             WallClockTime now);

/// Parse a peer's listMachInfo (lenient: absent fields stay empty; an
/// unknown deviceType or a missing supportedProfiles block is tolerated and
/// reflected in the result rather than an error). Fails only if `message`
/// is not a listMachInfo.
IMOS_CORE_API Result<PeerMachineInfo> parseListMachInfo(const codec::Message& message);

/// Profile 0 responder: give it every MessageReceived event; when the event
/// is a reqMachInfo it queues the listMachInfo answer (echoing the request's
/// messageID) on the same channel and returns true, meaning the event is
/// consumed. All other events return false untouched.
class IMOS_CORE_API Profile0Responder {
public:
    Profile0Responder(MachineInfo info, PeerSession* session, IClock* clock);

    bool handle(const SessionEvent& event);

    [[nodiscard]] const MachineInfo& machineInfo() const noexcept { return info_; }
    void setMachineInfo(MachineInfo info) { info_ = std::move(info); }

private:
    MachineInfo info_;
    PeerSession* session_;
    IClock* clock_;
};

}  // namespace imos::profiles
