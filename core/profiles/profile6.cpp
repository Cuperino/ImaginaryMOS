// SPDX-FileCopyrightText: 2026 Javier O. Cordero Pérez
// SPDX-License-Identifier: Apache-2.0

#include <imos/profiles/profile6.h>

#include <utility>

namespace imos::profiles {

Result<void> validateProfile6Identity(const Profile6Config& config, std::string_view selfID) {
    if (!config.enabled()) {
        return {};
    }
    if (Result<Fqid> self = parseFqid(selfID); !self.ok()) {
        return makeError(
            ErrorCode::InvalidArgument,
            "Profile 6 is enabled but the configured identity " + std::move(self).error().message);
    }
    return {};
}

Result<void> Profile6Classifier::configure(const Profile6Config& config, std::string_view selfID) {
    if (Result<void> valid = validateProfile6Identity(config, selfID); !valid.ok()) {
        return valid;
    }
    config_ = config;
    if (config.enabled()) {
        self_ = parseFqid(selfID).value();  // validated above
    } else {
        self_.reset();
    }
    return {};
}

Locality Profile6Classifier::classify(std::string_view mosID) const {
    if (!self_.has_value()) {
        return Locality::NotFqid;
    }
    return imos::classify(*self_, mosID);
}

std::vector<ClassifiedReference> Profile6Classifier::collectReferences(
    const codec::Message& message) const {
    std::vector<ClassifiedReference> references;
    if (!classifying()) {
        return references;
    }
    const codec::XmlNode* body = message.body();
    if (body == nullptr) {
        return references;
    }

    const auto collect = [&](const codec::XmlNode& item, std::string_view storyID) {
        ClassifiedReference reference;
        reference.storyID = storyID;
        reference.itemID = item.childText("itemID");
        reference.objID = item.childText("objID");
        reference.mosID = item.childText("mosID");
        reference.locality = classify(reference.mosID);
        references.push_back(std::move(reference));
    };
    const auto collectStory = [&](const codec::XmlNode& story) {
        const std::string storyID = story.childText("storyID");
        for (const codec::XmlNode& child : story.children) {
            if (child.isElement("item")) {
                collect(child, storyID);
            }
        }
    };

    switch (message.type()) {
        case codec::MessageType::RoCreate:
        case codec::MessageType::RoReplace:
            for (const codec::XmlNode& child : body->children) {
                if (child.isElement("story")) {
                    collectStory(child);
                }
            }
            break;
        case codec::MessageType::RoElementAction:
            // Only INSERT/REPLACE carry new content — the same rule the
            // Profile 2 object validation applies.
            if (const codec::XmlNode* source = body->firstChildElement("element_source")) {
                const codec::XmlNode* target = body->firstChildElement("element_target");
                const std::string targetStory =
                    target ? target->childText("storyID") : std::string{};
                for (const codec::XmlNode& child : source->children) {
                    if (child.isElement("story")) {
                        collectStory(child);
                    } else if (child.isElement("item")) {
                        collect(child, targetStory);
                    }
                }
            }
            break;
        case codec::MessageType::RoStorySend: {
            const std::string storyID = body->childText("storyID");
            if (const codec::XmlNode* storyBody = body->firstChildElement("storyBody")) {
                for (const codec::XmlNode& child : storyBody->children) {
                    if (child.isElement("storyItem")) {
                        collect(child, storyID);
                    }
                }
            }
            break;
        }
        default:
            break;
    }
    return references;
}

const ClassifiedReference* Profile6Classifier::firstForeign(
    const std::vector<ClassifiedReference>& references) {
    for (const ClassifiedReference& reference : references) {
        if (reference.locality == Locality::Foreign) {
            return &reference;
        }
    }
    return nullptr;
}

void Profile6Classifier::announceForeign(PeerSession& session, Channel channel,
                                         std::string_view roID,
                                         const std::vector<ClassifiedReference>& references) const {
    if (!self_.has_value() || !config_.flagForeignReferences) {
        return;
    }
    for (const ClassifiedReference& reference : references) {
        if (reference.locality != Locality::Foreign) {
            continue;
        }
        SessionEvent event;
        event.type = SessionEventType::ForeignObjectReferenced;
        event.channel = channel;
        event.diagnostic = "foreign object reference: mosID '" + reference.mosID + "' objID '" +
                           reference.objID + "' itemID '" + reference.itemID + "' in roID '" +
                           std::string{roID} + "' storyID '" + reference.storyID + "'";
        session.postEvent(std::move(event));
    }
}

}  // namespace imos::profiles
