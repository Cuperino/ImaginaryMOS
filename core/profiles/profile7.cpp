// SPDX-FileCopyrightText: 2026 Javier O. Cordero Pérez
// SPDX-License-Identifier: Apache-2.0

#include <imos/profiles/profile7.h>

#include <utility>

#include <imos/codec/gen/views.h>
#include <imos/profiles/profile2.h>

namespace imos::profiles {

namespace {

std::string operationName(StoryAction op) {
    switch (op) {
        case StoryAction::New:
            return "NEW";
        case StoryAction::Update:
            return "UPDATE";
        case StoryAction::Delete:
            return "DELETE";
        case StoryAction::Move:
            return "MOVE";
    }
    return "";
}

}  // namespace

Profile7Requester::Profile7Requester(PeerSession* session) : session_(session) {}

Result<Seq> Profile7Requester::requestStoryAction(const StoryActionRequest& request) {
    codec::Message message = codec::makeMessage(codec::MessageType::RoReqStoryAction);
    if (codec::XmlNode* body = message.body()) {
        codec::view::RoReqStoryAction view{*body};
        view.setAttrOperation(operationName(request.operation));
        if (request.leaseSeconds.has_value()) {
            view.setAttrLeaseLock(std::to_string(*request.leaseSeconds));
        }
        if (!request.username.empty()) {
            view.setAttrUsername(request.username);
        }

        codec::XmlNode& roStorySend = view.ensureRoStorySend();
        roStorySend.appendElement("roID").setText(request.roID);

        switch (request.operation) {
            case StoryAction::New: {
                // NEW uses element_target? / element_source with <story> children
                if (!request.beforeStoryID.empty()) {
                    roStorySend.appendElement("element_target")
                        .appendElement("storyID")
                        .setText(request.beforeStoryID);
                }
                if (request.story.has_value()) {
                    roStorySend.appendElement("element_source").children.push_back(*request.story);
                }
                break;
            }
            case StoryAction::Update: {
                // UPDATE uses flat RoStorySend: roID, storyID, storySlug?,
                // storyNum?, storyBody
                roStorySend.appendElement("storyID").setText(request.storyID);
                if (!request.storySlug.empty()) {
                    roStorySend.appendElement("storySlug").setText(request.storySlug);
                }
                if (!request.storyNum.empty()) {
                    roStorySend.appendElement("storyNum").setText(request.storyNum);
                }
                codec::XmlNode& storyBody = roStorySend.appendElement("storyBody");
                if (request.storyBody.has_value()) {
                    // Copy the storyBody element's children (paragraphs, etc.)
                    storyBody.children = request.storyBody->children;
                    for (const auto& attr : request.storyBody->attributes) {
                        storyBody.setAttribute(attr.name, attr.value);
                    }
                } else if (request.agencyMetadata.has_value()) {
                    // Set Read1stMEMasBody and append the metadata
                    storyBody.setAttribute("Read1stMEMasBody", "true");
                    roStorySend.appendElement("mosExternalMetadata").children =
                        request.agencyMetadata->children;
                }
                break;
            }
            case StoryAction::Delete: {
                // DELETE uses flat RoStorySend with empty storyBody
                roStorySend.appendElement("storyID").setText(request.storyID);
                roStorySend.appendElement("storyBody");
                break;
            }
            case StoryAction::Move: {
                // MOVE uses element_target? / element_source with <storyID> children
                if (!request.beforeStoryID.empty()) {
                    roStorySend.appendElement("element_target")
                        .appendElement("storyID")
                        .setText(request.beforeStoryID);
                }
                codec::XmlNode& source = roStorySend.appendElement("element_source");
                for (const auto& id : request.storyIDs) {
                    source.appendElement("storyID").setText(id);
                }
                break;
            }
        }
    }

    Result<Seq> seq = session_->enqueue(std::move(message));
    if (seq.ok()) {
        pending_.emplace(seq.value(), request.operation);
    }
    return seq;
}

bool Profile7Requester::handle(const SessionEvent& event) {
    if (event.type != SessionEventType::AckReceived ||
        event.message.type() != codec::MessageType::RoAck) {
        return false;
    }
    const auto it = pending_.find(event.seq);
    if (it == pending_.end()) {
        return false;
    }
    const StoryAction operation = it->second;
    pending_.erase(it);

    StoryActionResult result;
    result.seq = event.seq;
    result.roStatus = roAckStatusText(event.message);
    if (operation == StoryAction::New) {
        // Spec §3.9.1: a successful NEW answers with the storyID *in*
        // roStatus, so "not OK" cannot mean failure here; the only failure
        // marker the exchange leaves is the NACK keyword (or an empty
        // status, which names no story).
        result.ok = !result.roStatus.empty() && !result.roStatus.starts_with("NACK");
        if (result.ok) {
            result.assignedStoryID = result.roStatus;
        }
    } else {
        result.ok = !roAckIsNack(event.message);
    }

    results_.push_back(std::move(result));
    return true;
}

std::optional<StoryActionResult> Profile7Requester::nextResult() {
    if (results_.empty()) {
        return std::nullopt;
    }
    StoryActionResult result = std::move(results_.front());
    results_.pop_front();
    return result;
}

}  // namespace imos::profiles
