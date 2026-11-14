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
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include <imos/codec/message.h>
#include <imos/core_export.h>
#include <imos/error.h>
#include <imos/profiles/object_query.h>
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

/// The mosReqObjAction operation vocabulary (spec §3.3.3, a closed set).
enum class ObjAction : std::uint8_t { New, Update, Delete };

[[nodiscard]] IMOS_CORE_API std::string_view objActionName(ObjAction action) noexcept;
[[nodiscard]] IMOS_CORE_API std::optional<ObjAction> objActionFromName(
    std::string_view name) noexcept;

/// A parsed create request (mosObjCreate, or mosReqObjAction NEW): the
/// scalar metadata fields, plus the verbatim message for hosts that read
/// description/mosExternalMetadata riders directly. `message` is only valid
/// for the duration of the IObjectSource callback.
struct ObjectCreateRequest {
    std::string objSlug;
    std::string objGroup;
    std::string objType;
    std::string objTB;
    std::string objDur;
    std::string createdBy;
    const codec::Message* message = nullptr;
};

/// The searchable-schema pointers a device declares (mosListSearchableSchema
/// answers; each entry is an HTTP pointer to a schema).
using SchemaList = std::vector<std::string>;

/// Sans-I/O object provider implemented by the host device. Objects are
/// exchanged as verbatim <mosObj> subtrees (full codec::Message values, i.e.
/// a <mos> wrapper around one <mosObj>) so payloads round-trip byte-clean.
///
/// The Profile 1 slice (object/objectIDs) is mandatory. The Profile 3
/// methods are optional-by-capability: defaults return Unsupported so
/// P1-only hosts keep compiling, and supportsProfile3() (which the facade
/// reads to advertise Profile 3 in listMachInfo) defaults to false — a host
/// implementing the workflow overrides it alongside the methods.
class IObjectSource {
public:
    /// The object with this objID, or nullopt if the device no longer holds
    /// it (e.g. it vanished mid-sync).
    virtual std::optional<codec::Message> object(std::string_view objID) = 0;
    /// A stable snapshot of every objID the device currently exposes, used to
    /// drive mosReqAll in both batch and paced modes.
    virtual std::vector<std::string> objectIDs() = 0;

    // ---- Profile 3 (advanced object workflow), optional-by-capability ----

    /// Whether the Profile 3 methods below are implemented; drives the
    /// Profile 3 flag in this device's listMachInfo.
    [[nodiscard]] virtual bool supportsProfile3() const noexcept { return false; }

    /// Create an object or placeholder (mosObjCreate / mosReqObjAction NEW);
    /// returns the new objID, which the responder acks in statusDescription
    /// (spec-mandated overload of that field) before pushing the full mosObj.
    virtual Result<std::string> createObject(const ObjectCreateRequest& request) {
        (void)request;
        return makeError(ErrorCode::Unsupported, "Profile 3 not supported");
    }
    /// Modify or delete an existing object (mosReqObjAction UPDATE/DELETE).
    /// `body` is the verbatim mosReqObjAction message (metadata to merge per
    /// the mosItemReplace rules; spec §3.3.3). A success is acked and
    /// followed by the corresponding mosObj push (status UPDATED/DELETED).
    virtual Result<void> actOnObject(ObjAction op, std::string_view objID,
                                     const codec::Message& body) {
        (void)op;
        (void)objID;
        (void)body;
        return makeError(ErrorCode::Unsupported, "Profile 3 not supported");
    }
    /// The schemas object metadata can be queried under
    /// (mosReqSearchableSchema → mosListSearchableSchema).
    virtual Result<SchemaList> searchableSchemas() {
        return makeError(ErrorCode::Unsupported, "Profile 3 not supported");
    }
    /// Evaluate one search (mosReqObjList → mosObjList). Database-backed
    /// hosts translate `query` themselves; hosts that return Unsupported get
    /// the library evaluator run over object()/objectIDs() instead.
    virtual Result<ObjectQueryPage> queryObjects(const ObjectQuery& query) {
        (void)query;
        return makeError(ErrorCode::Unsupported, "Profile 3 not supported");
    }

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
    RequestFailed,     ///< a NACK mosAck answered one of our requests
    SyncAcknowledged,  ///< our mosReqAll was acked; the catalog stream follows
    ObjectCreated,     ///< our create was acked; objID from statusDescription
                       ///< (the spec-mandated overload of that field); the
                       ///< device's full mosObj push follows as ObjectUpdated
    ActionCompleted,   ///< our mosReqObjAction UPDATE/DELETE was acked
    SchemasListed,     ///< mosListSearchableSchema arrived; see searchableSchemas()
};

struct DirectoryEvent {
    DirectoryEventType type = DirectoryEventType::ObjectUpdated;
    std::string objID;
    std::string statusDescription;  ///< verbose reason on RequestFailed
    /// When the event answers one of our requests (RequestFailed,
    /// SyncAcknowledged, ObjectCreated, ActionCompleted, SchemasListed, or the
    /// ObjectUpdated of a mosReqObj hit): the channel and Seq that request's
    /// call returned, so a host with several requests in flight can tell
    /// which one this answers (a create's ack names no objID it was asked
    /// for). seq 0 for unsolicited pushes.
    Channel channel = Channel::Mom;
    Seq seq = 0;
};

/// One mosObjList page as the directory ingested it: the paging metadata
/// plus the objIDs whose mosObjs were folded into the mirror.
struct DirectoryQueryPage {
    Seq seq = 0;  ///< what query() returned for the request this page answers (aux)
    std::string queryID;
    std::size_t listReturnStart = 0;
    std::size_t listReturnEnd = 0;
    std::size_t listReturnTotal = 0;
    std::string listReturnStatus;
    std::vector<std::string> objIDs;
};

class Profile0Responder;

/// NCS-side mirror of one peer's object catalog (plan §4.6 "object
/// directory"; Profile 1 ingest + requests, Profile 3 create/action/schema/
/// search — the Profile 3 members live in profiles/profile3.cpp).
/// Ingest rules: a mosObj arriving as MessageReceived is an unsolicited push —
/// ingest and reply mosAck; a mosObj (or NACK mosAck) arriving as AckReceived
/// is the response to our mosReqObj — ingest only, no re-ack; a mosListAll
/// carries the whole catalog — ingest all and ack; a mosObjList page ingests
/// its <list> the same way (terminal reply — no re-ack). During a paced sync
/// the directory just sees ordinary pushes.
class IMOS_CORE_API ObjectDirectory {
public:
    ObjectDirectory(PeerSession* session, IClock* clock);

    /// Consume one session event; returns true when it was Profile 1 or 3
    /// traffic this directory ingested/answered.
    bool handle(const SessionEvent& event);

    Result<Seq> requestObject(std::string_view objID);  ///< mosReqObj
    Result<Seq> requestAll(int pauseSeconds);           ///< mosReqAll

    /// The mirrored object (a <mos> wrapper around one <mosObj>), or nullptr.
    [[nodiscard]] const codec::Message* object(std::string_view objID) const;
    [[nodiscard]] std::vector<std::string> objectIDs() const;
    [[nodiscard]] std::size_t size() const noexcept { return objects_.size(); }

    /// High-level directory changes for the application (ingest/delete/miss/
    /// sync-start/create/action/schema), in arrival order.
    [[nodiscard]] std::optional<DirectoryEvent> nextEvent();

    // ---- Profile 3 (advanced object workflow, M5) -----------------------

    /// Link the peer's machine-info view so resync() can prefer the Profile 3
    /// search path when the peer's listMachInfo advertises Profile 3.
    void setPeerInfo(const Profile0Responder* peer) noexcept { peerInfo_ = peer; }

    /// Ask the device to create an object or placeholder. `mosObjCreate`
    /// must be a mosObjCreate message (see makeMosObjCreate). On ACK the
    /// directory queues ObjectCreated carrying the new objID (read from the
    /// ack's statusDescription); the device's follow-up mosObj push then
    /// ingests as ObjectUpdated under the same objID. A NACK queues
    /// RequestFailed and leaves no directory entry behind.
    Result<Seq> createObject(codec::Message mosObjCreate);

    /// Request an action on a MOS-owned object (mosReqObjAction). `message`
    /// carries the metadata children (build one with
    /// makeMessage(MosReqObjAction) and the view); this call stamps the
    /// operation/objID attributes. New resolves like createObject (the ack's
    /// statusDescription is the objID); Update/Delete queue ActionCompleted.
    Result<Seq> requestObjAction(ObjAction op, std::string_view objID, codec::Message message);
    Result<Seq> requestObjAction(ObjAction op, std::string_view objID);

    /// Ask which schemas object metadata can be queried under
    /// (mosReqSearchableSchema, aux channel). The reply queues SchemasListed
    /// and lands in searchableSchemas().
    Result<Seq> requestSearchableSchemas();
    [[nodiscard]] const std::vector<std::string>& searchableSchemas() const noexcept {
        return schemas_;
    }

    /// Issue one search (mosReqObjList, aux channel). The mosObjList answer
    /// ingests its objects into the mirror and queues a DirectoryQueryPage;
    /// page through by re-issuing with the next start/end window (same
    /// queryID — the spec's cached-search shortcut).
    Result<Seq> query(const ObjectQuery& query);
    [[nodiscard]] std::optional<DirectoryQueryPage> nextQueryPage();

    /// Re-mirror the peer's catalog. When the peer's listMachInfo advertises
    /// Profile 3 this issues a match-all mosReqObjList (the spec's
    /// recommended resync mechanism — bounded pages of `pageSize`, 0 = one
    /// full page) and keeps requesting follow-up pages until the reported
    /// total is covered; otherwise it falls back to Profile 1's mosReqAll.
    Result<Seq> resync(std::size_t pageSize = 0);

    /// Automatically run resync() when our mosReqObj is NACKed (the mirror
    /// is presumed stale). Off by default — Profile 1 behavior surfaces
    /// RequestFailed and leaves recovery to the host.
    void setResyncOnMiss(bool enabled) noexcept { resyncOnMiss_ = enabled; }

private:
    enum class RequestKind : std::uint8_t { Object, All, Create, Action, Schemas, Query };
    struct Outstanding {
        RequestKind kind = RequestKind::Object;
        std::string objID;
        ObjAction action = ObjAction::New;
        bool resyncPage = false;  ///< a resync()-driven query page
    };

    void ingest(codec::Message mosObj);
    /// Reply mosAck to a received push/list, echoing its messageID (and the
    /// object's objID/objRev when it is a mosObj).
    void ackPush(Channel channel, const codec::Message& received);
    void pushEvent(DirectoryEvent event) { events_.push_back(std::move(event)); }
    /// AckReceived continuations (implemented in profile3.cpp).
    void completeCreate(const SessionEvent& event, const Outstanding& request);
    void completeAction(const SessionEvent& event, const Outstanding& request);
    void completeSchemas(const SessionEvent& event);
    void completeQuery(const SessionEvent& event, const Outstanding& request);
    [[nodiscard]] bool peerAdvertisesProfile3() const;
    Result<Seq> track(Result<Seq> seq, Channel channel, Outstanding request);

    PeerSession* session_;
    /// Reserved for future timer-based directory logic (resync backoff, sync
    /// deadlines); the Profile 1 slice needs no clock of its own.
    [[maybe_unused]] IClock* clock_;
    const Profile0Responder* peerInfo_ = nullptr;
    std::unordered_map<std::string, codec::Message> objects_;
    /// Keyed per channel: store seqs restart per stream, so mom and aux
    /// exchanges may share numbers.
    std::map<std::pair<Channel, Seq>, Outstanding> outstanding_;
    std::deque<DirectoryEvent> events_;
    std::deque<DirectoryQueryPage> pages_;
    std::vector<std::string> schemas_;
    bool resyncOnMiss_ = false;
    /// Active resync()-driven paging (Profile 3 path).
    struct ResyncPaging {
        bool active = false;
        std::string queryID;
        std::size_t pageSize = 0;
        std::size_t nextStart = 1;
    } resyncPaging_;
    std::uint64_t nextQuerySerial_ = 1;
};

/// Build a mosObjCreate message from the scalar metadata fields (spec
/// §3.3.1). Description/mosExternalMetadata riders are added by the caller
/// through codec::view::MosObjCreate on the returned message's body.
[[nodiscard]] IMOS_CORE_API codec::Message makeMosObjCreate(
    std::string_view objSlug, std::string_view objType, std::string_view objTB,
    std::string_view objDur = {}, std::string_view objGroup = {}, std::string_view createdBy = {});

}  // namespace imos::profiles
