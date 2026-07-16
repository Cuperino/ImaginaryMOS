// SPDX-FileCopyrightText: 2026 Javier O. Cordero Pérez
// SPDX-License-Identifier: Apache-2.0
//
// Profile 1 — Basic Object Based Workflow (plan §3.2 row 1, §4.6, milestone
// M3). A MOS device describes its media objects to the NCS and keeps that
// description in sync: the device pushes <mosObj> as objects are created,
// changed, or deleted, and the NCS can ask for one object (<mosReqObj>) or the
// whole catalog (<mosReqAll>, batched into one <mosListAll> or paced as
// individual <mosObj> messages). All Profile 1 traffic rides the mom channel.
//
// This module holds the role-agnostic services over the shared sans-I/O core:
// Profile1Responder is the device (imos) side and ObjectDirectory is the NCS
// (incs) side. Objects move as verbatim <mosObj> subtrees so
// mosExternalMetadata/mosPayload and vendor tags pass through untouched; typed
// access goes through codec::gen::MosObj.

#pragma once

#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <imos/codec/message.h>
#include <imos/core_export.h>
#include <imos/error.h>
#include <imos/session/peer_session.h>

namespace imos::profiles {

// ---- shared helpers ---------------------------------------------------

/// Build a mosAck. `status` is "ACK" or "NACK" (spec §3.1.1); the objID/objRev
/// echo the object being acknowledged (empty for a bare catalog ack) and
/// `statusDescription` carries the verbose error text on a NACK. The envelope
/// (mosID/ncsID/messageID) is left to the session; echo the request's
/// messageID before enqueuing so the peer correlates the reply.
[[nodiscard]] IMOS_CORE_API codec::Message buildMosAck(std::string_view objID,
                                                       std::string_view objRev,
                                                       std::string_view status,
                                                       std::string_view statusDescription);

/// The <objID> of a <mosObj> message, or empty when absent.
[[nodiscard]] IMOS_CORE_API std::string mosObjID(const codec::Message& mosObj);

// ---- device (imos) side ----------------------------------------------

/// Sans-I/O object provider implemented by the host device. Objects are
/// exchanged as verbatim <mosObj> subtrees (full codec::Message values, i.e.
/// a <mos> wrapper around one <mosObj>) so payloads round-trip byte-clean.
class IObjectSource {
public:
    /// The object with this objID, or nullopt if the device no longer holds
    /// it (e.g. it vanished mid-sync).
    virtual std::optional<codec::Message> object(std::string_view objID) = 0;
    /// A stable snapshot of every objID the device currently exposes, used to
    /// drive mosReqAll in both batch and paced modes.
    virtual std::vector<std::string> objectIDs() = 0;
    virtual ~IObjectSource() = default;
};

/// Device-side Profile 1 service: answers the NCS's mosReqObj / mosReqAll and
/// originates unsolicited mosObj pushes. Stays sans-I/O — a paced mosReqAll
/// records a job against the injected clock and releases one mosObj per poll()
/// when due; nextDeadline() feeds host scheduling like PeerSession's.
class IMOS_CORE_API Profile1Responder {
public:
    Profile1Responder(IObjectSource* source, PeerSession* session, IClock* clock);

    /// Set (or replace) the object source. Until one is attached, handle()
    /// leaves every event untouched.
    void setSource(IObjectSource* source) noexcept { source_ = source; }

    /// Consume one session event. Returns true when it was a mosReqObj or
    /// mosReqAll this responder answered; false otherwise (leave it for the
    /// application). AckReceived events for the device's own pushes are the
    /// application's to observe and are never consumed here.
    bool handle(const SessionEvent& event);

    /// Release the next paced mosReqAll object(s) that have come due. Call
    /// from the host loop alongside PeerSession::poll().
    void poll();

    /// Earliest instant poll() has a paced release due, for host scheduling.
    [[nodiscard]] std::optional<MonotonicTime> nextDeadline() const;

    /// Originate an unsolicited object push (create/update/delete, keyed by
    /// the object's own <status>). `mosObj` must be a mosObj message; the
    /// session assigns its messageID and awaits the NCS's mosAck.
    Result<Seq> announceObject(codec::Message mosObj);

private:
    /// A paced (pause > 0) mosReqAll in progress: the frozen snapshot plus the
    /// next-release instant on the injected clock.
    struct PacedSync {
        std::vector<std::string> ids;
        std::size_t index = 0;
        std::int64_t pauseMillis = 0;
        MonotonicTime nextRelease{};
        Channel channel = Channel::Mom;
        bool active = false;
    };

    void handleReqObj(const SessionEvent& event);
    void handleReqAll(const SessionEvent& event);
    void sendBatch(Channel channel);
    void releaseNextPaced();

    IObjectSource* source_;
    PeerSession* session_;
    IClock* clock_;
    PacedSync paced_;
};

// ---- NCS (incs) side --------------------------------------------------

enum class DirectoryEventType : std::uint8_t {
    ObjectUpdated,     ///< a mosObj was ingested (push, reqObj hit, or sync)
    ObjectDeleted,     ///< a mosObj with status "DELETED" removed a mirror entry
    RequestFailed,     ///< a NACK mosAck answered our mosReqObj (objID unknown)
    SyncAcknowledged,  ///< our mosReqAll was acked; the catalog stream follows
};

struct DirectoryEvent {
    DirectoryEventType type = DirectoryEventType::ObjectUpdated;
    std::string objID;
    std::string statusDescription;  ///< verbose reason on RequestFailed
};

/// NCS-side mirror of one peer's object catalog (plan §4.6 "object directory",
/// Profile 1 slice: ingest + requests; Profile 3 adds search/create/action).
/// Ingest rules: a mosObj arriving as MessageReceived is an unsolicited push —
/// ingest and reply mosAck; a mosObj (or NACK mosAck) arriving as AckReceived
/// is the response to our mosReqObj — ingest only, no re-ack; a mosListAll
/// carries the whole catalog — ingest all and ack. During a paced sync the
/// directory just sees ordinary pushes.
class IMOS_CORE_API ObjectDirectory {
public:
    ObjectDirectory(PeerSession* session, IClock* clock);

    /// Consume one session event; returns true when it was Profile 1 traffic
    /// this directory ingested/answered.
    bool handle(const SessionEvent& event);

    Result<Seq> requestObject(std::string_view objID);  ///< mosReqObj
    Result<Seq> requestAll(int pauseSeconds);           ///< mosReqAll

    /// The mirrored object (a <mos> wrapper around one <mosObj>), or nullptr.
    [[nodiscard]] const codec::Message* object(std::string_view objID) const;
    [[nodiscard]] std::vector<std::string> objectIDs() const;
    [[nodiscard]] std::size_t size() const noexcept { return objects_.size(); }

    /// High-level directory changes for the application (ingest/delete/miss/
    /// sync-start), in arrival order.
    [[nodiscard]] std::optional<DirectoryEvent> nextEvent();

private:
    enum class RequestKind : std::uint8_t { Object, All };
    struct Outstanding {
        RequestKind kind = RequestKind::Object;
        std::string objID;
    };

    void ingest(codec::Message mosObj);
    /// Reply mosAck to a received push/list, echoing its messageID (and the
    /// object's objID/objRev when it is a mosObj).
    void ackPush(Channel channel, const codec::Message& received);
    void pushEvent(DirectoryEvent event) { events_.push_back(std::move(event)); }

    PeerSession* session_;
    /// Reserved for future timer-based directory logic (resync backoff, sync
    /// deadlines); the Profile 1 slice needs no clock of its own.
    [[maybe_unused]] IClock* clock_;
    std::unordered_map<std::string, codec::Message> objects_;
    std::unordered_map<Seq, Outstanding> outstanding_;
    std::deque<DirectoryEvent> events_;
};

}  // namespace imos::profiles
