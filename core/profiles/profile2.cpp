// SPDX-FileCopyrightText: 2026 Javier O. Cordero Pérez
// SPDX-License-Identifier: Apache-2.0

#include <imos/profiles/profile2.h>

#include <string>
#include <utility>

#include <imos/codec/gen/views.h>
#include <imos/time_format.h>

namespace imos::profiles {

namespace {

std::string messageRoID(const codec::Message& message) {
    if (const codec::XmlNode* body = message.body()) {
        return body->childText("roID");
    }
    return {};
}

bool isConstructionType(codec::MessageType type) noexcept {
    switch (type) {
        case codec::MessageType::RoCreate:
        case codec::MessageType::RoReplace:
        case codec::MessageType::RoMetadataReplace:
        case codec::MessageType::RoDelete:
        case codec::MessageType::RoElementAction:
        case codec::MessageType::RoReadyToAir:
        case codec::MessageType::RoElementStat:
            return true;
        default:
            return false;
    }
}

}  // namespace

// ---- shared helpers ---------------------------------------------------

codec::Message buildRoAck(std::string_view roID, std::string_view roStatus,
                          std::span<const ElementStatus> statuses) {
    codec::Message message = codec::makeMessage(codec::MessageType::RoAck);
    if (codec::XmlNode* body = message.body()) {
        body->appendElement("roID").setText(roID);
        body->appendElement("roStatus").setText(roStatus);
        // Tuples are appended in spec order — interleaved, not grouped by
        // name — so the optional itemChannel can never misalign readers.
        for (const ElementStatus& status : statuses) {
            body->appendElement("storyID").setText(status.storyID);
            body->appendElement("itemID").setText(status.itemID);
            body->appendElement("objID").setText(status.objID);
            if (!status.itemChannel.empty()) {
                body->appendElement("itemChannel").setText(status.itemChannel);
            }
            body->appendElement("status").setText(status.status);
        }
    }
    return message;
}

std::string roAckStatusText(const codec::Message& roAck) {
    if (const codec::XmlNode* body = roAck.body()) {
        return body->childText("roStatus");
    }
    return {};
}

bool roAckIsNack(const codec::Message& roAck) {
    // Spec §6: roStatus is "OK" or an error description — anything that is
    // not OK is a refusal, whatever words the peer chose for it.
    const std::string text = roAckStatusText(roAck);
    constexpr std::string_view kSpace = " \t\r\n";
    const std::size_t first = text.find_first_not_of(kSpace);
    if (first == std::string::npos) {
        return true;  // an empty status is not OK
    }
    const std::size_t last = text.find_last_not_of(kSpace);
    return std::string_view{text}.substr(first, last - first + 1) != "OK";
}

std::vector<ElementStatus> roAckStatuses(const codec::Message& roAck) {
    std::vector<ElementStatus> statuses;
    const codec::XmlNode* body = roAck.body();
    if (body == nullptr) {
        return statuses;
    }
    bool open = false;
    for (const codec::XmlNode& child : body->children) {
        if (child.isElement("storyID")) {
            statuses.emplace_back();
            statuses.back().storyID = child.text();
            open = true;
        } else if (open && child.isElement("itemID")) {
            statuses.back().itemID = child.text();
        } else if (open && child.isElement("objID")) {
            statuses.back().objID = child.text();
        } else if (open && child.isElement("itemChannel")) {
            statuses.back().itemChannel = child.text();
        } else if (open && child.isElement("status")) {
            statuses.back().status = child.text();
        }
    }
    return statuses;
}

codec::Message buildRoElementStat(const ElementStatusReport& report) {
    codec::Message message = codec::makeMessage(codec::MessageType::RoElementStat);
    if (codec::XmlNode* body = message.body()) {
        codec::view::RoElementStat view{*body};
        view.setAttrElement(roElementName(report.element));
        view.setRoID(report.roID);
        if (!report.storyID.empty()) {
            view.setStoryID(report.storyID);
        }
        if (!report.itemID.empty()) {
            view.setItemID(report.itemID);
        }
        if (!report.objID.empty()) {
            view.setObjID(report.objID);
        }
        if (!report.itemChannel.empty()) {
            view.setItemChannel(report.itemChannel);
        }
        view.setStatus(report.status);
        view.setTime(report.time);
    }
    return message;
}

Result<ElementStatusReport> parseRoElementStat(const codec::Message& message) {
    if (message.type() != codec::MessageType::RoElementStat) {
        return makeError(ErrorCode::InvalidArgument, "not a roElementStat message");
    }
    const codec::XmlNode* body = message.body();
    if (body == nullptr) {
        return makeError(ErrorCode::InvalidArgument, "roElementStat has no body");
    }
    std::string elementName;
    if (const std::optional<std::string_view> attr = body->attribute("element")) {
        elementName = *attr;
    }
    const std::optional<RoElement> element = roElementFromName(elementName);
    if (!element.has_value()) {
        return makeError(ErrorCode::ProtocolViolation,
                         "roElementStat element '" + elementName + "' is not RO, STORY, or ITEM");
    }
    ElementStatusReport report;
    report.element = *element;
    report.roID = body->childText("roID");
    report.storyID = body->childText("storyID");
    report.itemID = body->childText("itemID");
    report.objID = body->childText("objID");
    report.itemChannel = body->childText("itemChannel");
    report.status = body->childText("status");
    report.time = body->childText("time");
    return report;
}

// ---- Profile2Responder (device / imos side) ---------------------------

Profile2Responder::Profile2Responder(PeerSession* session, IClock* clock)
    : session_(session), clock_(clock) {}

bool Profile2Responder::handle(const SessionEvent& event) {
    switch (event.type) {
        case SessionEventType::MessageReceived:
            if (!isConstructionType(event.message.type())) {
                return false;
            }
            handleConstruction(event);
            return true;
        case SessionEventType::AckReceived:
            if (event.channel != Channel::Ro ||
                pendingResync_.find(event.seq) == pendingResync_.end()) {
                return false;  // acks for the app's own sends stay visible
            }
            completeResync(event);
            return true;
        default:
            return false;
    }
}

Result<void> Profile2Responder::dispatchToSink(const codec::Message& message) {
    switch (message.type()) {
        case codec::MessageType::RoCreate:
            return sink_->onRoCreate(message);
        case codec::MessageType::RoReplace:
            return sink_->onRoReplace(message);
        case codec::MessageType::RoMetadataReplace:
            return sink_->onRoMetadataReplace(message);
        case codec::MessageType::RoDelete:
            return sink_->onRoDelete(message);
        case codec::MessageType::RoElementAction:
            return sink_->onRoElementAction(message);
        case codec::MessageType::RoReadyToAir:
            return sink_->onRoReadyToAir(message);
        case codec::MessageType::RoElementStat:
            return sink_->onRoElementStat(message);
        default:
            return makeError(ErrorCode::InvalidArgument, "not a construction message");
    }
}

std::string Profile2Responder::collectObjectStatuses(const codec::Message& message,
                                                     std::vector<ElementStatus>& statuses) {
    if (objects_ == nullptr) {
        return {};
    }
    const codec::XmlNode* body = message.body();
    if (body == nullptr) {
        return {};
    }
    std::string firstUnknown;
    const auto collectItem = [&](const codec::XmlNode& item, std::string_view storyID) {
        ElementStatus status;
        status.storyID = storyID;
        status.itemID = item.childText("itemID");
        status.objID = item.childText("objID");
        const bool known = !status.objID.empty() && objects_->object(status.objID).has_value();
        status.status = known ? "LOADED" : "UNKNOWN";
        if (!known && firstUnknown.empty()) {
            firstUnknown = status.objID;
        }
        statuses.push_back(std::move(status));
    };
    const auto collectStory = [&](const codec::XmlNode& story) {
        const std::string storyID = story.childText("storyID");
        for (const codec::XmlNode& child : story.children) {
            if (child.isElement("item")) {
                collectItem(child, storyID);
            }
        }
    };

    if (message.type() == codec::MessageType::RoCreate ||
        message.type() == codec::MessageType::RoReplace) {
        for (const codec::XmlNode& child : body->children) {
            if (child.isElement("story")) {
                collectStory(child);
            }
        }
    } else if (message.type() == codec::MessageType::RoElementAction) {
        // Only INSERT/REPLACE carry new content; MOVE/DELETE/SWAP reference
        // elements the device already validated on arrival.
        if (const codec::XmlNode* source = body->firstChildElement("element_source")) {
            const codec::XmlNode* target = body->firstChildElement("element_target");
            const std::string targetStory = target ? target->childText("storyID") : std::string{};
            for (const codec::XmlNode& child : source->children) {
                if (child.isElement("story")) {
                    collectStory(child);
                } else if (child.isElement("item")) {
                    collectItem(child, targetStory);
                }
            }
        }
    }
    return firstUnknown;
}

void Profile2Responder::handleConstruction(const SessionEvent& event) {
    const codec::Message& message = event.message;
    const std::string roID = messageRoID(message);

    // Profile 6: scan new content for foreign mosID references before any
    // state can change; rejecting turns the message into a NACK outright.
    std::vector<ClassifiedReference> references;
    if (profile6_ != nullptr && profile6_->classifying()) {
        references = profile6_->collectReferences(message);
        if (profile6_->rejecting()) {
            if (const ClassifiedReference* foreign = Profile6Classifier::firstForeign(references)) {
                codec::Message ack =
                    buildRoAck(roID, "NACK: foreign object reference '" + foreign->mosID +
                                         "' (objID '" + foreign->objID + "')");
                codec::echoMessageID(message, ack);
                (void)session_->enqueueReply(event.channel, std::move(ack));
                return;
            }
        }
    }

    if (sink_ != nullptr) {
        // Raw mode: the host owns state and resync; we only run the ack.
        Result<void> outcome = dispatchToSink(message);
        codec::Message ack =
            buildRoAck(roID, outcome.ok() ? std::string{"OK"} : "NACK: " + outcome.error().message);
        codec::echoMessageID(message, ack);
        (void)session_->enqueueReply(event.channel, std::move(ack));
        if (outcome.ok() && profile6_ != nullptr) {
            profile6_->announceForeign(*session_, event.channel, roID, references);
        }
        return;
    }

    std::vector<ElementStatus> statuses;
    const std::string firstUnknown = collectObjectStatuses(message, statuses);
    if (!firstUnknown.empty() && objectPolicy_ == UnknownObjectPolicy::Reject) {
        codec::Message ack =
            buildRoAck(roID, "NACK: unknown object '" + firstUnknown + "'", statuses);
        codec::echoMessageID(message, ack);
        (void)session_->enqueueReply(event.channel, std::move(ack));
        return;  // rejected before the model ever sees it
    }

    const Result<void> outcome = model_.apply(message);
    std::string roStatus;
    if (outcome.ok()) {
        // The message was applied, so roStatus is OK (spec §6: anything else
        // reads as an error description). Unresolved objects ride the ack
        // as informational per-item UNKNOWN statuses.
        roStatus = "OK";
    } else {
        roStatus = "NACK: " + outcome.error().message;
        statuses.clear();  // the payload tuples describe content we rejected
    }
    codec::Message ack = buildRoAck(roID, roStatus, statuses);
    codec::echoMessageID(message, ack);
    (void)session_->enqueueReply(event.channel, std::move(ack));

    if (outcome.ok()) {
        if (profile6_ != nullptr) {
            profile6_->announceForeign(*session_, event.channel, roID, references);
        }
        if (message.type() == codec::MessageType::RoElementStat) {
            if (Result<ElementStatusReport> report = parseRoElementStat(message); report.ok()) {
                statusEvents_.push_back(std::move(report).value());
            }
        }
        return;
    }
    if (outcome.error().code == ErrorCode::NotFound && !roID.empty()) {
        startResync(roID, event.channel, outcome.error().message, /*announce=*/true);
    }
}

void Profile2Responder::requestRunningOrder(std::string_view roID) {
    // Hydration: same rebuild machinery, but nothing is out of sync, so no
    // ResyncNeeded announcement.
    startResync(std::string{roID}, Channel::Ro, "hydrating", /*announce=*/false);
}

void Profile2Responder::noteUnknownRunningOrder(std::string_view roID, Channel channel,
                                                std::string_view reason) {
    startResync(std::string{roID}, channel, reason, /*announce=*/true);
}

void Profile2Responder::startResync(const std::string& roID, Channel channel,
                                    std::string_view reason, bool announce) {
    if (resyncing_.contains(roID)) {
        return;  // one rebuild per RO; later confused messages just NACK
    }
    resyncing_.insert(roID);

    if (announce) {
        // Surface a single "resyncing" state to the application through the
        // session's event queue seam (§3.3) — the session stays
        // message-agnostic.
        SessionEvent event;
        event.type = SessionEventType::ResyncNeeded;
        event.channel = channel;
        event.diagnostic = "resynchronizing roID '" + roID + "': " + std::string{reason};
        session_->postEvent(std::move(event));
    }

    codec::Message request = codec::makeMessage(codec::MessageType::RoReq);
    if (codec::XmlNode* body = request.body()) {
        codec::view::RoReq{*body}.setRoID(roID);
    }
    Result<Seq> seq = session_->enqueue(std::move(request));
    if (seq.ok()) {
        pendingResync_.emplace(seq.value(), roID);
    } else {
        resyncing_.erase(roID);  // a later confused message retries
        SessionEvent failed;
        failed.type = SessionEventType::ProtocolError;
        failed.channel = channel;
        failed.diagnostic = "failed to queue roReq for '" + roID + "': " + seq.error().message;
        session_->postEvent(std::move(failed));
    }
}

void Profile2Responder::completeResync(const SessionEvent& event) {
    const auto it = pendingResync_.find(event.seq);
    const std::string roID = it->second;
    pendingResync_.erase(it);
    resyncing_.erase(roID);

    if (event.message.type() == codec::MessageType::RoList) {
        // Rebuild wholesale; nothing is replayed (spec: rebuild, don't replay).
        if (Result<void> applied = model_.apply(event.message); !applied.ok()) {
            SessionEvent failed;
            failed.type = SessionEventType::ProtocolError;
            failed.channel = event.channel;
            failed.diagnostic =
                "roList rebuild for '" + roID + "' failed: " + applied.error().message;
            session_->postEvent(std::move(failed));
        }
        return;
    }
    // NACK branch: the NCS no longer has the RO, so neither should we.
    model_.erase(roID);
}

Result<Seq> Profile2Responder::sendElementStat(const ElementStatusReport& report) {
    if (sink_ == nullptr) {
        Result<void> recorded = model_.setStatus(report.element, report.roID, report.storyID,
                                                 report.itemID, report.status);
        if (!recorded.ok()) {
            return std::move(recorded).error();
        }
    }
    ElementStatusReport stamped = report;
    if (stamped.time.empty()) {
        stamped.time = formatSpecTime(clock_->wallClock());
    }
    return session_->enqueue(buildRoElementStat(stamped));
}

std::optional<ElementStatusReport> Profile2Responder::nextStatus() {
    if (statusEvents_.empty()) {
        return std::nullopt;
    }
    ElementStatusReport report = std::move(statusEvents_.front());
    statusEvents_.pop_front();
    return report;
}

bool Profile2Responder::resyncing(std::string_view roID) const {
    return resyncing_.contains(std::string{roID});
}

// ---- Profile2NcsResponder (NCS / incs side) ----------------------------

Profile2NcsResponder::Profile2NcsResponder(PeerSession* session, IClock* clock)
    : session_(session), clock_(clock) {}

bool Profile2NcsResponder::handle(const SessionEvent& event) {
    if (event.type != SessionEventType::MessageReceived) {
        return false;
    }
    switch (event.message.type()) {
        case codec::MessageType::RoReq: {
            const std::string roID = messageRoID(event.message);
            std::optional<codec::Message> list;
            if (source_ != nullptr) {
                list = source_->runningOrder(roID);
            }
            if (list.has_value()) {
                codec::echoMessageID(event.message, *list);
                // In queue order, not as an overtaking reply: the snapshot
                // already contains every construction message queued ahead
                // of it, so it must reach the device *after* them — sent
                // first, the rebuild would have those diffs applied a second
                // time on top of it. roList awaits no ack of its own, and
                // the device acks the queued messages past its pending
                // roReq, so the queue still drains.
                (void)session_->enqueue(event.channel, std::move(*list));
            } else {
                codec::Message ack = buildRoAck(roID, "NACK: unknown roID '" + roID + "'");
                codec::echoMessageID(event.message, ack);
                (void)session_->enqueueReply(event.channel, std::move(ack));
            }
            return true;
        }
        case codec::MessageType::RoElementStat: {
            Result<ElementStatusReport> report = parseRoElementStat(event.message);
            const std::string roID = messageRoID(event.message);
            codec::Message ack = buildRoAck(
                roID, report.ok() ? std::string{"OK"} : "NACK: " + report.error().message);
            codec::echoMessageID(event.message, ack);
            (void)session_->enqueueReply(event.channel, std::move(ack));
            if (report.ok()) {
                statusEvents_.push_back(std::move(report).value());
            }
            return true;
        }
        default:
            return false;
    }
}

Result<Seq> Profile2NcsResponder::sendElementStat(const ElementStatusReport& report) {
    ElementStatusReport stamped = report;
    if (stamped.time.empty()) {
        stamped.time = formatSpecTime(clock_->wallClock());
    }
    return session_->enqueue(buildRoElementStat(stamped));
}

std::optional<ElementStatusReport> Profile2NcsResponder::nextStatus() {
    if (statusEvents_.empty()) {
        return std::nullopt;
    }
    ElementStatusReport report = std::move(statusEvents_.front());
    statusEvents_.pop_front();
    return report;
}

}  // namespace imos::profiles
