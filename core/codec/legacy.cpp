// SPDX-FileCopyrightText: 2026 Javier O. Cordero Pérez
// SPDX-License-Identifier: Apache-2.0

#include <imos/codec/legacy.h>

#include <array>
#include <string>
#include <string_view>
#include <vector>

namespace imos::codec {

namespace {

struct SequenceMapping {
    MessageType type;
    std::string_view operation;
    bool sourceIsSubtree;     // source carries <story>/<item> subtrees, not IDs
    bool itemScope;           // target/source work at item level (inside a story)
    bool lastSourceIsTarget;  // *MoveMultiple: the final ID is the target
    bool hasTarget;           // emits an <element_target>
};

constexpr auto kSequenceMappings = std::to_array<SequenceMapping>({
    {MessageType::RoStoryAppend, "INSERT", true, false, false, false},
    {MessageType::RoStoryInsert, "INSERT", true, false, false, true},
    {MessageType::RoStoryReplace, "REPLACE", true, false, false, true},
    {MessageType::RoStoryMove, "MOVE", false, false, true, true},
    {MessageType::RoStoryMoveMultiple, "MOVE", false, false, true, true},
    {MessageType::RoStorySwap, "SWAP", false, false, false, false},
    {MessageType::RoStoryDelete, "DELETE", false, false, false, false},
    {MessageType::RoItemInsert, "INSERT", true, true, false, true},
    {MessageType::RoItemReplace, "REPLACE", true, true, false, true},
    {MessageType::RoItemMoveMultiple, "MOVE", false, true, true, true},
    {MessageType::RoItemDelete, "DELETE", false, true, false, true},
});

const SequenceMapping* findSequenceMapping(MessageType type) noexcept {
    for (const SequenceMapping& mapping : kSequenceMappings) {
        if (mapping.type == type) {
            return &mapping;
        }
    }
    return nullptr;
}

void copyEnvelope(const Message& from, Message& to) {
    for (const std::string_view name : {"mosID", "ncsID", "messageID"}) {
        if (const XmlNode* child = from.document().root.firstChildElement(name)) {
            to.document().root.appendElement(name).setText(child->text());
        }
    }
}

Message makeBareMessage(MessageType type) {
    XmlDocument document;
    document.root.name = "mos";
    Message message{std::move(document)};
    message.document().root.appendElement(messageInfo(type).wireName);
    message.refreshType();
    return message;
}

void appendIdElement(XmlNode& parent, std::string_view name, std::string_view value) {
    parent.appendElement(name).setText(value);
}

/// Normalize the eleven roStory*/roItem* sequence messages onto
/// roElementAction. Shapes follow the spec's own roElementAction examples:
/// story-scope SWAP/DELETE carry no target; *MoveMultiple's trailing ID is
/// the target; item-scope targets carry the containing storyID.
Result<Message> normalizeSequence(const Message& legacy, const SequenceMapping& mapping) {
    const XmlNode& body = *legacy.body();

    const XmlNode* roId = body.firstChildElement("roID");
    const std::string roIdText = roId != nullptr ? roId->text() : std::string{};

    const std::string_view idName = mapping.itemScope ? "itemID" : "storyID";
    std::vector<const XmlNode*> ids;
    std::vector<const XmlNode*> subtrees;
    std::string storyContext;  // item scope: the storyID naming the story
    bool sawStoryContext = false;
    for (const XmlNode& child : body.children) {
        if (child.kind != XmlNodeKind::Element) {
            continue;
        }
        if (mapping.itemScope && child.name == "storyID" && !sawStoryContext) {
            storyContext = child.text();
            sawStoryContext = true;
        } else if (child.name == idName) {
            ids.push_back(&child);
        } else if (mapping.sourceIsSubtree && (child.name == "story" || child.name == "item")) {
            subtrees.push_back(&child);
        }
    }

    if (mapping.itemScope && !sawStoryContext) {
        return makeError(ErrorCode::ProtocolViolation, std::string{legacy.info().wireName} +
                                                           " lacks the storyID naming "
                                                           "the story it operates on");
    }
    if (mapping.lastSourceIsTarget && ids.size() < 2) {
        return makeError(
            ErrorCode::ProtocolViolation,
            std::string{legacy.info().wireName} + " needs at least two IDs (sources plus target)");
    }
    if (mapping.sourceIsSubtree && subtrees.empty()) {
        return makeError(ErrorCode::ProtocolViolation, std::string{legacy.info().wireName} +
                                                           " carries no story/item "
                                                           "payload");
    }
    if (!mapping.sourceIsSubtree && !mapping.lastSourceIsTarget && ids.empty()) {
        return makeError(ErrorCode::ProtocolViolation,
                         std::string{legacy.info().wireName} + " carries no IDs");
    }

    Message normalized = makeBareMessage(MessageType::RoElementAction);
    copyEnvelope(legacy, normalized);
    XmlNode& action = *normalized.body();
    action.setAttribute("operation", mapping.operation);
    appendIdElement(action, "roID", roIdText);

    // Target: where the operation lands. Story-scope SWAP and DELETE carry
    // none; *MoveMultiple's trailing ID is the destination; the subtree
    // messages name their slot with a lone ID; roItemDelete's target is the
    // containing story alone.
    if (mapping.hasTarget) {
        XmlNode& target = action.appendElement("element_target");
        const std::string_view targetIdName = mapping.itemScope ? "itemID" : "storyID";
        if (mapping.itemScope) {
            appendIdElement(target, "storyID", storyContext);
        }
        if (mapping.lastSourceIsTarget) {
            appendIdElement(target, targetIdName, ids.back()->text());
        } else if (mapping.sourceIsSubtree && !ids.empty()) {
            appendIdElement(target, targetIdName, ids.front()->text());
        } else if (!mapping.itemScope) {
            appendIdElement(target, targetIdName, "");
        }
    }

    // Source: the moved/inserted subtrees or the affected IDs.
    XmlNode& source = action.appendElement("element_source");
    if (mapping.sourceIsSubtree) {
        for (const XmlNode* subtree : subtrees) {
            source.children.push_back(*subtree);  // verbatim copy: metadata rides along
            source.emptyTag = false;
        }
    } else {
        const std::size_t sourceIdCount = ids.size() - (mapping.lastSourceIsTarget ? 1 : 0);
        const std::string_view sourceIdName = mapping.itemScope ? "itemID" : "storyID";
        for (std::size_t i = 0; i < sourceIdCount; ++i) {
            appendIdElement(source, sourceIdName, ids[i]->text());
        }
    }
    return normalized;
}

/// roStat -> roElementStat element="RO"; roItemStat -> element="ITEM".
Message normalizeStat(const Message& legacy, bool itemScope) {
    Message normalized = makeBareMessage(MessageType::RoElementStat);
    copyEnvelope(legacy, normalized);
    XmlNode& stat = *normalized.body();
    stat.setAttribute("element", itemScope ? "ITEM" : "RO");

    const XmlNode& body = *legacy.body();
    for (const std::string_view name :
         {"roID", "storyID", "itemID", "objID", "itemChannel", "status", "time"}) {
        if (const XmlNode* child = body.firstChildElement(name)) {
            stat.appendElement(name).setText(child->text());
        }
    }
    return normalized;
}

}  // namespace

bool isLegacyRemoved(MessageType type) noexcept {
    return messageInfo(type).legacyRemoved;
}

Result<Message> normalizeLegacy(const Message& legacy) {
    if (!isLegacyRemoved(legacy.type()) || legacy.body() == nullptr) {
        return makeError(ErrorCode::InvalidArgument,
                         "normalizeLegacy requires one of the thirteen removed messages");
    }
    if (legacy.type() == MessageType::RoStat) {
        return normalizeStat(legacy, false);
    }
    if (legacy.type() == MessageType::RoItemStat) {
        return normalizeStat(legacy, true);
    }
    const SequenceMapping* mapping = findSequenceMapping(legacy.type());
    if (mapping == nullptr) {
        return makeError(ErrorCode::InvalidArgument, "unmapped legacy message");
    }
    return normalizeSequence(legacy, *mapping);
}

}  // namespace imos::codec
