// SPDX-FileCopyrightText: 2026 Javier O. Cordero Pérez
// SPDX-License-Identifier: Apache-2.0

#include <imos/profiles/profile4.h>

#include <utility>

#include <imos/codec/gen/views.h>

namespace imos::profiles {

namespace {

/// The <ro> content model (headers only): what roListAll may carry. Story
/// and item detail is deliberately not part of it.
constexpr std::string_view kRoHeaderFields[] = {
    "roID", "roSlug", "roChannel", "roEdStart", "roEdDur", "roTrigger", "mosExternalMetadata",
};

bool isRoHeaderField(const codec::XmlNode& node) {
    if (node.kind != codec::XmlNodeKind::Element) {
        return false;
    }
    for (const std::string_view name : kRoHeaderFields) {
        if (node.name == name) {
            return true;
        }
    }
    return false;
}

}  // namespace

// ---- Profile4Responder (device / imos side) ----------------------------

Profile4Responder::Profile4Responder(PeerSession* session, Profile2Responder* profile2)
    : session_(session), profile2_(profile2) {}

bool Profile4Responder::handle(const SessionEvent& event) {
    switch (event.type) {
        case SessionEventType::MessageReceived:
            if (event.message.type() != codec::MessageType::RoStorySend) {
                return false;
            }
            handleStorySend(event);
            return true;
        case SessionEventType::AckReceived:
            if (event.channel != Channel::Ro ||
                pendingListAll_.find(event.seq) == pendingListAll_.end()) {
                return false;
            }
            completeListAll(event);
            return true;
        default:
            return false;
    }
}

void Profile4Responder::handleStorySend(const SessionEvent& event) {
    const codec::Message& message = event.message;
    const codec::XmlNode* body = message.body();
    const std::string roID = body ? body->childText("roID") : std::string{};
    const std::string storyID = body ? body->childText("storyID") : std::string{};

    const auto nack = [&](std::string detail) {
        codec::Message ack = buildRoAck(roID, "NACK: " + std::move(detail));
        codec::echoMessageID(message, ack);
        (void)session_->enqueueReply(event.channel, std::move(ack));
    };

    if (roID.empty() || storyID.empty()) {
        nack("roStorySend is missing roID or storyID");
        return;
    }
    // Stories reference an RO the device should already hold (the spec sends
    // RO construction before roStorySend). With a linked Profile 2 model, an
    // unknown roID is the §3.3 "assume a prior communication error" trigger.
    if (profile2_ != nullptr && profile2_->modelMode() && !profile2_->model().has(roID)) {
        nack("unknown roID '" + roID + "'");
        profile2_->noteUnknownRunningOrder(roID, event.channel,
                                           "roStorySend for story '" + storyID + "'");
        return;
    }
    Result<codec::ResolvedStoryBody> resolved = codec::resolveStoryBody(message);
    if (!resolved.ok()) {
        nack(resolved.error().message);
        return;
    }

    // Replace-on-resend: the stored message is the retained original, so the
    // sink's view and later story() queries see identical verbatim content.
    codec::Message& stored = stories_[roID][storyID] = message;

    if (sink_ != nullptr) {
        StoryView view;
        view.roID = roID;
        view.storyID = storyID;
        view.storySlug = stored.body()->childText("storySlug");
        view.storyNum = stored.body()->childText("storyNum");
        view.body = codec::resolveStoryBody(stored).value();  // resolved above
        view.message = &stored;
        if (Result<void> accepted = sink_->onStory(view); !accepted.ok()) {
            eraseStory(roID, storyID);  // rejected content is not retained
            nack(accepted.error().message);
            return;
        }
    }

    codec::Message ack = buildRoAck(roID, "OK");
    codec::echoMessageID(message, ack);
    (void)session_->enqueueReply(event.channel, std::move(ack));
}

Result<Seq> Profile4Responder::requestAllRunningOrders(bool hydrate) {
    Result<Seq> seq = session_->enqueue(codec::makeMessage(codec::MessageType::RoReqAll));
    if (seq.ok()) {
        pendingListAll_.emplace(seq.value(), hydrate);
    }
    return seq;
}

void Profile4Responder::completeListAll(const SessionEvent& event) {
    const auto it = pendingListAll_.find(event.seq);
    const bool hydrate = it->second;
    pendingListAll_.erase(it);

    if (event.message.type() != codec::MessageType::RoListAll) {
        SessionEvent failed;
        failed.type = SessionEventType::ProtocolError;
        failed.channel = event.channel;
        failed.diagnostic = "roReqAll was answered with '" +
                            std::string{event.message.info().wireName} + "', not roListAll";
        session_->postEvent(std::move(failed));
        return;
    }

    headers_.clear();  // the reply is the complete enumeration
    if (const codec::XmlNode* body = event.message.body()) {
        for (const codec::XmlNode& child : body->children) {
            if (!child.isElement("ro")) {
                continue;
            }
            const std::string roID = child.childText("roID");
            if (!roID.empty()) {
                headers_.insert_or_assign(roID, child);
            }
        }
    }
    if (hydrate && profile2_ != nullptr && profile2_->modelMode()) {
        // Cold-start convergence: fetch every listed RO through the Profile 2
        // rebuild machinery (roReq → roList upserts the model).
        for (const auto& [roID, header] : headers_) {
            profile2_->requestRunningOrder(roID);
        }
    }
}

std::vector<std::string> Profile4Responder::headerIDs() const {
    std::vector<std::string> ids;
    ids.reserve(headers_.size());
    for (const auto& [roID, header] : headers_) {
        ids.push_back(roID);
    }
    return ids;
}

const codec::XmlNode* Profile4Responder::header(std::string_view roID) const {
    const auto it = headers_.find(std::string{roID});
    return it == headers_.end() ? nullptr : &it->second;
}

const codec::Message* Profile4Responder::story(std::string_view roID,
                                               std::string_view storyID) const {
    const auto ro = stories_.find(std::string{roID});
    if (ro == stories_.end()) {
        return nullptr;
    }
    const auto story = ro->second.find(std::string{storyID});
    return story == ro->second.end() ? nullptr : &story->second;
}

std::vector<std::string> Profile4Responder::storyIDs(std::string_view roID) const {
    std::vector<std::string> ids;
    const auto ro = stories_.find(std::string{roID});
    if (ro == stories_.end()) {
        return ids;
    }
    ids.reserve(ro->second.size());
    for (const auto& [storyID, message] : ro->second) {
        ids.push_back(storyID);
    }
    return ids;
}

void Profile4Responder::eraseStories(std::string_view roID) {
    stories_.erase(std::string{roID});
}

void Profile4Responder::eraseStory(std::string_view roID, std::string_view storyID) {
    const auto ro = stories_.find(std::string{roID});
    if (ro == stories_.end()) {
        return;
    }
    ro->second.erase(std::string{storyID});
    if (ro->second.empty()) {
        stories_.erase(ro);
    }
}

// ---- Profile4NcsResponder (NCS / incs side) ----------------------------

Profile4NcsResponder::Profile4NcsResponder(PeerSession* session) : session_(session) {}

bool Profile4NcsResponder::handle(const SessionEvent& event) {
    if (event.type != SessionEventType::MessageReceived ||
        event.message.type() != codec::MessageType::RoReqAll) {
        return false;
    }
    codec::Message list = codec::makeMessage(codec::MessageType::RoListAll);
    if (codec::XmlNode* body = list.body(); body != nullptr && store_ != nullptr) {
        for (const std::string& roID : store_->runningOrderIDs()) {
            const codec::XmlNode* stored = store_->runningOrder(roID);
            if (stored == nullptr) {
                continue;
            }
            // Headers only: the <ro> content model — story/item detail (and
            // fields it does not allow, e.g. macroIn/macroOut) is stripped;
            // header metadata rides through verbatim.
            codec::XmlNode& ro = body->appendElement("ro");
            for (const codec::XmlNode& child : stored->children) {
                if (isRoHeaderField(child)) {
                    ro.children.push_back(child);
                }
            }
        }
    }
    codec::echoMessageID(event.message, list);
    (void)session_->enqueueReply(event.channel, std::move(list));
    return true;
}

}  // namespace imos::profiles
