// SPDX-FileCopyrightText: 2026 Javier O. Cordero Pérez
// SPDX-License-Identifier: Apache-2.0
//
// Profile 2 — Basic Running Order / Content List Workflow (plan §3.2 row 2,
// §4.6, milestone M3). The NCS builds and mutates running orders on the
// device with the roConstruction family; the device acknowledges each
// message with roAck and reports element status back with roElementStat
// (bidirectional since 2.8.5). All Profile 2 traffic rides the ro channel.
//
// This module holds the role-agnostic services over the shared sans-I/O
// core: Profile2Responder is the device (imos) side — it routes the
// construction family into a RunningOrderModel (or a raw IRunningOrderSink
// for hosts that opt out), answers roAck, and owns the §3.3 resync driver —
// and Profile2NcsResponder is the NCS (incs) side, answering the device's
// roReq from the authoring store and ingesting roElementStat.

#pragma once

#include <cstdint>
#include <deque>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <imos/codec/message.h>
#include <imos/core_export.h>
#include <imos/error.h>
#include <imos/profiles/profile1.h>
#include <imos/profiles/running_order_model.h>
#include <imos/session/peer_session.h>

namespace imos::profiles {

// ---- shared helpers ---------------------------------------------------

/// One (storyID, itemID, objID, itemChannel?, status) tuple from a roAck's
/// per-element status list (spec §3.4.1).
struct ElementStatus {
    std::string storyID;
    std::string itemID;
    std::string objID;
    std::string itemChannel;
    std::string status;
};

/// The content of one roElementStat (spec §3.7.1), used both to originate a
/// status report and to surface an ingested one. `time` is stamped from the
/// injected clock on send and carried verbatim on receive.
struct ElementStatusReport {
    RoElement element = RoElement::RunningOrder;
    std::string roID;
    std::string storyID;      ///< STORY/ITEM levels
    std::string itemID;       ///< ITEM level
    std::string objID;        ///< optional
    std::string itemChannel;  ///< optional
    std::string status;
    std::string time;
};

/// Build a roAck. `roStatus` is "OK" on success or "NACK: <detail>" on
/// failure (the spec's roStatus is free text; the "NACK" prefix is the
/// documented failure convention, per §3.5.1's "roAck is sent with the
/// status value of NACK"). Tuples in `statuses` are appended in spec order.
/// Echo the request's messageID before enqueuing the reply.
[[nodiscard]] IMOS_CORE_API codec::Message buildRoAck(std::string_view roID,
                                                      std::string_view roStatus,
                                                      std::span<const ElementStatus> statuses = {});

/// True when a roAck signals failure (roStatus is or starts with "NACK").
[[nodiscard]] IMOS_CORE_API bool roAckIsNack(const codec::Message& roAck);

/// The roAck's <roStatus> text ("" when absent).
[[nodiscard]] IMOS_CORE_API std::string roAckStatusText(const codec::Message& roAck);

/// The roAck's per-element tuples, walked sequentially (a new storyID opens
/// a new tuple, so the optional itemChannel never misaligns the lists).
[[nodiscard]] IMOS_CORE_API std::vector<ElementStatus> roAckStatuses(const codec::Message& roAck);

[[nodiscard]] IMOS_CORE_API codec::Message buildRoElementStat(const ElementStatusReport& report);
[[nodiscard]] IMOS_CORE_API Result<ElementStatusReport> parseRoElementStat(
    const codec::Message& message);

// ---- device (imos) side ----------------------------------------------

/// Raw-mode alternative to the RunningOrderModel (plan §4.6): one callback
/// per construction message. Return success to roAck "OK", or an error whose
/// message becomes the NACK detail. Hosts using the sink get no shadow
/// model and no automatic resync driver — they own both.
class IRunningOrderSink {
public:
    virtual Result<void> onRoCreate(const codec::Message& message) = 0;
    virtual Result<void> onRoReplace(const codec::Message& message) = 0;
    virtual Result<void> onRoMetadataReplace(const codec::Message& message) = 0;
    virtual Result<void> onRoDelete(const codec::Message& message) = 0;
    virtual Result<void> onRoElementAction(const codec::Message& message) = 0;
    virtual Result<void> onRoReadyToAir(const codec::Message& message) = 0;
    virtual Result<void> onRoElementStat(const codec::Message& message) = 0;
    virtual ~IRunningOrderSink() = default;
};

/// Device-side Profile 2 service. Default mode routes the construction
/// family into an owned RunningOrderModel and answers roAck from the
/// model's outcome. On an unknown roID/storyID/itemID it runs the §3.3
/// resync heuristic: NACK the message, surface a single ResyncNeeded
/// session event (not one per confused message), issue roReq for the RO,
/// and rebuild wholesale from the roList reply — or drop the RO when the
/// NCS answers NACK (the RO no longer exists there).
class IMOS_CORE_API Profile2Responder {
public:
    /// What to do when a construction message references items whose objID
    /// the attached object source cannot resolve (spec §3.4.1 example):
    /// Accept applies the message and reports the per-item statuses in an
    /// "OK"-family roAck; Reject NACKs it with the same tuples.
    enum class UnknownObjectPolicy : std::uint8_t { Accept, Reject };

    Profile2Responder(PeerSession* session, IClock* clock);

    /// Switch to raw mode: construction messages go to `sink`, the model is
    /// bypassed, and the resync driver is disabled. Pass nullptr to return
    /// to model mode.
    void setSink(IRunningOrderSink* sink) noexcept { sink_ = sink; }

    /// Attach the device's Profile 1 object source so item references in
    /// construction messages are validated against the media store.
    void setObjectSource(IObjectSource* source) noexcept { objects_ = source; }
    void setUnknownObjectPolicy(UnknownObjectPolicy policy) noexcept { objectPolicy_ = policy; }

    [[nodiscard]] RunningOrderModel& model() noexcept { return model_; }
    [[nodiscard]] const RunningOrderModel& model() const noexcept { return model_; }

    /// Consume one session event. Construction messages and resync replies
    /// return true; everything else (including the ResyncNeeded event this
    /// responder itself posts) is left for the application.
    bool handle(const SessionEvent& event);

    /// Originate a device-side status report (PLAY/STOP/READY…). Validates
    /// against and records into the model (unless in sink mode), stamps
    /// <time> from the clock, and awaits the NCS's roAck.
    Result<Seq> sendElementStat(const ElementStatusReport& report);

    /// Ingested NCS-originated roElementStat reports, in arrival order.
    [[nodiscard]] std::optional<ElementStatusReport> nextStatus();

    /// True while a roReq/roList rebuild for this RO is outstanding.
    [[nodiscard]] bool resyncing(std::string_view roID) const;

private:
    void handleConstruction(const SessionEvent& event);
    Result<void> dispatchToSink(const codec::Message& message);
    void startResync(const std::string& roID, Channel channel, std::string_view reason);
    void completeResync(const SessionEvent& event);
    /// Item objID validation tuples for roCreate/roReplace/roElementAction
    /// payloads; returns the first unknown objID ("" when all resolve).
    std::string collectObjectStatuses(const codec::Message& message,
                                      std::vector<ElementStatus>& statuses);

    PeerSession* session_;
    IClock* clock_;
    RunningOrderModel model_;
    IRunningOrderSink* sink_ = nullptr;
    IObjectSource* objects_ = nullptr;
    UnknownObjectPolicy objectPolicy_ = UnknownObjectPolicy::Accept;
    std::unordered_map<Seq, std::string> pendingResync_;  ///< roReq seq → roID
    std::set<std::string> resyncing_;
    std::deque<ElementStatusReport> statusEvents_;
};

// ---- NCS (incs) side --------------------------------------------------

/// The NCS's authoritative running-order store, implemented by the
/// authoring layer (incs::RunningOrderAuthor). Answering a roReq means the
/// device is rebuilding from scratch: return the complete RO as a roList
/// message (body children in roList order) and treat the returned snapshot
/// as the device's new known state, or nullopt when the RO does not exist
/// (the responder then answers a NACK roAck).
class IRunningOrderSource {
public:
    virtual std::optional<codec::Message> runningOrder(std::string_view roID) = 0;
    virtual ~IRunningOrderSource() = default;
};

/// NCS-side Profile 2 service: answers the device's roReq from the
/// authoring store with a roList (terminal reply, echoing the request's
/// messageID) or a NACK roAck, and ingests device-originated roElementStat
/// (ack + surfaced through nextStatus()).
class IMOS_CORE_API Profile2NcsResponder {
public:
    Profile2NcsResponder(PeerSession* session, IClock* clock);

    void setSource(IRunningOrderSource* source) noexcept { source_ = source; }

    bool handle(const SessionEvent& event);

    /// Originate an NCS-side status update (2.8.5+ semantics; flows into the
    /// device's model). Stamps <time> from the clock; awaits the roAck.
    Result<Seq> sendElementStat(const ElementStatusReport& report);

    [[nodiscard]] std::optional<ElementStatusReport> nextStatus();

private:
    PeerSession* session_;
    IClock* clock_;
    IRunningOrderSource* source_ = nullptr;
    std::deque<ElementStatusReport> statusEvents_;
};

}  // namespace imos::profiles
