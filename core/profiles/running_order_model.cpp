// SPDX-FileCopyrightText: 2026 Javier O. Cordero Pérez
// SPDX-License-Identifier: Apache-2.0

#include <imos/profiles/running_order_model.h>

#include <algorithm>
#include <cstddef>
#include <string>
#include <utility>

namespace imos::profiles {

namespace {

constexpr std::size_t kNpos = static_cast<std::size_t>(-1);

/// One ordered element sequence under a parent: <story> children of an RO
/// node or <item> children of a story. The five roElementAction operations
/// are implemented once against this shape and reused at both levels.
struct Sequence {
    codec::XmlNode* parent;
    std::string_view elemName;  ///< "story" / "item"
    std::string_view keyName;   ///< "storyID" / "itemID"

    [[nodiscard]] std::size_t indexOf(std::string_view id) const {
        for (std::size_t i = 0; i < parent->children.size(); ++i) {
            const codec::XmlNode& child = parent->children[i];
            if (child.isElement(elemName) && child.childText(keyName) == id) {
                return i;
            }
        }
        return kNpos;
    }

    /// Insertion index for "append": right after the last sequence element,
    /// or the very end when the sequence is empty.
    [[nodiscard]] std::size_t appendIndex() const {
        for (std::size_t i = parent->children.size(); i > 0; --i) {
            if (parent->children[i - 1].isElement(elemName)) {
                return i;
            }
        }
        return parent->children.size();
    }

    [[nodiscard]] bool contains(std::string_view id) const { return indexOf(id) != kNpos; }
};

/// Validate that inserted/replacement payload nodes carry unique, nonempty
/// keys that do not collide with elements already in the sequence (an ID
/// listed in `ignore` — the element being replaced — is allowed to recur).
Result<void> checkPayloadKeys(const Sequence& seq, const std::vector<const codec::XmlNode*>& nodes,
                              const std::vector<std::string>& ignore) {
    std::vector<std::string> seen;
    for (const codec::XmlNode* node : nodes) {
        std::string id = node->childText(seq.keyName);
        if (id.empty()) {
            return makeError(ErrorCode::ProtocolViolation, "payload " + std::string{seq.elemName} +
                                                               " is missing its " +
                                                               std::string{seq.keyName});
        }
        if (std::find(seen.begin(), seen.end(), id) != seen.end()) {
            return makeError(ErrorCode::ProtocolViolation,
                             "duplicate " + std::string{seq.keyName} + " '" + id + "' in payload");
        }
        if (std::find(ignore.begin(), ignore.end(), id) == ignore.end() && seq.contains(id)) {
            return makeError(ErrorCode::ProtocolViolation, std::string{seq.keyName} + " '" + id +
                                                               "' already exists in the sequence");
        }
        seen.push_back(std::move(id));
    }
    return {};
}

Result<void> sequenceInsert(Sequence seq, const std::vector<const codec::XmlNode*>& nodes,
                            std::string_view beforeId) {
    if (nodes.empty()) {
        return makeError(ErrorCode::ProtocolViolation, "INSERT carries no payload elements");
    }
    if (Result<void> keys = checkPayloadKeys(seq, nodes, {}); !keys.ok()) {
        return keys;
    }
    std::size_t pos = kNpos;
    if (beforeId.empty()) {
        pos = seq.appendIndex();  // empty/absent target → insert at the tail
    } else {
        pos = seq.indexOf(beforeId);
        if (pos == kNpos) {
            return makeError(ErrorCode::NotFound, "unknown " + std::string{seq.keyName} + " '" +
                                                      std::string{beforeId} + "'");
        }
    }
    for (const codec::XmlNode* node : nodes) {
        seq.parent->children.insert(seq.parent->children.begin() + static_cast<std::ptrdiff_t>(pos),
                                    *node);  // deep copy: payload rides through verbatim
        ++pos;
    }
    return {};
}

Result<void> sequenceReplace(Sequence seq, const std::vector<const codec::XmlNode*>& nodes,
                             std::string_view targetId) {
    if (nodes.empty()) {
        return makeError(ErrorCode::ProtocolViolation, "REPLACE carries no payload elements");
    }
    if (targetId.empty()) {
        return makeError(ErrorCode::ProtocolViolation,
                         "REPLACE requires a target " + std::string{seq.keyName});
    }
    const std::size_t pos = seq.indexOf(targetId);
    if (pos == kNpos) {
        return makeError(ErrorCode::NotFound, "unknown " + std::string{seq.keyName} + " '" +
                                                  std::string{targetId} + "'");
    }
    if (Result<void> keys = checkPayloadKeys(seq, nodes, {std::string{targetId}}); !keys.ok()) {
        return keys;
    }
    seq.parent->children.erase(seq.parent->children.begin() + static_cast<std::ptrdiff_t>(pos));
    std::size_t at = pos;
    for (const codec::XmlNode* node : nodes) {
        seq.parent->children.insert(seq.parent->children.begin() + static_cast<std::ptrdiff_t>(at),
                                    *node);
        ++at;
    }
    return {};
}

Result<void> sequenceMove(Sequence seq, const std::vector<std::string>& ids,
                          std::string_view beforeId) {
    if (ids.empty()) {
        return makeError(ErrorCode::ProtocolViolation, "MOVE carries no source IDs");
    }
    for (std::size_t i = 0; i < ids.size(); ++i) {
        if (std::find(ids.begin(), ids.begin() + static_cast<std::ptrdiff_t>(i), ids[i]) !=
            ids.begin() + static_cast<std::ptrdiff_t>(i)) {
            return makeError(ErrorCode::ProtocolViolation,
                             "duplicate " + std::string{seq.keyName} + " '" + ids[i] + "' in MOVE");
        }
        if (!seq.contains(ids[i])) {
            return makeError(ErrorCode::NotFound,
                             "unknown " + std::string{seq.keyName} + " '" + ids[i] + "'");
        }
    }

    // Resolve the anchor to a key that survives removal. MOVE-to-self
    // (the target is itself being moved) anchors on the first element after
    // the target's original position that stays put, so a degenerate
    // "move [X] before X" leaves the sequence unchanged.
    std::string anchor;  // empty → tail
    if (!beforeId.empty()) {
        const std::size_t targetPos = seq.indexOf(beforeId);
        if (targetPos == kNpos) {
            return makeError(ErrorCode::NotFound, "unknown " + std::string{seq.keyName} + " '" +
                                                      std::string{beforeId} + "'");
        }
        if (std::find(ids.begin(), ids.end(), std::string{beforeId}) == ids.end()) {
            anchor = beforeId;
        } else {
            for (std::size_t i = targetPos + 1; i < seq.parent->children.size(); ++i) {
                const codec::XmlNode& child = seq.parent->children[i];
                if (!child.isElement(seq.elemName)) {
                    continue;
                }
                std::string id = child.childText(seq.keyName);
                if (std::find(ids.begin(), ids.end(), id) == ids.end()) {
                    anchor = std::move(id);
                    break;
                }
            }
        }
    }

    std::vector<codec::XmlNode> moved;
    moved.reserve(ids.size());
    for (const std::string& id : ids) {
        const std::size_t pos = seq.indexOf(id);
        moved.push_back(std::move(seq.parent->children[pos]));
        seq.parent->children.erase(seq.parent->children.begin() + static_cast<std::ptrdiff_t>(pos));
    }

    std::size_t at = anchor.empty() ? seq.appendIndex() : seq.indexOf(anchor);
    for (codec::XmlNode& node : moved) {
        seq.parent->children.insert(seq.parent->children.begin() + static_cast<std::ptrdiff_t>(at),
                                    std::move(node));
        ++at;
    }
    return {};
}

Result<void> sequenceDelete(Sequence seq, const std::vector<std::string>& ids) {
    if (ids.empty()) {
        return makeError(ErrorCode::ProtocolViolation, "DELETE carries no source IDs");
    }
    for (const std::string& id : ids) {
        if (!seq.contains(id)) {
            return makeError(ErrorCode::NotFound,
                             "unknown " + std::string{seq.keyName} + " '" + id + "'");
        }
    }
    for (const std::string& id : ids) {
        const std::size_t pos = seq.indexOf(id);
        if (pos != kNpos) {  // a duplicated ID in the list deletes once
            seq.parent->children.erase(seq.parent->children.begin() +
                                       static_cast<std::ptrdiff_t>(pos));
        }
    }
    return {};
}

Result<void> sequenceSwap(Sequence seq, const std::vector<std::string>& ids) {
    if (ids.size() != 2) {
        return makeError(ErrorCode::ProtocolViolation,
                         "SWAP requires exactly two " + std::string{seq.keyName} + "s");
    }
    const std::size_t a = seq.indexOf(ids[0]);
    const std::size_t b = seq.indexOf(ids[1]);
    if (a == kNpos || b == kNpos) {
        return makeError(ErrorCode::NotFound, "unknown " + std::string{seq.keyName} + " '" +
                                                  ids[a == kNpos ? 0 : 1] + "'");
    }
    std::swap(seq.parent->children[a], seq.parent->children[b]);
    return {};
}

/// Element-status key inside one RO's status map.
std::string statusKey(RoElement element, std::string_view storyID, std::string_view itemID) {
    switch (element) {
        case RoElement::RunningOrder:
            return "R";
        case RoElement::Story:
            return "S|" + std::string{storyID};
        case RoElement::Item:
            return "I|" + std::string{storyID} + "|" + std::string{itemID};
    }
    return {};
}

}  // namespace

std::string_view roElementName(RoElement element) noexcept {
    switch (element) {
        case RoElement::RunningOrder:
            return "RO";
        case RoElement::Story:
            return "STORY";
        case RoElement::Item:
            return "ITEM";
    }
    return {};
}

std::optional<RoElement> roElementFromName(std::string_view name) noexcept {
    if (name == "RO") {
        return RoElement::RunningOrder;
    }
    if (name == "STORY") {
        return RoElement::Story;
    }
    if (name == "ITEM") {
        return RoElement::Item;
    }
    return std::nullopt;
}

Result<void> RunningOrderModel::apply(const codec::Message& message) {
    const codec::XmlNode* body = message.body();
    if (body == nullptr) {
        return makeError(ErrorCode::InvalidArgument, "message has no body element");
    }
    const std::string roID = body->childText("roID");

    switch (message.type()) {
        case codec::MessageType::RoCreate: {
            if (roID.empty()) {
                return makeError(ErrorCode::InvalidArgument, "roCreate is missing its roID");
            }
            if (orders_.contains(roID)) {
                return makeError(ErrorCode::WrongState, "roID '" + roID +
                                                            "' already exists; roReplace "
                                                            "replaces a running order");
            }
            StoredRo stored;
            stored.node = *body;  // deep copy: unknown tags ride through
            stored.node.name = "ro";
            orders_.emplace(roID, std::move(stored));
            return {};
        }
        case codec::MessageType::RoReplace:
        case codec::MessageType::RoList: {
            // Both carry the complete RO, so they upsert: nothing is missing
            // that a resync could recover. roList is the rebuild path itself.
            if (roID.empty()) {
                return makeError(ErrorCode::InvalidArgument, "message is missing its roID");
            }
            StoredRo stored;
            stored.node = *body;
            stored.node.name = "ro";
            orders_.insert_or_assign(roID, std::move(stored));
            return {};
        }
        case codec::MessageType::RoMetadataReplace:
            return applyMetadataReplace(message);
        case codec::MessageType::RoDelete: {
            const auto it = orders_.find(roID);
            if (it == orders_.end()) {
                return makeError(ErrorCode::NotFound, "unknown roID '" + roID + "'");
            }
            orders_.erase(it);
            return {};
        }
        case codec::MessageType::RoReadyToAir: {
            const auto it = orders_.find(roID);
            if (it == orders_.end()) {
                return makeError(ErrorCode::NotFound, "unknown roID '" + roID + "'");
            }
            it->second.roAir = body->childText("roAir");
            return {};
        }
        case codec::MessageType::RoElementAction:
            return applyElementAction(message);
        case codec::MessageType::RoElementStat:
            return applyElementStat(message);
        default:
            return makeError(
                ErrorCode::InvalidArgument,
                std::string{message.info().wireName} + " is not a Profile 2 construction message");
    }
}

Result<void> RunningOrderModel::applyMetadataReplace(const codec::Message& message) {
    const codec::XmlNode* body = message.body();
    const std::string roID = body->childText("roID");
    const auto it = orders_.find(roID);
    if (it == orders_.end()) {
        return makeError(ErrorCode::NotFound, "unknown roID '" + roID + "'");
    }
    // Header fields are replaced wholesale from the message (absent optional
    // fields drop); the story list — and only the story list — is preserved.
    codec::XmlNode rebuilt = it->second.node;
    rebuilt.children.clear();
    for (const codec::XmlNode& child : body->children) {
        if (!child.isElement("story")) {
            rebuilt.children.push_back(child);
        }
    }
    for (const codec::XmlNode& child : it->second.node.children) {
        if (child.isElement("story")) {
            rebuilt.children.push_back(child);
        }
    }
    it->second.node = std::move(rebuilt);
    return {};
}

Result<void> RunningOrderModel::applyElementAction(const codec::Message& message) {
    const codec::XmlNode* body = message.body();
    const std::string roID = body->childText("roID");
    const auto it = orders_.find(roID);
    if (it == orders_.end()) {
        return makeError(ErrorCode::NotFound, "unknown roID '" + roID + "'");
    }

    std::string operation;
    if (const std::optional<std::string_view> op = body->attribute("operation")) {
        operation = *op;
    }
    const codec::XmlNode* source = body->firstChildElement("element_source");
    if (source == nullptr) {
        return makeError(ErrorCode::ProtocolViolation, "roElementAction has no element_source");
    }
    const codec::XmlNode* target = body->firstChildElement("element_target");
    const std::string targetStory = target ? target->childText("storyID") : std::string{};
    const std::string targetItem = target ? target->childText("itemID") : std::string{};

    // The source carries stories, items, storyIDs, or itemIDs — never a mix
    // (spec §3.6.1: "all Stories or all Items").
    std::vector<const codec::XmlNode*> storyNodes;
    std::vector<const codec::XmlNode*> itemNodes;
    std::vector<std::string> storyIds;
    std::vector<std::string> itemIds;
    for (const codec::XmlNode& child : source->children) {
        if (child.isElement("story")) {
            storyNodes.push_back(&child);
        } else if (child.isElement("item")) {
            itemNodes.push_back(&child);
        } else if (child.isElement("storyID")) {
            storyIds.push_back(child.text());
        } else if (child.isElement("itemID")) {
            itemIds.push_back(child.text());
        }
    }
    const int kinds = static_cast<int>(!storyNodes.empty()) + static_cast<int>(!itemNodes.empty()) +
                      static_cast<int>(!storyIds.empty()) + static_cast<int>(!itemIds.empty());
    if (kinds != 1) {
        return makeError(ErrorCode::ProtocolViolation, kinds == 0
                                                           ? "element_source is empty"
                                                           : "element_source mixes element kinds");
    }

    // All mutations run on a scratch copy; commit only on success so a
    // failed action leaves the stored RO untouched (transactionality).
    codec::XmlNode work = it->second.node;
    const Sequence stories{&work, "story", "storyID"};

    const auto itemSequence = [&](std::string_view storyID) -> Result<Sequence> {
        if (storyID.empty()) {
            return makeError(ErrorCode::ProtocolViolation,
                             "item-level " + operation + " requires a target storyID");
        }
        const std::size_t pos = stories.indexOf(storyID);
        if (pos == kNpos) {
            return makeError(ErrorCode::NotFound, "unknown storyID '" + std::string{storyID} + "'");
        }
        return Sequence{&work.children[pos], "item", "itemID"};
    };

    Result<void> outcome = makeError(ErrorCode::ProtocolViolation,
                                     "unknown roElementAction operation '" + operation + "'");
    if (operation == "INSERT") {
        if (!storyNodes.empty()) {
            outcome = sequenceInsert(stories, storyNodes, targetStory);
        } else if (!itemNodes.empty()) {
            Result<Sequence> items = itemSequence(targetStory);
            outcome = items.ok() ? sequenceInsert(items.value(), itemNodes, targetItem)
                                 : Result<void>{std::move(items).error()};
        } else {
            outcome = makeError(ErrorCode::ProtocolViolation,
                                "INSERT requires story or item payload elements");
        }
    } else if (operation == "REPLACE") {
        if (!storyNodes.empty()) {
            outcome = sequenceReplace(stories, storyNodes, targetStory);
        } else if (!itemNodes.empty()) {
            Result<Sequence> items = itemSequence(targetStory);
            outcome = items.ok() ? sequenceReplace(items.value(), itemNodes, targetItem)
                                 : Result<void>{std::move(items).error()};
        } else {
            outcome = makeError(ErrorCode::ProtocolViolation,
                                "REPLACE requires story or item payload elements");
        }
    } else if (operation == "MOVE") {
        if (!storyIds.empty()) {
            outcome = sequenceMove(stories, storyIds, targetStory);
        } else if (!itemIds.empty()) {
            Result<Sequence> items = itemSequence(targetStory);
            outcome = items.ok() ? sequenceMove(items.value(), itemIds, targetItem)
                                 : Result<void>{std::move(items).error()};
        } else {
            outcome = makeError(ErrorCode::ProtocolViolation,
                                "MOVE requires storyID or itemID "
                                "source references");
        }
    } else if (operation == "DELETE") {
        if (!storyIds.empty()) {
            outcome = sequenceDelete(stories, storyIds);
        } else if (!itemIds.empty()) {
            Result<Sequence> items = itemSequence(targetStory);
            outcome = items.ok() ? sequenceDelete(items.value(), itemIds)
                                 : Result<void>{std::move(items).error()};
        } else {
            outcome = makeError(ErrorCode::ProtocolViolation,
                                "DELETE requires storyID or itemID "
                                "source references");
        }
    } else if (operation == "SWAP") {
        if (!storyIds.empty()) {
            outcome = sequenceSwap(stories, storyIds);
        } else if (!itemIds.empty()) {
            Result<Sequence> items = itemSequence(targetStory);
            outcome = items.ok() ? sequenceSwap(items.value(), itemIds)
                                 : Result<void>{std::move(items).error()};
        } else {
            outcome = makeError(ErrorCode::ProtocolViolation,
                                "SWAP requires storyID or itemID "
                                "source references");
        }
    }

    if (!outcome.ok()) {
        return outcome;
    }
    it->second.node = std::move(work);
    return {};
}

Result<void> RunningOrderModel::applyElementStat(const codec::Message& message) {
    const codec::XmlNode* body = message.body();
    std::string elementName;
    if (const std::optional<std::string_view> attr = body->attribute("element")) {
        elementName = *attr;
    }
    const std::optional<RoElement> element = roElementFromName(elementName);
    if (!element.has_value()) {
        return makeError(ErrorCode::ProtocolViolation,
                         "roElementStat element '" + elementName + "' is not RO, STORY, or ITEM");
    }
    return setStatus(*element, body->childText("roID"), body->childText("storyID"),
                     body->childText("itemID"), body->childText("status"));
}

void RunningOrderModel::erase(std::string_view roID) {
    orders_.erase(std::string{roID});
}

bool RunningOrderModel::has(std::string_view roID) const {
    return orders_.contains(std::string{roID});
}

std::vector<std::string> RunningOrderModel::runningOrderIDs() const {
    std::vector<std::string> ids;
    ids.reserve(orders_.size());
    for (const auto& [id, stored] : orders_) {
        ids.push_back(id);
    }
    return ids;
}

const codec::XmlNode* RunningOrderModel::runningOrder(std::string_view roID) const {
    const auto it = orders_.find(std::string{roID});
    return it == orders_.end() ? nullptr : &it->second.node;
}

std::vector<std::string> RunningOrderModel::storyIDs(std::string_view roID) const {
    std::vector<std::string> ids;
    if (const codec::XmlNode* ro = runningOrder(roID)) {
        for (const codec::XmlNode& child : ro->children) {
            if (child.isElement("story")) {
                ids.push_back(child.childText("storyID"));
            }
        }
    }
    return ids;
}

const codec::XmlNode* RunningOrderModel::story(std::string_view roID,
                                               std::string_view storyID) const {
    if (const codec::XmlNode* ro = runningOrder(roID)) {
        for (const codec::XmlNode& child : ro->children) {
            if (child.isElement("story") && child.childText("storyID") == storyID) {
                return &child;
            }
        }
    }
    return nullptr;
}

std::vector<std::string> RunningOrderModel::itemIDs(std::string_view roID,
                                                    std::string_view storyID) const {
    std::vector<std::string> ids;
    if (const codec::XmlNode* storyNode = story(roID, storyID)) {
        for (const codec::XmlNode& child : storyNode->children) {
            if (child.isElement("item")) {
                ids.push_back(child.childText("itemID"));
            }
        }
    }
    return ids;
}

const codec::XmlNode* RunningOrderModel::item(std::string_view roID, std::string_view storyID,
                                              std::string_view itemID) const {
    if (const codec::XmlNode* storyNode = story(roID, storyID)) {
        for (const codec::XmlNode& child : storyNode->children) {
            if (child.isElement("item") && child.childText("itemID") == itemID) {
                return &child;
            }
        }
    }
    return nullptr;
}

std::optional<std::string> RunningOrderModel::readyToAir(std::string_view roID) const {
    const auto it = orders_.find(std::string{roID});
    if (it == orders_.end()) {
        return std::nullopt;
    }
    return it->second.roAir;
}

Result<void> RunningOrderModel::setStatus(RoElement element, std::string_view roID,
                                          std::string_view storyID, std::string_view itemID,
                                          std::string_view status) {
    const auto it = orders_.find(std::string{roID});
    if (it == orders_.end()) {
        return makeError(ErrorCode::NotFound, "unknown roID '" + std::string{roID} + "'");
    }
    if (element == RoElement::Story || element == RoElement::Item) {
        if (story(roID, storyID) == nullptr) {
            return makeError(ErrorCode::NotFound, "unknown storyID '" + std::string{storyID} + "'");
        }
    }
    if (element == RoElement::Item && item(roID, storyID, itemID) == nullptr) {
        return makeError(ErrorCode::NotFound, "unknown itemID '" + std::string{itemID} + "'");
    }
    it->second.statuses.insert_or_assign(statusKey(element, storyID, itemID), std::string{status});
    return {};
}

std::optional<std::string> RunningOrderModel::status(RoElement element, std::string_view roID,
                                                     std::string_view storyID,
                                                     std::string_view itemID) const {
    const auto it = orders_.find(std::string{roID});
    if (it == orders_.end()) {
        return std::nullopt;
    }
    switch (element) {
        case RoElement::RunningOrder:
            break;
        case RoElement::Story:
            if (story(roID, storyID) == nullptr) {
                return std::nullopt;  // gone elements report no status
            }
            break;
        case RoElement::Item:
            if (item(roID, storyID, itemID) == nullptr) {
                return std::nullopt;
            }
            break;
    }
    const auto found = it->second.statuses.find(statusKey(element, storyID, itemID));
    if (found == it->second.statuses.end()) {
        return std::nullopt;
    }
    return found->second;
}

}  // namespace imos::profiles
