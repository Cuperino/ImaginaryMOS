// SPDX-FileCopyrightText: 2026 Javier O. Cordero Pérez
// SPDX-License-Identifier: Apache-2.0

#include <imos/profiles/profile1.h>

#include <charconv>
#include <string>
#include <system_error>
#include <utility>

#include <imos/codec/gen/views.h>
#include <imos/profiles/running_order_model.h>

namespace imos::profiles {

namespace {

/// Read a direct child element's text, or "" when absent.
std::string childText(const codec::Message& message, std::string_view name) {
    if (const codec::XmlNode* body = message.body()) {
        return body->childText(name);
    }
    return {};
}

/// Wrap a verbatim <mosObj> subtree in a <mos> message for the mirror.
codec::Message wrapObject(const codec::XmlNode& mosObjNode) {
    codec::Message message = codec::makeMessage(codec::MessageType::MosObj);
    if (codec::XmlNode* body = message.body()) {
        *body = mosObjNode;  // deep copy: payload/unknown tags ride through
    }
    return message;
}

/// Parse a mosReqAll <pause> as a non-negative integer; 0 (batch) is the
/// default when absent or unparseable (spec §3.2.1: pause of zero → mosListAll).
int parsePause(const codec::Message& reqAll) {
    const std::string text = childText(reqAll, "pause");
    int value = 0;
    const char* const first = text.data();
    const char* const last = first + text.size();
    const auto [ptr, ec] = std::from_chars(first, last, value);
    if (ec != std::errc{} || ptr != last || value < 0) {
        return 0;
    }
    return value;
}

}  // namespace

codec::Message buildMosAck(std::string_view objID, std::string_view objRev, std::string_view status,
                           std::string_view statusDescription) {
    codec::Message message = codec::makeMessage(codec::MessageType::MosAck);
    if (codec::XmlNode* body = message.body()) {
        codec::view::MosAck view{*body};
        view.setObjID(objID);
        view.setObjRev(objRev);
        view.setStatus(status);
        view.setStatusDescription(statusDescription);
    }
    return message;
}

std::string mosObjID(const codec::Message& mosObj) {
    return childText(mosObj, "objID");
}

// ---- Profile1Responder (device / imos side) ---------------------------

Profile1Responder::Profile1Responder(IObjectSource* source, PeerSession* session, IClock* clock)
    : source_(source), session_(session), clock_(clock) {}

bool Profile1Responder::handle(const SessionEvent& event) {
    if (source_ == nullptr || event.type != SessionEventType::MessageReceived) {
        return false;
    }
    switch (event.message.type()) {
        case codec::MessageType::MosReqObj:
            handleReqObj(event);
            return true;
        case codec::MessageType::MosReqAll:
            handleReqAll(event);
            return true;
        default:
            return false;
    }
}

void Profile1Responder::handleReqObj(const SessionEvent& event) {
    const std::string objID = childText(event.message, "objID");
    std::optional<codec::Message> object = source_->object(objID);

    codec::Message reply = object.has_value()
                               ? std::move(*object)
                               : buildMosAck(objID, "", "NACK", "objID '" + objID + "' not found");
    codec::echoMessageID(event.message, reply);
    (void)session_->enqueueReply(event.channel, std::move(reply));
}

void Profile1Responder::handleReqAll(const SessionEvent& event) {
    const int pause = parsePause(event.message);

    // Both methods start by acking the request (spec §3.2.1); the ack completes
    // the NCS's mosReqAll exchange, then the catalog follows as its own traffic.
    codec::Message ack = buildMosAck("", "", "ACK", "");
    codec::echoMessageID(event.message, ack);
    (void)session_->enqueueReply(event.channel, std::move(ack));

    if (pause <= 0) {
        sendBatch(event.channel);
        return;
    }
    // Method 2: pace individual mosObj messages `pause` seconds apart, released
    // by poll() so live traffic can interleave.
    paced_ = PacedSync{};
    paced_.ids = source_->objectIDs();
    paced_.pauseMillis = static_cast<std::int64_t>(pause) * 1000;
    paced_.channel = event.channel;
    paced_.nextRelease = clock_->now() + paced_.pauseMillis;
    paced_.active = !paced_.ids.empty();
}

void Profile1Responder::sendBatch(Channel channel) {
    codec::Message list = codec::makeMessage(codec::MessageType::MosListAll);
    if (codec::XmlNode* body = list.body()) {
        codec::view::MosListAll view{*body};
        for (const std::string& id : source_->objectIDs()) {
            std::optional<codec::Message> object = source_->object(id);
            if (!object.has_value()) {
                continue;  // vanished between snapshot and fetch → skip
            }
            if (const codec::XmlNode* objBody = object->body()) {
                view.addMosObj() = *objBody;  // verbatim <mosObj> subtree
            }
        }
    }
    (void)session_->enqueue(channel, std::move(list));  // awaits its own mosAck
}

void Profile1Responder::poll() {
    if (!paced_.active) {
        return;
    }
    const MonotonicTime now = clock_->now();
    while (paced_.active && now >= paced_.nextRelease) {
        releaseNextPaced();
        if (paced_.active) {
            paced_.nextRelease = paced_.nextRelease + paced_.pauseMillis;
        }
    }
}

void Profile1Responder::releaseNextPaced() {
    while (paced_.index < paced_.ids.size()) {
        std::optional<codec::Message> object = source_->object(paced_.ids[paced_.index++]);
        if (!object.has_value()) {
            continue;  // vanished mid-sync → skip, keep the cadence
        }
        (void)session_->enqueue(paced_.channel, std::move(*object));
        if (paced_.index >= paced_.ids.size()) {
            paced_.active = false;  // that was the last object in the snapshot
        }
        return;
    }
    paced_.active = false;  // snapshot exhausted (trailing objects all vanished)
}

std::optional<MonotonicTime> Profile1Responder::nextDeadline() const {
    if (paced_.active) {
        return paced_.nextRelease;
    }
    return std::nullopt;
}

Result<Seq> Profile1Responder::announceObject(codec::Message mosObj) {
    if (mosObj.type() != codec::MessageType::MosObj) {
        return makeError(ErrorCode::InvalidArgument, "announceObject requires a mosObj message");
    }
    return session_->enqueue(std::move(mosObj));
}

// ---- ObjectDirectory (NCS / incs side) --------------------------------

ObjectDirectory::ObjectDirectory(PeerSession* session, IClock* clock)
    : session_(session), clock_(clock) {}

bool ObjectDirectory::handle(const SessionEvent& event) {
    switch (event.type) {
        case SessionEventType::MessageReceived:
            switch (event.message.type()) {
                case codec::MessageType::MosObj: {
                    // Unsolicited push: mirror it and ack.
                    ackPush(event.channel, event.message);
                    ingest(codec::Message{event.message});
                    return true;
                }
                case codec::MessageType::MosListAll: {
                    codec::Message list{event.message};
                    if (codec::XmlNode* body = list.body()) {
                        codec::view::MosListAll view{*body};
                        const std::size_t count = view.mosObjCount();
                        for (std::size_t i = 0; i < count; ++i) {
                            if (const codec::XmlNode* child = view.mosObj(i)) {
                                ingest(wrapObject(*child));
                            }
                        }
                    }
                    ackPush(event.channel, event.message);
                    return true;
                }
                default:
                    return false;
            }
        case SessionEventType::AckReceived: {
            const auto it = outstanding_.find(std::make_pair(event.channel, event.seq));
            if (it == outstanding_.end()) {
                return false;
            }
            const Outstanding request = it->second;
            outstanding_.erase(it);
            switch (request.kind) {
                case RequestKind::Object:
                    if (event.message.type() == codec::MessageType::MosObj) {
                        ingest(codec::Message{event.message});  // reqObj hit
                    } else {                                    // NACK mosAck: miss
                        pushEvent(DirectoryEvent{DirectoryEventType::RequestFailed, request.objID,
                                                 childText(event.message, "statusDescription")});
                        if (resyncOnMiss_) {
                            (void)resync(resyncPaging_.pageSize);  // mirror presumed stale
                        }
                    }
                    break;
                case RequestKind::All:
                    // mosReqAll acked; the catalog (mosListAll / paced mosObj) follows.
                    pushEvent(DirectoryEvent{DirectoryEventType::SyncAcknowledged, "", ""});
                    break;
                case RequestKind::Create:
                    completeCreate(event, request);
                    break;
                case RequestKind::Action:
                    completeAction(event, request);
                    break;
                case RequestKind::Schemas:
                    completeSchemas(event);
                    break;
                case RequestKind::Query:
                    completeQuery(event, request);
                    break;
            }
            return true;
        }
        default:
            return false;
    }
}

Result<Seq> ObjectDirectory::track(Result<Seq> seq, Channel channel, Outstanding request) {
    if (seq.ok()) {
        outstanding_[std::make_pair(channel, seq.value())] = std::move(request);
    }
    return seq;
}

void ObjectDirectory::ackPush(Channel channel, const codec::Message& received) {
    codec::Message ack =
        buildMosAck(childText(received, "objID"), childText(received, "objRev"), "ACK", "");
    codec::echoMessageID(received, ack);
    (void)session_->enqueueReply(channel, std::move(ack));
}

void ObjectDirectory::ingest(codec::Message mosObj) {
    std::string objID = mosObjID(mosObj);
    if (objID.empty()) {
        return;  // malformed: nothing to key the mirror on
    }
    if (childText(mosObj, "status") == "DELETED") {
        objects_.erase(objID);
        pushEvent(DirectoryEvent{DirectoryEventType::ObjectDeleted, std::move(objID), ""});
        return;
    }
    pushEvent(DirectoryEvent{DirectoryEventType::ObjectUpdated, objID, ""});
    // Spec §2.2: an "UPDATED" push carries only what changed — tags it
    // leaves out stay intact on the mirrored object, and a tag sent with a
    // null value is removed. Every other status (NEW, a list/resync entry)
    // describes the object in full and replaces the mirror's copy.
    const auto existing = objects_.find(objID);
    if (existing != objects_.end() && childText(mosObj, "status") == "UPDATED") {
        codec::XmlNode* target = existing->second.body();
        const codec::XmlNode* incoming = mosObj.body();
        if (target != nullptr && incoming != nullptr) {
            mergeElementFields(*target, *incoming, "objID");
            return;
        }
    }
    objects_.insert_or_assign(std::move(objID), std::move(mosObj));
}

Result<Seq> ObjectDirectory::requestObject(std::string_view objID) {
    codec::Message request = codec::makeMessage(codec::MessageType::MosReqObj);
    if (codec::XmlNode* body = request.body()) {
        codec::view::MosReqObj{*body}.setObjID(objID);
    }
    Outstanding tracked;
    tracked.kind = RequestKind::Object;
    tracked.objID = std::string{objID};
    return track(session_->enqueue(std::move(request)), Channel::Mom, std::move(tracked));
}

Result<Seq> ObjectDirectory::requestAll(int pauseSeconds) {
    codec::Message request = codec::makeMessage(codec::MessageType::MosReqAll);
    if (codec::XmlNode* body = request.body()) {
        codec::view::MosReqAll{*body}.setPause(std::to_string(pauseSeconds < 0 ? 0 : pauseSeconds));
    }
    Outstanding tracked;
    tracked.kind = RequestKind::All;
    return track(session_->enqueue(std::move(request)), Channel::Mom, std::move(tracked));
}

const codec::Message* ObjectDirectory::object(std::string_view objID) const {
    const auto it = objects_.find(std::string{objID});
    return it == objects_.end() ? nullptr : &it->second;
}

std::vector<std::string> ObjectDirectory::objectIDs() const {
    std::vector<std::string> ids;
    ids.reserve(objects_.size());
    for (const auto& [id, message] : objects_) {
        ids.push_back(id);
    }
    return ids;
}

std::optional<DirectoryEvent> ObjectDirectory::nextEvent() {
    if (events_.empty()) {
        return std::nullopt;
    }
    DirectoryEvent event = std::move(events_.front());
    events_.pop_front();
    return event;
}

}  // namespace imos::profiles
