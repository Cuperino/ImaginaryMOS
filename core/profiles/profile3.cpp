// SPDX-FileCopyrightText: 2026 Javier O. Cordero Pérez
// SPDX-License-Identifier: Apache-2.0
//
// Profile 3 — Advanced Object Based Workflow. Device side
// (Profile3Responder) plus the NCS-side Profile 3 members of
// ObjectDirectory (declared in profiles/profile1.h).

#include <imos/profiles/profile3.h>

#include <cctype>
#include <charconv>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <imos/codec/gen/views.h>
#include <imos/profiles/profile0.h>

namespace imos::profiles {

namespace {

std::string childText(const codec::Message& message, std::string_view name) {
    if (const codec::XmlNode* body = message.body()) {
        return body->childText(name);
    }
    return {};
}

std::string trimmed(std::string_view text) {
    std::size_t begin = 0;
    std::size_t end = text.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(text[begin])) != 0) {
        ++begin;
    }
    while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1])) != 0) {
        --end;
    }
    return std::string{text.substr(begin, end - begin)};
}

std::size_t parseCount(std::string_view text) {
    std::size_t value = 0;
    const char* const first = text.data();
    const char* const last = first + text.size();
    const auto [ptr, ec] = std::from_chars(first, last, value);
    if (ec != std::errc{} || ptr != last) {
        return 0;
    }
    return value;
}

bool mosAckIsAck(const codec::Message& message) {
    return message.type() == codec::MessageType::MosAck && childText(message, "status") != "NACK";
}

/// Wrap a verbatim <mosObj> subtree in a <mos> message for the mirror.
codec::Message wrapMosObj(const codec::XmlNode& mosObjNode) {
    codec::Message message = codec::makeMessage(codec::MessageType::MosObj);
    if (codec::XmlNode* body = message.body()) {
        *body = mosObjNode;  // deep copy: payload/unknown tags ride through
    }
    return message;
}

}  // namespace

std::string_view objActionName(ObjAction action) noexcept {
    switch (action) {
        case ObjAction::New:
            return "NEW";
        case ObjAction::Update:
            return "UPDATE";
        case ObjAction::Delete:
            return "DELETE";
    }
    return {};
}

std::optional<ObjAction> objActionFromName(std::string_view name) noexcept {
    if (name == "NEW") {
        return ObjAction::New;
    }
    if (name == "UPDATE") {
        return ObjAction::Update;
    }
    if (name == "DELETE") {
        return ObjAction::Delete;
    }
    return std::nullopt;  // the operation vocabulary is closed (spec §3.3.3)
}

codec::Message makeMosObjCreate(std::string_view objSlug, std::string_view objType,
                                std::string_view objTB, std::string_view objDur,
                                std::string_view objGroup, std::string_view createdBy) {
    codec::Message message = codec::makeMessage(codec::MessageType::MosObjCreate);
    if (codec::XmlNode* body = message.body()) {
        codec::view::MosObjCreate view{*body};
        view.setObjSlug(objSlug);
        if (!objGroup.empty()) {
            view.setObjGroup(objGroup);
        }
        view.setObjType(objType);
        view.setObjTB(objTB);
        if (!objDur.empty()) {
            view.setObjDur(objDur);
        }
        if (!createdBy.empty()) {
            view.setCreatedBy(createdBy);
        }
    }
    return message;
}

// ---- Profile3Responder (device / imos side) ---------------------------

Profile3Responder::Profile3Responder(PeerSession* session, Profile1Responder* profile1,
                                     Profile2Responder* profile2)
    : session_(session), profile1_(profile1), profile2_(profile2) {}

bool Profile3Responder::handle(const SessionEvent& event) {
    switch (event.type) {
        case SessionEventType::MessageReceived:
            switch (event.message.type()) {
                case codec::MessageType::MosObjCreate:
                    handleCreate(event);
                    return true;
                case codec::MessageType::MosReqObjAction:
                    handleAction(event);
                    return true;
                case codec::MessageType::MosReqSearchableSchema:
                    handleSchemaRequest(event);
                    return true;
                case codec::MessageType::MosReqObjList:
                    handleQuery(event);
                    return true;
                default:
                    return false;
            }
        case SessionEventType::AckReceived:
            return completeItemReplace(event);
        case SessionEventType::DeliveryFailed: {
            const auto it = pendingReplace_.find(std::make_pair(event.channel, event.seq));
            if (it != pendingReplace_.end()) {
                ItemReplaceResult result;
                result.seq = event.seq;
                result.roID = it->second.roID;
                result.storyID = it->second.storyID;
                result.roStatus = "delivery failed";
                replaceResults_.push_back(std::move(result));
                pendingReplace_.erase(it);
            }
            return false;  // the link fault itself stays visible to the app
        }
        default:
            return false;
    }
}

void Profile3Responder::nackRequest(const SessionEvent& event, std::string_view objID,
                                    std::string_view reason) {
    codec::Message ack = buildMosAck(objID, "", "NACK", reason);
    codec::echoMessageID(event.message, ack);
    (void)session_->enqueueReply(event.channel, std::move(ack));
}

void Profile3Responder::handleCreate(const SessionEvent& event) {
    runCreate(event);
}

void Profile3Responder::runCreate(const SessionEvent& event) {
    if (source_ == nullptr) {
        nackRequest(event, "", "no object source attached");
        return;
    }
    ObjectCreateRequest request;
    request.objSlug = childText(event.message, "objSlug");
    request.objGroup = childText(event.message, "objGroup");
    request.objType = childText(event.message, "objType");
    request.objTB = childText(event.message, "objTB");
    request.objDur = childText(event.message, "objDur");
    request.createdBy = childText(event.message, "createdBy");
    request.message = &event.message;

    Result<std::string> created = source_->createObject(request);
    if (!created.ok()) {
        nackRequest(event, "", created.error().message);
        return;
    }
    const std::string objID = std::move(created).value();
    // Spec §3.3.1: on success the ack's statusDescription *is* the new objID.
    codec::Message ack = buildMosAck(objID, "", "ACK", objID);
    codec::echoMessageID(event.message, ack);
    (void)session_->enqueueReply(event.channel, std::move(ack));
    // Ack first, push after: the full mosObj follows as its own exchange.
    pushObjectStatus(objID, "NEW");
}

void Profile3Responder::handleAction(const SessionEvent& event) {
    const codec::XmlNode* body = event.message.body();
    const std::optional<ObjAction> op =
        objActionFromName(body != nullptr ? body->attribute("operation").value_or("") : "");
    if (!op.has_value()) {
        nackRequest(event, "", "unknown mosReqObjAction operation");
        return;
    }
    if (*op == ObjAction::New) {
        runCreate(event);  // NEW mirrors mosObjCreate, objID attribute unused
        return;
    }
    const std::string objID{body->attribute("objID").value_or("")};
    if (objID.empty()) {
        nackRequest(event, "",
                    std::string{objActionName(*op)} + " operation requires an objID attribute");
        return;
    }
    if (source_ == nullptr) {
        nackRequest(event, objID, "no object source attached");
        return;
    }
    Result<void> acted = source_->actOnObject(*op, objID, event.message);
    if (!acted.ok()) {
        nackRequest(event, objID, acted.error().message);
        return;
    }
    codec::Message ack = buildMosAck(objID, "", "ACK", "");
    codec::echoMessageID(event.message, ack);
    (void)session_->enqueueReply(event.channel, std::move(ack));
    pushObjectStatus(objID, *op == ObjAction::Update ? "UPDATED" : "DELETED");
}

void Profile3Responder::pushObjectStatus(std::string_view objID, std::string_view status) {
    if (profile1_ == nullptr) {
        return;
    }
    if (status == "DELETED") {
        // The object is gone; synthesize the minimal delete push Profile 1
        // observers key on (spec §3.3.3: the subsequent mosObj's status).
        codec::Message push = codec::makeMessage(codec::MessageType::MosObj);
        if (codec::XmlNode* body = push.body()) {
            codec::view::MosObj view{*body};
            view.setObjID(objID);
            view.setStatus(status);
        }
        (void)profile1_->announceObject(std::move(push));
        return;
    }
    if (source_ == nullptr) {
        return;
    }
    std::optional<codec::Message> object = source_->object(objID);
    if (!object.has_value()) {
        return;  // vanished between the action and the push
    }
    if (codec::XmlNode* body = object->body()) {
        codec::view::MosObj{*body}.setStatus(status);
    }
    (void)profile1_->announceObject(std::move(*object));
}

void Profile3Responder::handleSchemaRequest(const SessionEvent& event) {
    codec::Message reply = codec::makeMessage(codec::MessageType::MosListSearchableSchema);
    if (codec::XmlNode* body = reply.body()) {
        codec::view::MosListSearchableSchema view{*body};
        if (const codec::XmlNode* request = event.message.body()) {
            if (const std::optional<std::string_view> username = request->attribute("username")) {
                view.setAttrUsername(*username);
            }
        }
        if (source_ != nullptr) {
            Result<SchemaList> schemas = source_->searchableSchemas();
            if (schemas.ok()) {
                bool first = true;
                for (const std::string& schema : schemas.value()) {
                    if (first) {
                        view.setMosSchema(schema);
                        first = false;
                    } else {
                        body->appendElement("mosSchema").setText(schema);
                    }
                }
            }
        }
    }
    codec::echoMessageID(event.message, reply);
    (void)session_->enqueueReply(event.channel, std::move(reply));
}

Result<ObjectQueryPage> Profile3Responder::runQuery(const ObjectQuery& query) {
    if (source_ == nullptr) {
        return makeError(ErrorCode::WrongState, "no object source attached");
    }
    Result<ObjectQueryPage> page = source_->queryObjects(query);
    if (page.ok() || page.error().code != ErrorCode::Unsupported) {
        return page;
    }
    // The host does not translate queries itself: run the library evaluator
    // over the Profile 1 enumeration so simple hosts get search for free.
    std::vector<codec::Message> owned;
    for (const std::string& id : source_->objectIDs()) {
        std::optional<codec::Message> object = source_->object(id);
        if (object.has_value()) {
            owned.push_back(std::move(*object));
        }
    }
    std::vector<const codec::Message*> corpus;
    corpus.reserve(owned.size());
    for (const codec::Message& message : owned) {
        corpus.push_back(&message);
    }
    return evaluateQuery(query, corpus);
}

void Profile3Responder::handleQuery(const SessionEvent& event) {
    Result<ObjectQuery> parsed = parseObjectQuery(event.message);
    ObjectQuery query;
    ObjectQueryPage page;
    if (parsed.ok()) {
        query = std::move(parsed).value();
        Result<ObjectQueryPage> evaluated = runQuery(query);
        if (evaluated.ok()) {
            page = std::move(evaluated).value();
        } else {
            page.listReturnStatus = evaluated.error().message;
        }
    } else {
        // Malformed query: still answer the exchange — a zero-total
        // mosObjList whose status carries the diagnostic (§4.1 leniency:
        // the requester learns why rather than timing out).
        query.queryID = childText(event.message, "queryID");
        if (const codec::XmlNode* body = event.message.body()) {
            query.username = std::string{body->attribute("username").value_or("")};
        }
        page.listReturnStatus = parsed.error().message;
    }
    codec::Message reply = buildMosObjList(query, page);
    codec::echoMessageID(event.message, reply);
    (void)session_->enqueueReply(event.channel, std::move(reply));
}

Result<Seq> Profile3Responder::replaceItem(std::string_view roID, std::string_view storyID,
                                           const codec::XmlNode& item) {
    if (roID.empty() || storyID.empty()) {
        return makeError(ErrorCode::InvalidArgument, "mosItemReplace requires roID and storyID");
    }
    if (!item.isElement("item") || item.childText("itemID").empty()) {
        return makeError(ErrorCode::InvalidArgument,
                         "mosItemReplace requires an <item> subtree with an itemID");
    }
    codec::Message message = codec::makeMessage(codec::MessageType::MosItemReplace);
    if (codec::XmlNode* body = message.body()) {
        codec::view::MosItemReplace view{*body};
        view.setRoID(roID);
        view.setStoryID(storyID);
        view.ensureItem() = item;  // verbatim subtree
    }
    PendingReplace pending;
    pending.roID = std::string{roID};
    pending.storyID = std::string{storyID};
    pending.message = message;  // folded into the shadow model on OK
    Result<Seq> seq = session_->enqueue(std::move(message));
    if (seq.ok()) {
        pendingReplace_[std::make_pair(Channel::Ro, seq.value())] = std::move(pending);
    }
    return seq;
}

bool Profile3Responder::completeItemReplace(const SessionEvent& event) {
    const auto it = pendingReplace_.find(std::make_pair(event.channel, event.seq));
    if (it == pendingReplace_.end()) {
        return false;
    }
    PendingReplace pending = std::move(it->second);
    pendingReplace_.erase(it);

    const bool nack =
        event.message.type() != codec::MessageType::RoAck || roAckIsNack(event.message);
    ItemReplaceResult result;
    result.seq = event.seq;
    result.ok = !nack;
    result.roID = pending.roID;
    result.storyID = pending.storyID;
    result.roStatus = roAckStatusText(event.message);
    if (!nack) {
        // Converge the shadow model with what the NCS just accepted.
        if (profile2_ != nullptr && profile2_->modelMode()) {
            (void)profile2_->model().apply(pending.message);
        }
    } else if (profile2_ != nullptr && profile2_->modelMode()) {
        // §3.3: the NCS refused a story we believe exists — the shadow is
        // presumed stale; escalate to the resync driver.
        profile2_->noteUnknownRunningOrder(pending.roID, Channel::Ro,
                                           "mosItemReplace NACKed: " + result.roStatus);
    }
    replaceResults_.push_back(std::move(result));
    return true;
}

std::optional<ItemReplaceResult> Profile3Responder::nextItemReplaceResult() {
    if (replaceResults_.empty()) {
        return std::nullopt;
    }
    ItemReplaceResult result = std::move(replaceResults_.front());
    replaceResults_.pop_front();
    return result;
}

// ---- ObjectDirectory Profile 3 members (NCS / incs side) ---------------

bool ObjectDirectory::peerAdvertisesProfile3() const {
    if (peerInfo_ == nullptr) {
        return false;
    }
    const PeerMachineInfo* peer = peerInfo_->peerMachineInfo();
    return peer != nullptr && peer->info.profiles[3];
}

Result<Seq> ObjectDirectory::createObject(codec::Message mosObjCreate) {
    if (mosObjCreate.type() != codec::MessageType::MosObjCreate) {
        return makeError(ErrorCode::InvalidArgument, "createObject requires a mosObjCreate");
    }
    Outstanding tracked;
    tracked.kind = RequestKind::Create;
    return track(session_->enqueue(std::move(mosObjCreate)), Channel::Mom, std::move(tracked));
}

Result<Seq> ObjectDirectory::requestObjAction(ObjAction op, std::string_view objID) {
    return requestObjAction(op, objID, codec::makeMessage(codec::MessageType::MosReqObjAction));
}

Result<Seq> ObjectDirectory::requestObjAction(ObjAction op, std::string_view objID,
                                              codec::Message message) {
    if (message.type() != codec::MessageType::MosReqObjAction) {
        return makeError(ErrorCode::InvalidArgument,
                         "requestObjAction requires a mosReqObjAction message");
    }
    if (op != ObjAction::New && objID.empty()) {
        return makeError(ErrorCode::InvalidArgument,
                         std::string{objActionName(op)} + " requires the target objID");
    }
    if (codec::XmlNode* body = message.body()) {
        codec::view::MosReqObjAction view{*body};
        view.setAttrOperation(objActionName(op));
        if (op != ObjAction::New) {
            view.setAttrObjID(objID);
        }
    }
    Outstanding tracked;
    tracked.kind = op == ObjAction::New ? RequestKind::Create : RequestKind::Action;
    tracked.objID = std::string{objID};
    tracked.action = op;
    return track(session_->enqueue(std::move(message)), Channel::Mom, std::move(tracked));
}

Result<Seq> ObjectDirectory::requestSearchableSchemas() {
    Outstanding tracked;
    tracked.kind = RequestKind::Schemas;
    return track(session_->enqueue(codec::makeMessage(codec::MessageType::MosReqSearchableSchema)),
                 Channel::Aux, std::move(tracked));
}

Result<Seq> ObjectDirectory::query(const ObjectQuery& query) {
    if (query.listReturnStart < 1) {
        return makeError(ErrorCode::InvalidArgument, "listReturnStart must be 1 or greater");
    }
    Outstanding tracked;
    tracked.kind = RequestKind::Query;
    return track(session_->enqueue(buildMosReqObjList(query)), Channel::Aux, std::move(tracked));
}

Result<Seq> ObjectDirectory::resync(std::size_t pageSize) {
    if (!peerAdvertisesProfile3()) {
        return requestAll(0);  // Profile 1 fallback: batch mosReqAll
    }
    resyncPaging_.active = true;
    resyncPaging_.queryID = "imos-resync-" + std::to_string(nextQuerySerial_++);
    resyncPaging_.pageSize = pageSize;
    resyncPaging_.nextStart = 1;

    ObjectQuery matchAll;  // null generalSearch, no groups → everything
    matchAll.queryID = resyncPaging_.queryID;
    matchAll.listReturnStart = 1;
    if (pageSize > 0) {
        matchAll.listReturnEnd = pageSize;
    }
    Outstanding tracked;
    tracked.kind = RequestKind::Query;
    tracked.resyncPage = true;
    return track(session_->enqueue(buildMosReqObjList(matchAll)), Channel::Aux, std::move(tracked));
}

void ObjectDirectory::completeCreate(const SessionEvent& event, const Outstanding& request) {
    (void)request;
    if (!mosAckIsAck(event.message)) {
        pushEvent(DirectoryEvent{DirectoryEventType::RequestFailed,
                                 childText(event.message, "objID"),
                                 childText(event.message, "statusDescription")});
        return;  // NACK leaves no phantom directory entry
    }
    // Spec: the ACK's statusDescription carries the new objID.
    std::string objID = trimmed(childText(event.message, "statusDescription"));
    if (objID.empty()) {
        objID = trimmed(childText(event.message, "objID"));
    }
    pushEvent(DirectoryEvent{DirectoryEventType::ObjectCreated, std::move(objID),
                             childText(event.message, "statusDescription")});
    // The device's follow-up mosObj push ingests as ObjectUpdated and
    // correlates by this objID.
}

void ObjectDirectory::completeAction(const SessionEvent& event, const Outstanding& request) {
    if (!mosAckIsAck(event.message)) {
        pushEvent(DirectoryEvent{DirectoryEventType::RequestFailed, request.objID,
                                 childText(event.message, "statusDescription")});
        return;
    }
    pushEvent(DirectoryEvent{DirectoryEventType::ActionCompleted, request.objID,
                             childText(event.message, "statusDescription")});
}

void ObjectDirectory::completeSchemas(const SessionEvent& event) {
    if (event.message.type() != codec::MessageType::MosListSearchableSchema) {
        pushEvent(DirectoryEvent{DirectoryEventType::RequestFailed, "",
                                 childText(event.message, "statusDescription")});
        return;
    }
    schemas_.clear();
    if (const codec::XmlNode* body = event.message.body()) {
        for (const codec::XmlNode& child : body->children) {
            if (child.isElement("mosSchema")) {
                schemas_.push_back(child.text());
            }
        }
    }
    pushEvent(DirectoryEvent{DirectoryEventType::SchemasListed, "", ""});
}

void ObjectDirectory::completeQuery(const SessionEvent& event, const Outstanding& request) {
    if (event.message.type() != codec::MessageType::MosObjList) {
        resyncPaging_.active = false;
        pushEvent(DirectoryEvent{DirectoryEventType::RequestFailed, "",
                                 childText(event.message, "statusDescription")});
        return;
    }
    DirectoryQueryPage page;
    page.queryID = childText(event.message, "queryID");
    page.listReturnStart = parseCount(childText(event.message, "listReturnStart"));
    page.listReturnEnd = parseCount(childText(event.message, "listReturnEnd"));
    page.listReturnTotal = parseCount(childText(event.message, "listReturnTotal"));
    page.listReturnStatus = childText(event.message, "listReturnStatus");
    if (const codec::XmlNode* body = event.message.body()) {
        if (const codec::XmlNode* list = body->firstChildElement("list")) {
            for (const codec::XmlNode& child : list->children) {
                if (child.isElement("mosObj")) {
                    page.objIDs.push_back(child.childText("objID"));
                    ingest(wrapMosObj(child));
                }
            }
        }
    }
    const bool morePages = request.resyncPage && resyncPaging_.active && page.listReturnEnd > 0 &&
                           page.listReturnEnd < page.listReturnTotal;
    pages_.push_back(std::move(page));
    if (!request.resyncPage) {
        return;
    }
    if (!morePages) {
        resyncPaging_.active = false;
        return;
    }
    // Chain the next window (same queryID — the spec's cached-search
    // shortcut lets the device serve it from its buffer).
    ObjectQuery next;
    next.queryID = resyncPaging_.queryID;
    next.listReturnStart = pages_.back().listReturnEnd + 1;
    if (resyncPaging_.pageSize > 0) {
        next.listReturnEnd = pages_.back().listReturnEnd + resyncPaging_.pageSize;
    }
    Outstanding tracked;
    tracked.kind = RequestKind::Query;
    tracked.resyncPage = true;
    (void)track(session_->enqueue(buildMosReqObjList(next)), Channel::Aux, std::move(tracked));
}

std::optional<DirectoryQueryPage> ObjectDirectory::nextQueryPage() {
    if (pages_.empty()) {
        return std::nullopt;
    }
    DirectoryQueryPage page = std::move(pages_.front());
    pages_.pop_front();
    return page;
}

}  // namespace imos::profiles
