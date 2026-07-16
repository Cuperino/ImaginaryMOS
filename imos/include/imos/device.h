// SPDX-FileCopyrightText: 2026 Javier O. Cordero Pérez
// SPDX-License-Identifier: Apache-2.0
//
// imos::Device — the Media Object Server (device) role facade (plan §4.6,
// §8.1). A thin, thread-free composition over the shared sans-I/O core: it
// owns one PeerSession for the configured peer plus the profile responders
// this milestone provides (Profile 0 machine-info, Profile 1 object
// workflow, Profile 2 running orders with the shadow RunningOrderModel and
// §3.3 resync driver) and dispatches inbound session events through them.
// The host drives it pull-mode: push connectivity/frames in, call poll(),
// drain nextEvent(). No library-owned threads (plan §8.1).
//
// M3 grows the former version-stub into real code; later milestones thicken
// it (search, control) without changing this shape.

#pragma once

#include <deque>
#include <optional>
#include <span>

#include <imos/imos_export.h>
#include <imos/profiles/profile0.h>
#include <imos/profiles/profile1.h>
#include <imos/profiles/profile2.h>
#include <imos/profiles/running_order_model.h>
#include <imos/session/peer_session.h>

namespace imos {

/// Device configuration: the session parameters for the one peer relationship
/// plus the machine facts this device declares (deviceType is forced to MOS).
struct DeviceConfig {
    SessionConfig session;
    profiles::MachineInfo machineInfo;
};

class IMOS_IMOS_API Device {
public:
    Device(DeviceConfig config, PeerSession::Dependencies deps);

    Device(const Device&) = delete;
    Device& operator=(const Device&) = delete;
    Device(Device&&) = delete;
    Device& operator=(Device&&) = delete;

    /// Register the host's object provider for Profile 1 (mosReqObj/mosReqAll
    /// answers and the mosReqAll snapshot). Until attached, those requests are
    /// surfaced unhandled via nextEvent(). Profile 2 also uses it to validate
    /// item references in construction messages (per-element roAck statuses).
    void attach(profiles::IObjectSource& source);

    /// Opt out of the RunningOrderModel: route construction messages to the
    /// host's raw sink instead (the host then owns RO state and resync).
    void attach(profiles::IRunningOrderSink& sink);

    /// What to do when construction messages reference objIDs the attached
    /// object source cannot resolve (default: accept with statuses).
    void setUnknownObjectPolicy(profiles::Profile2Responder::UnknownObjectPolicy policy);

    /// The Profile 2 shadow running-order store (queries: playlists, story/
    /// item lookup, readiness, element status).
    [[nodiscard]] profiles::RunningOrderModel& runningOrderModel() noexcept;
    [[nodiscard]] const profiles::RunningOrderModel& runningOrderModel() const noexcept;

    // Host glue: connectivity changes and inbound frames pushed in.
    void onChannelConnected(Channel channel);
    void onChannelDisconnected(Channel channel);
    void onFrameReceived(Channel channel, std::span<const std::uint8_t> frame);

    /// Drive session timers plus paced Profile 1 releases, then dispatch any
    /// resulting events through the responders.
    void poll();
    /// Earliest instant poll() has work due (session retries/keepAlive plus
    /// paced mosReqAll), for host scheduling.
    [[nodiscard]] std::optional<MonotonicTime> nextDeadline() const;

    /// Push an object lifecycle change to the NCS (create/update/delete, keyed
    /// by the mosObj's own <status>). Awaits the NCS's mosAck.
    Result<Seq> announceObject(codec::Message mosObj);

    /// Report an element status change (PLAY/STOP/READY…) to the NCS via
    /// roElementStat. Validated against and recorded into the model (unless
    /// a raw sink is attached); <time> is stamped from the injected clock.
    Result<Seq> reportElementStatus(const profiles::ElementStatusReport& report);

    /// Next ingested NCS-originated roElementStat (2.8.5+ semantics), in
    /// arrival order. Already acked and folded into the model.
    [[nodiscard]] std::optional<profiles::ElementStatusReport> nextElementStatus();

    /// Next event the responders did not consume (acks for the device's own
    /// pushes, faults, protocol errors), in arrival order.
    [[nodiscard]] std::optional<SessionEvent> nextEvent();

    /// Direct session access for host transport glue (e.g. pump()).
    [[nodiscard]] PeerSession& session() noexcept { return session_; }

private:
    void dispatch();

    PeerSession session_;
    profiles::Profile0Responder profile0_;
    profiles::Profile1Responder profile1_;
    profiles::Profile2Responder profile2_;
    std::deque<SessionEvent> appEvents_;
};

}  // namespace imos
