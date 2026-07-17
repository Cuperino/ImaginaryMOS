// SPDX-FileCopyrightText: 2026 Javier O. Cordero Pérez
// SPDX-License-Identifier: Apache-2.0
//
// imos::Device — the Media Object Server (device) role facade (plan §4.6,
// §8.1). A thin, thread-free composition over the shared sans-I/O core: it
// owns one PeerSession for the configured peer plus the profile responders
// provided so far (Profile 0 machine-info, Profile 1 object workflow,
// Profile 2 running orders with the shadow RunningOrderModel and §3.3
// resync driver, Profile 4 RO enumeration/hydration and story bodies) and
// dispatches inbound session events through them. The host drives it
// pull-mode: push connectivity/frames in, call poll(), drain nextEvent().
// No library-owned threads (plan §8.1).
//
// M3 grew the former version-stub into real code; M4 added Profile 4; later
// milestones thicken it (search, control) without changing this shape.

#pragma once

#include <deque>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <imos/fqid.h>
#include <imos/imos_export.h>
#include <imos/profiles/profile0.h>
#include <imos/profiles/profile1.h>
#include <imos/profiles/profile2.h>
#include <imos/profiles/profile4.h>
#include <imos/profiles/profile5.h>
#include <imos/profiles/profile6.h>
#include <imos/profiles/profile7.h>
#include <imos/profiles/running_order_model.h>
#include <imos/session/peer_session.h>

namespace imos {

/// Device configuration: the session parameters for the one peer relationship
/// plus the machine facts this device declares (deviceType is forced to MOS).
struct DeviceConfig {
    SessionConfig session;
    profiles::MachineInfo machineInfo;
    /// Hydrate on connect (Profile 4): when the ro channel comes up, ask the
    /// NCS for every MOS-active RO (roReqAll) and fetch each one into the
    /// shadow model, so a cold-started device converges without host code.
    bool hydrateRunningOrdersOnConnect = false;
    /// Profile 6 (MOS Redirection): FQID identity enforcement and foreign-
    /// reference classification, relative to session.mosID. All-off by
    /// default — nothing changes for non-claiming devices. Gate enabled
    /// configs through Device::validateConfig() at bring-up; when enabled
    /// and valid, listMachInfo advertises Profile 6.
    profiles::Profile6Config profile6{};
};

class IMOS_IMOS_API Device {
public:
    /// The bring-up gate (Profile 6 identity enforcement): fails with a
    /// clear diagnostic when config.profile6 is enabled but session.mosID
    /// is not a Fully Qualified MOS ID. Check before constructing —
    /// misconfiguration should be caught at bring-up, not on air. A Device
    /// built from a config this rejects still runs, but with Profile 6
    /// disabled and a ProtocolError session event carrying the diagnostic.
    [[nodiscard]] static Result<void> validateConfig(const DeviceConfig& config);

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

    /// Register the host's story consumer for Profile 4 (roStorySend).
    /// Stories are acked/NACKed from the sink's outcome; without a sink they
    /// are still acked, stored, and queryable via story().
    void attach(profiles::IStorySink& sink);

    /// Register the host's control consumer for Profile 5 (roCtrl/roItemCue).
    /// onCtrl's outcome drives the roAck; cues are always acked. Without a
    /// sink, requests and cues queue for nextCtrlRequest()/nextItemCue().
    void attach(profiles::IControlSink& sink);

    /// What to do when construction messages reference objIDs the attached
    /// object source cannot resolve (default: accept with statuses).
    void setUnknownObjectPolicy(profiles::Profile2Responder::UnknownObjectPolicy policy);

    /// The Profile 2 shadow running-order store (queries: playlists, story/
    /// item lookup, readiness, element status).
    [[nodiscard]] profiles::RunningOrderModel& runningOrderModel() noexcept;
    [[nodiscard]] const profiles::RunningOrderModel& runningOrderModel() const noexcept;

    // Profile 4: RO enumeration, hydration, and the story store.

    /// Ask the NCS for every MOS-active RO (roReqAll → roListAll header
    /// index); with `hydrate` each listed RO is then fetched into the model.
    Result<Seq> requestAllRunningOrders(bool hydrate = true);
    /// roIDs from the last roListAll, and one verbatim <ro> header subtree.
    [[nodiscard]] std::vector<std::string> runningOrderHeaderIDs() const;
    [[nodiscard]] const codec::XmlNode* runningOrderHeader(std::string_view roID) const;
    /// The last accepted roStorySend for a story (verbatim), or nullptr.
    [[nodiscard]] const codec::Message* story(std::string_view roID,
                                              std::string_view storyID) const;
    [[nodiscard]] std::vector<std::string> storyIDs(std::string_view roID) const;

    // Profile 6: MOS Redirection (no messages — FQID classification).

    /// Where an object referenced by `mosID` lives relative to this device
    /// (Locality::NotFqid whenever Profile 6 is not enabled and valid).
    /// Ingested foreign references also surface as ForeignObjectReferenced
    /// events and StoryView::references when the config flags them.
    [[nodiscard]] Locality classifyMosID(std::string_view mosID) const;

    // Profile 5: item control (roCtrl in, roItemCue both ways).

    /// Originate a cue (prompter style; spec §3.7.2). Pass `addresseeNcsID`
    /// to address the item's parent MOS — its name then rides the envelope's
    /// <ncsID> slot per the spec's counterintuitive assignment; empty uses
    /// the session's configured ncsID. The peer's roAck surfaces via
    /// nextEvent() as an ordinary AckReceived.
    Result<Seq> sendItemCue(const profiles::ItemCue& cue, std::string_view addresseeNcsID = {});

    /// Sink-less drains: resolved control requests and cues, in arrival
    /// order (already acked "OK"). Empty when an IControlSink is attached —
    /// the sink then sees them instead.
    [[nodiscard]] std::optional<profiles::CtrlRequest> nextCtrlRequest();
    [[nodiscard]] std::optional<profiles::ItemCue> nextItemCue();

    // Profile 7: MOS story action requests.

    /// Request the NCS to create/update/move/delete a story (roReqStoryAction).
    /// Awaits roAck via nextStoryActionResult().
    Result<Seq> requestStoryAction(const profiles::StoryActionRequest& request);
    /// Next resolved roReqStoryAction (outcome from roAck), in order.
    [[nodiscard]] std::optional<profiles::StoryActionResult> nextStoryActionResult();

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

    /// Request the peer's machine info (Profile 0). The reply is captured in
    /// peerMachineInfo() when it arrives. The spec (§4.1.3) homes reqMachInfo
    /// on both legacy MOS ports — mom (10540) and ro (10541) — so the caller
    /// picks the channel; Mom is the conventional default.
    Result<Seq> requestMachineInfo(Channel channel = Channel::Mom);
    /// The peer's declared machine info, parsed from its listMachInfo (or
    /// nullptr when never received or parsing failed).
    [[nodiscard]] const profiles::PeerMachineInfo* peerMachineInfo() const;

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

    // Declared (and thus initialized) before session_: the classifier is
    // armed from config.session.mosID before the session moves it away.
    profiles::Profile6Classifier profile6_;
    std::optional<Error> profile6Error_;  ///< bring-up diagnostic, posted once
    PeerSession session_;
    profiles::Profile0Responder profile0_;
    profiles::Profile1Responder profile1_;
    profiles::Profile2Responder profile2_;
    profiles::Profile4Responder profile4_;
    profiles::Profile5Responder profile5_;
    profiles::Profile7Requester profile7_;
    bool hydrateOnConnect_ = false;
    std::deque<SessionEvent> appEvents_;
};

}  // namespace imos
