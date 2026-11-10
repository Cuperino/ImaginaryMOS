// SPDX-FileCopyrightText: 2026 Javier O. Cordero Pérez
// SPDX-License-Identifier: Apache-2.0

#include <imos/profiles/profile5.h>

#include <utility>

#include <imos/codec/gen/views.h>
#include <imos/time_format.h>

namespace imos::profiles {

// ---- shared vocabulary --------------------------------------------------

std::string_view ctrlCommandName(CtrlCommand command) noexcept {
    switch (command) {
        case CtrlCommand::Ready:
            return "READY";
        case CtrlCommand::Execute:
            return "EXECUTE";
        case CtrlCommand::Pause:
            return "PAUSE";
        case CtrlCommand::Stop:
            return "STOP";
        case CtrlCommand::Signal:
            return "SIGNAL";
    }
    return "";
}

std::optional<CtrlCommand> ctrlCommandFromName(std::string_view name) noexcept {
    if (name == "READY") {
        return CtrlCommand::Ready;
    }
    if (name == "EXECUTE") {
        return CtrlCommand::Execute;
    }
    if (name == "PAUSE") {
        return CtrlCommand::Pause;
    }
    if (name == "STOP") {
        return CtrlCommand::Stop;
    }
    if (name == "SIGNAL") {
        return CtrlCommand::Signal;
    }
    return std::nullopt;
}

Result<CtrlRequest> parseRoCtrl(const codec::Message& message) {
    if (message.type() != codec::MessageType::RoCtrl) {
        return makeError(ErrorCode::InvalidArgument, "not a roCtrl message");
    }
    const codec::XmlNode* body = message.body();
    if (body == nullptr) {
        return makeError(ErrorCode::InvalidArgument, "roCtrl has no body");
    }
    CtrlRequest request;
    request.roID = body->childText("roID");
    request.storyID = body->childText("storyID");
    request.itemID = body->childText("itemID");
    if (request.roID.empty()) {
        return makeError(ErrorCode::ProtocolViolation, "roCtrl is missing roID");
    }
    if (request.storyID.empty() && !request.itemID.empty()) {
        return makeError(ErrorCode::ProtocolViolation,
                         "roCtrl has itemID '" + request.itemID +
                             "' without a storyID — inconsistent scope shape");
    }
    const std::string command = body->childText("command");
    const std::optional<CtrlCommand> parsed = ctrlCommandFromName(command);
    if (!parsed.has_value()) {
        return makeError(ErrorCode::ProtocolViolation, "roCtrl command '" + command +
                                                           "' is not READY, EXECUTE, PAUSE, "
                                                           "STOP, or SIGNAL");
    }
    request.command = *parsed;
    request.scope = request.storyID.empty()  ? CtrlScope::RunningOrder
                    : request.itemID.empty() ? CtrlScope::Story
                                             : CtrlScope::Item;
    return request;
}

Result<ItemCue> parseRoItemCue(const codec::Message& message) {
    if (message.type() != codec::MessageType::RoItemCue) {
        return makeError(ErrorCode::InvalidArgument, "not a roItemCue message");
    }
    const codec::XmlNode* body = message.body();
    if (body == nullptr) {
        return makeError(ErrorCode::InvalidArgument, "roItemCue has no body");
    }
    ItemCue cue;
    cue.mosID = body->childText("mosID");
    cue.roID = body->childText("roID");
    cue.storyID = body->childText("storyID");
    cue.itemID = body->childText("itemID");
    cue.roEventType = body->childText("roEventType");
    cue.roEventTime = body->childText("roEventTime");
    if (Result<WallClockTime> parsed = parseSpecTime(cue.roEventTime); parsed.ok()) {
        cue.eventTime = parsed.value();
    }
    for (const codec::XmlNode& child : body->children) {
        if (child.isElement("mosExternalMetadata")) {
            cue.metadata.push_back(child);
        }
    }
    return cue;
}

codec::Message buildRoCtrl(std::string_view roID, std::string_view storyID, std::string_view itemID,
                           CtrlCommand command) {
    codec::Message message = codec::makeMessage(codec::MessageType::RoCtrl);
    if (codec::XmlNode* body = message.body()) {
        codec::view::RoCtrl view{*body};
        // All three ID elements are always present — the spec selects the
        // scope by leaving the lower levels *empty*, not absent.
        view.setRoID(roID);
        view.setStoryID(storyID);
        view.setItemID(itemID);
        view.setCommand(ctrlCommandName(command));
    }
    return message;
}

codec::Message buildRoItemCue(const ItemCue& cue) {
    codec::Message message = codec::makeMessage(codec::MessageType::RoItemCue);
    if (codec::XmlNode* body = message.body()) {
        codec::view::RoItemCue view{*body};
        view.setMosID(cue.mosID);
        view.setRoID(cue.roID);
        view.setStoryID(cue.storyID);
        view.setItemID(cue.itemID);
        view.setRoEventType(cue.roEventType);
        view.setRoEventTime(cue.roEventTime);
        for (const codec::XmlNode& block : cue.metadata) {
            body->children.push_back(block);  // verbatim; last in the model
        }
    }
    return message;
}

// ---- Profile5Responder (device / imos side) -----------------------------

Profile5Responder::Profile5Responder(PeerSession* session, Profile2Responder* profile2)
    : session_(session), profile2_(profile2) {}

bool Profile5Responder::handle(const SessionEvent& event) {
    if (event.type != SessionEventType::MessageReceived) {
        return false;
    }
    switch (event.message.type()) {
        case codec::MessageType::RoCtrl:
            handleCtrl(event);
            return true;
        case codec::MessageType::RoItemCue:
            handleItemCue(event);
            return true;
        default:
            return false;
    }
}

std::string Profile5Responder::validateTarget(const CtrlRequest& request) const {
    if (profile2_ == nullptr || !profile2_->modelMode()) {
        return {};  // no shadow model linked — the host owns RO state
    }
    const RunningOrderModel& model = profile2_->model();
    if (!model.has(request.roID)) {
        return "unknown roID '" + request.roID + "'";
    }
    if (request.scope == CtrlScope::RunningOrder) {
        return {};
    }
    if (model.story(request.roID, request.storyID) == nullptr) {
        return "unknown storyID '" + request.storyID + "' in RO '" + request.roID + "'";
    }
    if (request.scope == CtrlScope::Story) {
        return {};
    }
    if (model.item(request.roID, request.storyID, request.itemID) == nullptr) {
        return "unknown itemID '" + request.itemID + "' in story '" + request.storyID + "'";
    }
    return {};
}

void Profile5Responder::handleCtrl(const SessionEvent& event) {
    const codec::Message& message = event.message;
    const codec::XmlNode* body = message.body();
    const std::string roID = body ? body->childText("roID") : std::string{};

    const auto reply = [&](std::string_view roStatus) {
        codec::Message ack = buildRoAck(roID, roStatus);
        codec::echoMessageID(message, ack);
        (void)session_->enqueueReply(event.channel, std::move(ack));
    };

    Result<CtrlRequest> parsed = parseRoCtrl(message);
    if (!parsed.ok()) {
        reply("NACK: " + parsed.error().message);
        return;
    }
    CtrlRequest request = std::move(parsed).value();

    if (std::string missing = validateTarget(request); !missing.empty()) {
        // §3.3: a reference the shadow model cannot resolve means a prior
        // communication error — NACK, then rebuild the RO.
        reply("NACK: " + missing);
        profile2_->noteUnknownRunningOrder(
            request.roID, event.channel,
            "roCtrl " + std::string{ctrlCommandName(request.command)} + ": " + missing);
        return;
    }

    if (sink_ != nullptr) {
        request.message = &message;
        const Result<void> outcome = sink_->onCtrl(request);
        reply(outcome.ok() ? std::string{"OK"} : "NACK: " + outcome.error().message);
        return;
    }
    reply("OK");
    request.message = nullptr;
    pendingCtrl_.push_back(std::move(request));
}

void Profile5Responder::handleItemCue(const SessionEvent& event) {
    const codec::Message& message = event.message;
    Result<ItemCue> parsed = parseRoItemCue(message);
    if (!parsed.ok()) {
        return;  // cannot happen for a typed roItemCue; leave unacked noise alone
    }
    ItemCue cue = std::move(parsed).value();

    // A cue is a notification, not a command: always acked, never applied
    // to any model.
    codec::Message ack = buildRoAck(cue.roID, "OK");
    codec::echoMessageID(message, ack);
    (void)session_->enqueueReply(event.channel, std::move(ack));

    if (sink_ != nullptr) {
        cue.message = &message;
        sink_->onItemCue(cue);
        return;
    }
    cue.message = nullptr;
    cues_.push_back(std::move(cue));
}

Result<Seq> Profile5Responder::sendItemCue(const ItemCue& cue, std::string_view addresseeNcsID) {
    codec::Message message = buildRoItemCue(cue);
    if (!addresseeNcsID.empty()) {
        // Spec §3.7.2, parent-MOS form: the addressee's name rides the
        // envelope's <ncsID> slot (the session fills <mosID> with ours).
        message.setNcsID(addresseeNcsID);
    }
    return session_->enqueue(std::move(message));
}

std::optional<CtrlRequest> Profile5Responder::nextCtrl() {
    if (pendingCtrl_.empty()) {
        return std::nullopt;
    }
    CtrlRequest request = std::move(pendingCtrl_.front());
    pendingCtrl_.pop_front();
    return request;
}

std::optional<ItemCue> Profile5Responder::nextItemCue() {
    if (cues_.empty()) {
        return std::nullopt;
    }
    ItemCue cue = std::move(cues_.front());
    cues_.pop_front();
    return cue;
}

// ---- Profile5NcsResponder (NCS / incs side) -----------------------------

Profile5NcsResponder::Profile5NcsResponder(PeerSession* session) : session_(session) {}

Result<Seq> Profile5NcsResponder::sendCtrl(CtrlScope scope, std::string_view roID,
                                           std::string_view storyID, std::string_view itemID,
                                           CtrlCommand command) {
    if (roID.empty()) {
        return makeError(ErrorCode::InvalidArgument, "roCtrl needs a roID at every scope");
    }
    switch (scope) {
        case CtrlScope::RunningOrder:
            if (!storyID.empty() || !itemID.empty()) {
                return makeError(ErrorCode::InvalidArgument,
                                 "running-order scope takes no storyID or itemID");
            }
            break;
        case CtrlScope::Story:
            if (storyID.empty() || !itemID.empty()) {
                return makeError(ErrorCode::InvalidArgument,
                                 "story scope takes roID and storyID, no itemID");
            }
            break;
        case CtrlScope::Item:
            if (storyID.empty() || itemID.empty()) {
                return makeError(ErrorCode::InvalidArgument, "item scope takes all three IDs");
            }
            break;
    }
    Result<Seq> seq = session_->enqueue(buildRoCtrl(roID, storyID, itemID, command));
    if (seq.ok()) {
        pending_.insert(seq.value());
    }
    return seq;
}

Result<Seq> Profile5NcsResponder::sendItemCue(const ItemCue& cue) {
    Result<Seq> seq = session_->enqueue(buildRoItemCue(cue));
    if (seq.ok()) {
        pending_.insert(seq.value());
    }
    return seq;
}

bool Profile5NcsResponder::handle(const SessionEvent& event) {
    switch (event.type) {
        case SessionEventType::MessageReceived: {
            if (event.message.type() != codec::MessageType::RoItemCue) {
                return false;
            }
            Result<ItemCue> parsed = parseRoItemCue(event.message);
            if (!parsed.ok()) {
                return false;
            }
            ItemCue cue = std::move(parsed).value();
            codec::Message ack = buildRoAck(cue.roID, "OK");
            codec::echoMessageID(event.message, ack);
            (void)session_->enqueueReply(event.channel, std::move(ack));
            cues_.push_back(std::move(cue));
            return true;
        }
        case SessionEventType::AckReceived: {
            // Store sequence numbers are per channel; ours are all ro.
            const auto it =
                event.channel == Channel::Ro ? pending_.find(event.seq) : pending_.end();
            if (it == pending_.end()) {
                return false;
            }
            pending_.erase(it);
            CtrlResult result;
            result.seq = event.seq;
            result.roStatus = roAckStatusText(event.message);
            result.ok = !roAckIsNack(event.message);
            results_.push_back(std::move(result));
            return true;
        }
        case SessionEventType::DeliveryFailed: {
            const auto it =
                event.channel == Channel::Ro ? pending_.find(event.seq) : pending_.end();
            if (it == pending_.end()) {
                return false;
            }
            pending_.erase(it);
            CtrlResult result;
            result.seq = event.seq;
            result.ok = false;
            result.roStatus = "NACK: delivery failed (retries exhausted)";
            results_.push_back(std::move(result));
            return true;
        }
        default:
            return false;
    }
}

std::optional<CtrlResult> Profile5NcsResponder::nextResult() {
    if (results_.empty()) {
        return std::nullopt;
    }
    CtrlResult result = std::move(results_.front());
    results_.pop_front();
    return result;
}

std::optional<ItemCue> Profile5NcsResponder::nextItemCue() {
    if (cues_.empty()) {
        return std::nullopt;
    }
    ItemCue cue = std::move(cues_.front());
    cues_.pop_front();
    return cue;
}

}  // namespace imos::profiles
