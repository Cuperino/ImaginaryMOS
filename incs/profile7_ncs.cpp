// SPDX-FileCopyrightText: 2026 Javier O. Cordero Pérez
// SPDX-License-Identifier: Apache-2.0

#include <incs/profile7_ncs.h>

#include <algorithm>
#include <charconv>
#include <utility>

#include <imos/codec/gen/views.h>
#include <imos/codec/story_body.h>
#include <imos/profiles/profile2.h>
#include <incs/running_order_author.h>

namespace incs {

// ---- SingleHolderLeasePolicy ------------------------------------------------

imos::Result<void> SingleHolderLeasePolicy::acquire(std::string_view roID, std::string_view storyID,
                                                    std::string_view holder, int durationSeconds,
                                                    imos::MonotonicTime now) {
    const auto key = std::make_pair(std::string{roID}, std::string{storyID});
    const auto it = leases_.find(key);

    if (it != leases_.end()) {
        // Lease exists — check if held by same holder or if expired
        if (it->second.holder != holder && it->second.expiry > now) {
            // Held by different holder and not expired
            return imos::makeError(imos::ErrorCode::WrongState,
                                   "story is locked by " + it->second.holder);
        }
        // Same holder (renewal) or expired — update expiry
        it->second.holder = std::string{holder};
        it->second.expiry = now + static_cast<std::int64_t>(durationSeconds) * 1000;
    } else {
        // New lease
        LeaseEntry entry;
        entry.holder = std::string{holder};
        entry.expiry = now + static_cast<std::int64_t>(durationSeconds) * 1000;
        leases_[key] = entry;
    }
    return {};
}

void SingleHolderLeasePolicy::release(std::string_view roID, std::string_view storyID,
                                      std::string_view holder) {
    const auto key = std::make_pair(std::string{roID}, std::string{storyID});
    const auto it = leases_.find(key);
    if (it != leases_.end() && it->second.holder == holder) {
        leases_.erase(it);
    }
}

// ---- Profile7NcsResponder ---------------------------------------------------

Profile7NcsResponder::Profile7NcsResponder(RunningOrderAuthor* author, ILeasePolicy* leases,
                                           const imos::profiles::Profile0Responder* peer0,
                                           imos::PeerSession* session, imos::IClock* clock)
    : author_(author), leases_(leases), peer0_(peer0), session_(session), clock_(clock) {}

void Profile7NcsResponder::setCapabilityPolicy(CapabilityPolicy policy) noexcept {
    capabilityPolicy_ = policy;
}

bool Profile7NcsResponder::handle(const imos::SessionEvent& event) {
    if (event.type != imos::SessionEventType::MessageReceived ||
        event.message.type() != imos::codec::MessageType::RoReqStoryAction) {
        return false;
    }

    const imos::codec::Message& message = event.message;
    const imos::codec::XmlNode* body = message.body();
    if (body == nullptr) {
        return true;  // malformed; let the session error it
    }

    // Terminal-reply helper: build a "NACK: <reason>" roAck echoing this
    // request's messageID and enqueue it on the inbound channel.
    const auto sendNack = [&](std::string_view roID, std::string_view reason) {
        imos::codec::Message ack = imos::profiles::buildRoAck(roID, "NACK: " + std::string{reason});
        imos::codec::echoMessageID(message, ack);
        (void)session_->enqueueReply(event.channel, std::move(ack));
    };

    // Parse attributes
    std::optional<std::string_view> operation = body->attribute("operation");
    std::optional<std::string_view> leaseLock = body->attribute("leaseLock");
    std::optional<std::string_view> username = body->attribute("username");

    if (!operation.has_value()) {
        sendNack("", "missing operation");
        return true;
    }

    imos::profiles::StoryAction action = imos::profiles::StoryAction::Move;
    if (*operation == "NEW") {
        action = imos::profiles::StoryAction::New;
    } else if (*operation == "UPDATE") {
        action = imos::profiles::StoryAction::Update;
    } else if (*operation == "DELETE") {
        action = imos::profiles::StoryAction::Delete;
    } else if (*operation == "MOVE") {
        action = imos::profiles::StoryAction::Move;
    } else {
        sendNack("", "unknown operation");
        return true;
    }

    // Check capability gating: does the peer advertise Profile 7?
    if (peer0_ != nullptr && capabilityPolicy_ == CapabilityPolicy::Strict) {
        const imos::profiles::PeerMachineInfo* peer = peer0_->peerMachineInfo();
        if (peer == nullptr || !peer->info.profiles[7]) {
            sendNack("", "Profile 7 not advertised by peer");
            return true;
        }
        // Lenient policy (the default) accepts even when the peer never
        // advertised Profile 7; the action is still recorded in the audit trail.
    }

    // Parse leaseLock and username
    int leaseSeconds = 0;
    if (leaseLock.has_value()) {
        (void)std::from_chars(leaseLock->data(), leaseLock->data() + leaseLock->size(),
                              leaseSeconds);
    }
    std::string user{username.value_or(std::string_view{})};

    // Parse the wrapped roStorySend
    imos::profiles::StoryActionRequest req;
    req.operation = action;
    req.username = user;
    if (leaseSeconds > 0) {
        req.leaseSeconds = leaseSeconds;
    }

    const imos::codec::XmlNode* roStorySend = body->firstChildElement("roStorySend");
    if (roStorySend == nullptr) {
        sendNack("", "missing roStorySend");
        return true;
    }

    req.roID = roStorySend->childText("roID");
    if (req.roID.empty()) {
        sendNack("", "missing roID");
        return true;
    }

    // Parse operation-specific payload
    std::vector<imos::codec::XmlNode> newStories;  // NEW: every <story>, in order
    switch (action) {
        case imos::profiles::StoryAction::New: {
            if (const imos::codec::XmlNode* target =
                    roStorySend->firstChildElement("element_target")) {
                req.beforeStoryID = target->childText("storyID");
            }
            if (const imos::codec::XmlNode* source =
                    roStorySend->firstChildElement("element_source")) {
                for (const imos::codec::XmlNode& child : source->children) {
                    if (child.isElement("story")) {
                        if (!req.story.has_value()) {
                            req.story = child;
                        }
                        newStories.push_back(child);
                    }
                }
            }
            break;
        }
        case imos::profiles::StoryAction::Update: {
            req.storyID = roStorySend->childText("storyID");
            req.storySlug = roStorySend->childText("storySlug");
            req.storyNum = roStorySend->childText("storyNum");
            if (const imos::codec::XmlNode* sb = roStorySend->firstChildElement("storyBody")) {
                req.storyBody = *sb;
            }
            if (const imos::codec::XmlNode* mem =
                    roStorySend->firstChildElement("mosExternalMetadata")) {
                req.agencyMetadata = *mem;
            }
            break;
        }
        case imos::profiles::StoryAction::Delete: {
            req.storyID = roStorySend->childText("storyID");
            break;
        }
        case imos::profiles::StoryAction::Move: {
            if (const imos::codec::XmlNode* target =
                    roStorySend->firstChildElement("element_target")) {
                req.beforeStoryID = target->childText("storyID");
            }
            if (const imos::codec::XmlNode* source =
                    roStorySend->firstChildElement("element_source")) {
                for (const imos::codec::XmlNode& child : source->children) {
                    if (child.isElement("storyID")) {
                        req.storyIDs.push_back(child.text());
                    }
                }
            }
            break;
        }
    }

    // Lease identity. The holder is the requesting MOS — stable across its
    // requests, so the follow-up action the spec requires inside the lease
    // (§3.9.1 note 5) renews it instead of colliding with it. The lease
    // covers the stories the request names: the one being updated/deleted,
    // or every story being moved. A NEW story has no ID until the NCS
    // assigns one, so its lease is taken once the story exists.
    const bool leaseRequested = leaseSeconds > 0 && leases_ != nullptr;
    const std::string holder{message.mosID().value_or(std::string_view{})};
    std::vector<std::string> leaseStories;
    if (action == imos::profiles::StoryAction::Move) {
        leaseStories = req.storyIDs;
    } else if (!req.storyID.empty()) {
        leaseStories.push_back(req.storyID);
    }

    // Records one resolved action in the audit trail (accepted iff no reason).
    const auto recordAudit = [&](std::string_view storyID, std::string_view reason) {
        StoryActionAudit audit;
        audit.roID = req.roID;
        audit.storyID = std::string{storyID};
        audit.username = user;
        audit.operation = action;
        audit.accepted = reason.empty();
        audit.detail = std::string{reason};
        audits_.push_back(std::move(audit));
    };

    // Try to acquire lease if requested
    if (leaseRequested) {
        for (std::size_t i = 0; i < leaseStories.size(); ++i) {
            imos::Result<void> leaseResult =
                leases_->acquire(req.roID, leaseStories[i], holder, leaseSeconds, clock_->now());
            if (leaseResult.ok()) {
                continue;
            }
            // Another holder owns one of the stories: refuse the request
            // as a whole. Leases this holder already has on the others
            // stay — they are its own, and they simply run out.
            sendNack(req.roID, leaseResult.error().message);
            recordAudit(req.storyID, leaseResult.error().message);
            return true;
        }
    }

    // Apply the action
    std::string nackReason;
    std::string assignedStoryID;
    switch (action) {
        case imos::profiles::StoryAction::New: {
            if (imos::Result<std::string> result = applyNew(req, newStories); result.ok()) {
                assignedStoryID = std::move(result).value();
            } else {
                nackReason = result.error().message;
            }
            break;
        }
        case imos::profiles::StoryAction::Update: {
            if (imos::Result<void> result = applyUpdate(req); !result.ok()) {
                nackReason = result.error().message;
            }
            break;
        }
        case imos::profiles::StoryAction::Delete: {
            if (imos::Result<void> result = applyDelete(req); !result.ok()) {
                nackReason = result.error().message;
            }
            break;
        }
        case imos::profiles::StoryAction::Move: {
            if (imos::Result<void> result = applyMove(req); !result.ok()) {
                nackReason = result.error().message;
            }
            break;
        }
    }

    const bool accepted = nackReason.empty();
    const bool isNew = action == imos::profiles::StoryAction::New;
    // On success: NEW answers with the assigned storyID, everything else "OK".
    const std::string& resolvedStoryID = (accepted && isNew) ? assignedStoryID : req.storyID;

    // Build and send roAck
    std::string status = accepted ? (isNew ? assignedStoryID : "OK") : ("NACK: " + nackReason);
    imos::codec::Message ack = imos::profiles::buildRoAck(req.roID, status);
    imos::codec::echoMessageID(message, ack);
    (void)session_->enqueueReply(event.channel, std::move(ack));

    recordAudit(resolvedStoryID, nackReason);

    // A refused action leaves the holder's lease in place: the holder is the
    // same on its retry, so it re-acquires freely, and releasing here would
    // drop a lease an earlier accepted request legitimately holds.
    if (accepted && isNew && leaseRequested) {
        (void)leases_->acquire(req.roID, assignedStoryID, holder, leaseSeconds, clock_->now());
    }

    return true;
}

imos::Result<std::string> Profile7NcsResponder::applyNew(
    const imos::profiles::StoryActionRequest& req,
    const std::vector<imos::codec::XmlNode>& stories) {
    if (stories.empty()) {
        return imos::makeError(imos::ErrorCode::InvalidArgument,
                               "NEW missing story in element_source");
    }
    // Resolve every storyID first (an empty one asks the NCS to assign it)
    // and refuse the whole request on a clash, so a NEW either inserts all
    // of its stories or none.
    std::vector<std::string> ids;
    ids.reserve(stories.size());
    for (const imos::codec::XmlNode& story : stories) {
        std::string storyID = story.childText("storyID");
        if (storyID.empty()) {
            do {
                storyID = nextGeneratedStoryID(req.roID);
            } while (std::find(ids.begin(), ids.end(), storyID) != ids.end());
        } else if (std::find(ids.begin(), ids.end(), storyID) != ids.end() ||
                   author_->desired().story(req.roID, storyID) != nullptr) {
            return imos::makeError(imos::ErrorCode::InvalidArgument,
                                   "storyID '" + storyID + "' already exists");
        }
        ids.push_back(std::move(storyID));
    }

    for (std::size_t i = 0; i < stories.size(); ++i) {
        const imos::codec::XmlNode& story = stories[i];
        // The running-order story is everything the MOS sent except the
        // script: its items and metadata ride along; the body goes to the
        // story-send store.
        imos::codec::XmlNode builtStory = incs::RunningOrderAuthor::makeStory(
            ids[i], story.childText("storySlug"), story.childText("storyNum"));
        for (const imos::codec::XmlNode& child : story.children) {
            if (child.kind == imos::codec::XmlNodeKind::Element && child.name != "storyID" &&
                child.name != "storySlug" && child.name != "storyNum" &&
                child.name != "storyBody") {
                builtStory.children.push_back(child);
            }
        }
        // Each lands before the same target, so they keep the order sent.
        if (imos::Result<void> result =
                author_->insertStory(req.roID, builtStory, req.beforeStoryID);
            !result.ok()) {
            return std::move(result).error();
        }
        if (const imos::codec::XmlNode* sb = story.firstChildElement("storyBody")) {
            if (imos::Result<void> result = author_->setStoryBody(req.roID, ids[i], *sb);
                !result.ok()) {
                return std::move(result).error();
            }
        }
    }
    if (imos::Result<std::vector<imos::Seq>> result = author_->commit(req.roID); !result.ok()) {
        return std::move(result).error();
    }
    return ids.front();
}

imos::Result<void> Profile7NcsResponder::applyUpdate(
    const imos::profiles::StoryActionRequest& req) {
    if (req.storyID.empty()) {
        return imos::makeError(imos::ErrorCode::InvalidArgument, "UPDATE missing storyID");
    }
    // UPDATE replaces the story with the header the MOS sent (slug/num carried
    // verbatim), then its body. The story's item references are not the
    // MOS's to drop by omission: when the new script embeds <storyItem>
    // references they define the story's items (an item the story already
    // has keeps its full running-order form, a new reference becomes an
    // item); a script that embeds none — plain text, or the agency form —
    // leaves the existing items as they are.
    imos::codec::XmlNode builtStory =
        incs::RunningOrderAuthor::makeStory(req.storyID, req.storySlug, req.storyNum);
    if (const imos::codec::XmlNode* current = author_->desired().story(req.roID, req.storyID)) {
        std::vector<const imos::codec::XmlNode*> embedded;
        if (req.storyBody.has_value()) {
            for (const imos::codec::XmlNode& child : req.storyBody->children) {
                if (child.isElement("storyItem")) {
                    embedded.push_back(&child);
                }
            }
        }
        if (embedded.empty()) {
            for (const imos::codec::XmlNode& child : current->children) {
                if (child.isElement("item")) {
                    builtStory.children.push_back(child);
                }
            }
        } else {
            for (const imos::codec::XmlNode* reference : embedded) {
                const std::string itemID = reference->childText("itemID");
                const imos::codec::XmlNode* existing = nullptr;
                for (const imos::codec::XmlNode& child : current->children) {
                    if (child.isElement("item") && child.childText("itemID") == itemID) {
                        existing = &child;
                        break;
                    }
                }
                if (existing != nullptr) {
                    builtStory.children.push_back(*existing);
                } else {
                    imos::codec::XmlNode item = *reference;
                    item.name = "item";
                    builtStory.children.push_back(std::move(item));
                }
            }
        }
    }
    if (imos::Result<void> result = author_->replaceStory(req.roID, req.storyID, builtStory);
        !result.ok()) {
        return std::move(result).error();
    }
    // The agency (Read1stMEMasBody) form carries an empty storyBody flagged
    // Read1stMEMasBody plus the mosExternalMetadata that *is* the body; a
    // literal-body UPDATE carries the script inline (any trailing
    // mosExternalMetadata is ordinary story-level metadata, not the body).
    const bool isAgency = req.agencyMetadata.has_value() &&
                          (!req.storyBody.has_value() ||
                           req.storyBody->attribute("Read1stMEMasBody").value_or("") == "true");
    if (isAgency) {
        if (imos::Result<void> result =
                author_->setAgencyStoryBody(req.roID, req.storyID, *req.agencyMetadata);
            !result.ok()) {
            return std::move(result).error();
        }
    } else if (req.storyBody.has_value()) {
        if (imos::Result<void> result =
                author_->setStoryBody(req.roID, req.storyID, *req.storyBody);
            !result.ok()) {
            return std::move(result).error();
        }
    }

    // sendStory delivers the body to devices via the P4 echo.
    if (imos::Result<imos::Seq> result = author_->sendStory(req.roID, req.storyID); !result.ok()) {
        return std::move(result).error();
    }

    if (imos::Result<std::vector<imos::Seq>> result = author_->commit(req.roID); !result.ok()) {
        return std::move(result).error();
    }

    return {};
}

imos::Result<void> Profile7NcsResponder::applyDelete(
    const imos::profiles::StoryActionRequest& req) {
    if (req.storyID.empty()) {
        return imos::makeError(imos::ErrorCode::InvalidArgument, "DELETE missing storyID");
    }

    if (imos::Result<void> result = author_->removeStory(req.roID, req.storyID); !result.ok()) {
        return std::move(result).error();
    }

    if (imos::Result<std::vector<imos::Seq>> result = author_->commit(req.roID); !result.ok()) {
        return std::move(result).error();
    }

    return {};
}

imos::Result<void> Profile7NcsResponder::applyMove(const imos::profiles::StoryActionRequest& req) {
    if (req.storyIDs.empty()) {
        return imos::makeError(imos::ErrorCode::InvalidArgument, "MOVE missing source storyIDs");
    }

    // One source ID maps to moveStory; several ride in one moveStories element_source.
    imos::Result<void> moved =
        req.storyIDs.size() == 1
            ? author_->moveStory(req.roID, req.storyIDs.front(), req.beforeStoryID)
            : author_->moveStories(req.roID, req.storyIDs, req.beforeStoryID);
    if (!moved.ok()) {
        return std::move(moved).error();
    }

    if (imos::Result<std::vector<imos::Seq>> result = author_->commit(req.roID); !result.ok()) {
        return std::move(result).error();
    }

    return {};
}

std::string Profile7NcsResponder::nextGeneratedStoryID(std::string_view roID) {
    // Generate P7-<n> format, checking for collisions against the desired RO
    while (true) {
        std::string candidate = "P7-" + std::to_string(nextGeneratedStoryIdCounter_++);
        // Check if it exists in the desired model
        const imos::codec::XmlNode* ro = author_->desired().runningOrder(roID);
        if (ro == nullptr) {
            return candidate;  // RO doesn't exist yet, any ID works
        }
        bool exists = false;
        for (const imos::codec::XmlNode& child : ro->children) {
            if (child.isElement("story") && child.childText("storyID") == candidate) {
                exists = true;
                break;
            }
        }
        if (!exists) {
            return candidate;
        }
        // Collision, try next
    }
}

std::optional<StoryActionAudit> Profile7NcsResponder::nextAudit() {
    if (audits_.empty()) {
        return std::nullopt;
    }
    StoryActionAudit audit = std::move(audits_.front());
    audits_.pop_front();
    return audit;
}

}  // namespace incs
