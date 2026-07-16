// SPDX-FileCopyrightText: 2026 Javier O. Cordero Pérez
// SPDX-License-Identifier: Apache-2.0

#include <incs/running_order_author.h>

#include <algorithm>
#include <cstddef>
#include <set>
#include <string>
#include <utility>

#include <imos/codec/gen/views.h>

namespace incs {

namespace {

using imos::Result;
using imos::codec::Message;
using imos::codec::MessageType;
using imos::codec::XmlNode;

/// RO header child order (the roCreate/roList content model, stories last).
constexpr std::string_view kHeaderOrder[] = {"roID",      "roSlug",   "roChannel",
                                             "roEdStart", "roEdDur",  "roTrigger",
                                             "macroIn",   "macroOut", "mosExternalMetadata"};

std::string childTextOf(const XmlNode& parent, std::string_view name) {
    if (const XmlNode* child = parent.firstChildElement(name)) {
        return child->text();
    }
    return {};
}

/// One diffable sequence level: stories under an RO, or items under one
/// story (storyScope carries the enclosing storyID at the item level).
struct LevelSpec {
    std::string_view elemName;
    std::string_view keyName;
    std::string storyScope;
};

LevelSpec storyLevel() {
    return LevelSpec{"story", "storyID", {}};
}
LevelSpec itemLevel(std::string_view storyID) {
    return LevelSpec{"item", "itemID", std::string{storyID}};
}

std::vector<std::string> keysOf(const XmlNode& parent, const LevelSpec& level) {
    std::vector<std::string> keys;
    for (const XmlNode& child : parent.children) {
        if (child.isElement(level.elemName)) {
            keys.push_back(childTextOf(child, level.keyName));
        }
    }
    return keys;
}

const XmlNode* findByKey(const XmlNode& parent, const LevelSpec& level, std::string_view key) {
    for (const XmlNode& child : parent.children) {
        if (child.isElement(level.elemName) && childTextOf(child, level.keyName) == key) {
            return &child;
        }
    }
    return nullptr;
}

/// Serialization of everything but the nested sequence — the RO header
/// (children that are not <story>) or a story's own fields (not <item>).
std::string headerSerialization(const XmlNode& node, std::string_view sequenceName) {
    std::string out;
    for (const XmlNode& child : node.children) {
        if (!child.isElement(sequenceName)) {
            out += imos::codec::serializeXmlNode(child);
        }
    }
    return out;
}

Message makeElementAction(std::string_view roID, std::string_view operation) {
    Message message = imos::codec::makeMessage(MessageType::RoElementAction);
    if (XmlNode* body = message.body()) {
        body->setAttribute("operation", operation);
        body->appendElement("roID").setText(roID);
    }
    return message;
}

/// Append the element_target for an anchor ("" = tail). Story level omits
/// the target entirely for a tail anchor (spec: absent target); item level
/// always names the enclosing story.
void addAnchorTarget(XmlNode& body, const LevelSpec& level, std::string_view anchor) {
    if (level.storyScope.empty() && anchor.empty()) {
        return;
    }
    XmlNode& target = body.appendElement("element_target");
    if (level.storyScope.empty()) {
        target.appendElement("storyID").setText(anchor);
        return;
    }
    target.appendElement("storyID").setText(level.storyScope);
    if (!anchor.empty()) {
        target.appendElement("itemID").setText(anchor);
    }
}

/// The keys of the longest subsequence of `rem` already in `fin` order —
/// these stay put; everything else is MOVEd. Minimizing the moved set is
/// what makes MOVE always win over DELETE+INSERT churn.
std::set<std::string> lisStable(const std::vector<std::string>& rem,
                                const std::vector<std::string>& fin) {
    std::set<std::string> stable;
    if (rem.empty()) {
        return stable;
    }
    std::vector<std::size_t> pos(rem.size(), 0);
    for (std::size_t i = 0; i < rem.size(); ++i) {
        pos[i] = static_cast<std::size_t>(std::find(fin.begin(), fin.end(), rem[i]) - fin.begin());
    }
    std::vector<std::size_t> best(rem.size(), 1);
    std::vector<std::size_t> prev(rem.size(), rem.size());
    std::size_t tail = 0;
    for (std::size_t i = 1; i < rem.size(); ++i) {
        for (std::size_t j = 0; j < i; ++j) {
            if (pos[j] < pos[i] && best[j] + 1 > best[i]) {
                best[i] = best[j] + 1;
                prev[i] = j;
            }
        }
        if (best[i] > best[tail]) {
            tail = i;
        }
    }
    for (std::size_t i = tail; i != rem.size(); i = prev[i]) {
        stable.insert(rem[i]);
        if (prev[i] == rem.size()) {
            break;
        }
    }
    return stable;
}

/// Emit the roElementAction sequence transforming `before`'s element list
/// into `after`'s at one level: DELETE, then content REPLACE (or an
/// item-level recursion when only a story's items changed), then a single
/// SWAP when the change is exactly a transposition, else batched
/// INSERT/MOVE runs computed right-to-left against the final order.
void diffSequence(const XmlNode& before, const XmlNode& after, const LevelSpec& level,
                  std::string_view roID, std::vector<Message>& out) {
    const std::vector<std::string> keysB = keysOf(before, level);
    const std::vector<std::string> fin = keysOf(after, level);
    const std::set<std::string> setB{keysB.begin(), keysB.end()};
    const std::set<std::string> setA{fin.begin(), fin.end()};

    std::vector<std::string> deletions;
    for (const std::string& key : keysB) {
        if (!setA.contains(key)) {
            deletions.push_back(key);
        }
    }
    if (!deletions.empty()) {
        Message del = makeElementAction(roID, "DELETE");
        XmlNode& body = *del.body();
        addAnchorTarget(body, level, {});
        XmlNode& source = body.appendElement("element_source");
        for (const std::string& key : deletions) {
            source.appendElement(level.keyName).setText(key);
        }
        out.push_back(std::move(del));
    }

    bool newKeys = false;
    for (const std::string& key : fin) {
        if (!setB.contains(key)) {
            newKeys = true;
            continue;
        }
        const XmlNode* b = findByKey(before, level, key);
        const XmlNode* a = findByKey(after, level, key);
        if (imos::codec::serializeXmlNode(*b) == imos::codec::serializeXmlNode(*a)) {
            continue;
        }
        if (level.storyScope.empty() &&
            headerSerialization(*b, "item") == headerSerialization(*a, "item")) {
            diffSequence(*b, *a, itemLevel(key), roID, out);  // items-only change
            continue;
        }
        Message rep = makeElementAction(roID, "REPLACE");
        XmlNode& body = *rep.body();
        addAnchorTarget(body, level, key);
        body.appendElement("element_source").children.push_back(*a);
        out.push_back(std::move(rep));
    }

    std::vector<std::string> rem;
    for (const std::string& key : keysB) {
        if (setA.contains(key)) {
            rem.push_back(key);
        }
    }
    if (rem == fin) {
        return;  // same survivors, same order, nothing new
    }

    if (!newKeys && deletions.empty() && rem.size() == fin.size()) {
        std::vector<std::size_t> misplaced;
        for (std::size_t i = 0; i < rem.size(); ++i) {
            if (rem[i] != fin[i]) {
                misplaced.push_back(i);
            }
        }
        if (misplaced.size() == 2 && rem[misplaced[0]] == fin[misplaced[1]] &&
            rem[misplaced[1]] == fin[misplaced[0]]) {
            Message swap = makeElementAction(roID, "SWAP");
            XmlNode& body = *swap.body();
            addAnchorTarget(body, level, {});
            XmlNode& source = body.appendElement("element_source");
            source.appendElement(level.keyName).setText(rem[misplaced[0]]);
            source.appendElement(level.keyName).setText(rem[misplaced[1]]);
            out.push_back(std::move(swap));
            return;
        }
    }

    const std::set<std::string> stable = lisStable(rem, fin);
    std::string anchor;  // "" → tail
    std::size_t i = fin.size();
    while (i > 0) {
        --i;
        if (stable.contains(fin[i])) {
            anchor = fin[i];
            continue;
        }
        const bool isNew = !setB.contains(fin[i]);
        std::size_t j = i;
        while (j > 0 && !stable.contains(fin[j - 1]) && (!setB.contains(fin[j - 1])) == isNew) {
            --j;
        }
        Message action = makeElementAction(roID, isNew ? "INSERT" : "MOVE");
        XmlNode& body = *action.body();
        addAnchorTarget(body, level, anchor);
        XmlNode& source = body.appendElement("element_source");
        for (std::size_t k = j; k <= i; ++k) {
            if (isNew) {
                source.children.push_back(*findByKey(after, level, fin[k]));
            } else {
                source.appendElement(level.keyName).setText(fin[k]);
            }
        }
        out.push_back(std::move(action));
        anchor = fin[j];
        i = j;
    }
}

}  // namespace

RunningOrderAuthor::RunningOrderAuthor(imos::PeerSession* session)
    : RunningOrderAuthor(session, Config{}) {}

RunningOrderAuthor::RunningOrderAuthor(imos::PeerSession* session, Config config)
    : session_(session), config_(config) {}

Result<void> RunningOrderAuthor::applyEdit(const Message& message) {
    return desired_.apply(message);
}

Result<void> RunningOrderAuthor::create(std::string_view roID, std::string_view roSlug) {
    Message message = imos::codec::makeMessage(MessageType::RoCreate);
    if (XmlNode* body = message.body()) {
        imos::codec::view::RoCreate view{*body};
        view.setRoID(roID);
        view.setRoSlug(roSlug);
    }
    return applyEdit(message);
}

Result<void> RunningOrderAuthor::destroy(std::string_view roID) {
    Message message = imos::codec::makeMessage(MessageType::RoDelete);
    if (XmlNode* body = message.body()) {
        imos::codec::view::RoDelete{*body}.setRoID(roID);
    }
    return applyEdit(message);
}

Result<void> RunningOrderAuthor::setHeaderField(std::string_view roID, std::string_view field,
                                                std::string_view value) {
    if (field == "story" || field == "roID") {
        return imos::makeError(imos::ErrorCode::InvalidArgument,
                               "'" + std::string{field} + "' is not an editable header field");
    }
    const XmlNode* ro = desired_.runningOrder(roID);
    if (ro == nullptr) {
        return imos::makeError(imos::ErrorCode::NotFound,
                               "unknown roID '" + std::string{roID} + "'");
    }
    Message message = imos::codec::makeMessage(MessageType::RoMetadataReplace);
    if (XmlNode* body = message.body()) {
        for (const XmlNode& child : ro->children) {
            if (!child.isElement("story")) {
                body->children.push_back(child);
            }
        }
        imos::codec::detail::setChildText(*body, field, value, kHeaderOrder);
    }
    return applyEdit(message);
}

Result<void> RunningOrderAuthor::setReadyToAir(std::string_view roID, std::string_view roAir) {
    Message message = imos::codec::makeMessage(MessageType::RoReadyToAir);
    if (XmlNode* body = message.body()) {
        imos::codec::view::RoReadyToAir view{*body};
        view.setRoID(roID);
        view.setRoAir(roAir);
    }
    return applyEdit(message);
}

Result<void> RunningOrderAuthor::insertStory(std::string_view roID, const XmlNode& story,
                                             std::string_view beforeStoryID) {
    if (!story.isElement("story")) {
        return imos::makeError(imos::ErrorCode::InvalidArgument, "not a <story> subtree");
    }
    Message action = makeElementAction(roID, "INSERT");
    XmlNode& body = *action.body();
    if (!beforeStoryID.empty()) {
        body.appendElement("element_target").appendElement("storyID").setText(beforeStoryID);
    }
    body.appendElement("element_source").children.push_back(story);
    return applyEdit(action);
}

Result<void> RunningOrderAuthor::replaceStory(std::string_view roID, std::string_view storyID,
                                              const XmlNode& story) {
    if (!story.isElement("story")) {
        return imos::makeError(imos::ErrorCode::InvalidArgument, "not a <story> subtree");
    }
    Message action = makeElementAction(roID, "REPLACE");
    XmlNode& body = *action.body();
    body.appendElement("element_target").appendElement("storyID").setText(storyID);
    body.appendElement("element_source").children.push_back(story);
    return applyEdit(action);
}

Result<void> RunningOrderAuthor::removeStory(std::string_view roID, std::string_view storyID) {
    Message action = makeElementAction(roID, "DELETE");
    action.body()->appendElement("element_source").appendElement("storyID").setText(storyID);
    return applyEdit(action);
}

Result<void> RunningOrderAuthor::moveStory(std::string_view roID, std::string_view storyID,
                                           std::string_view beforeStoryID) {
    Message action = makeElementAction(roID, "MOVE");
    XmlNode& body = *action.body();
    if (!beforeStoryID.empty()) {
        body.appendElement("element_target").appendElement("storyID").setText(beforeStoryID);
    }
    body.appendElement("element_source").appendElement("storyID").setText(storyID);
    return applyEdit(action);
}

Result<void> RunningOrderAuthor::swapStories(std::string_view roID, std::string_view storyA,
                                             std::string_view storyB) {
    Message action = makeElementAction(roID, "SWAP");
    XmlNode& source = action.body()->appendElement("element_source");
    source.appendElement("storyID").setText(storyA);
    source.appendElement("storyID").setText(storyB);
    return applyEdit(action);
}

Result<void> RunningOrderAuthor::insertItem(std::string_view roID, std::string_view storyID,
                                            const XmlNode& item, std::string_view beforeItemID) {
    if (!item.isElement("item")) {
        return imos::makeError(imos::ErrorCode::InvalidArgument, "not an <item> subtree");
    }
    Message action = makeElementAction(roID, "INSERT");
    XmlNode& body = *action.body();
    XmlNode& target = body.appendElement("element_target");
    target.appendElement("storyID").setText(storyID);
    if (!beforeItemID.empty()) {
        target.appendElement("itemID").setText(beforeItemID);
    }
    body.appendElement("element_source").children.push_back(item);
    return applyEdit(action);
}

Result<void> RunningOrderAuthor::replaceItem(std::string_view roID, std::string_view storyID,
                                             std::string_view itemID, const XmlNode& item) {
    if (!item.isElement("item")) {
        return imos::makeError(imos::ErrorCode::InvalidArgument, "not an <item> subtree");
    }
    Message action = makeElementAction(roID, "REPLACE");
    XmlNode& body = *action.body();
    XmlNode& target = body.appendElement("element_target");
    target.appendElement("storyID").setText(storyID);
    target.appendElement("itemID").setText(itemID);
    body.appendElement("element_source").children.push_back(item);
    return applyEdit(action);
}

Result<void> RunningOrderAuthor::removeItem(std::string_view roID, std::string_view storyID,
                                            std::string_view itemID) {
    Message action = makeElementAction(roID, "DELETE");
    XmlNode& body = *action.body();
    body.appendElement("element_target").appendElement("storyID").setText(storyID);
    body.appendElement("element_source").appendElement("itemID").setText(itemID);
    return applyEdit(action);
}

Result<void> RunningOrderAuthor::moveItem(std::string_view roID, std::string_view storyID,
                                          std::string_view itemID, std::string_view beforeItemID) {
    Message action = makeElementAction(roID, "MOVE");
    XmlNode& body = *action.body();
    XmlNode& target = body.appendElement("element_target");
    target.appendElement("storyID").setText(storyID);
    if (!beforeItemID.empty()) {
        target.appendElement("itemID").setText(beforeItemID);
    }
    body.appendElement("element_source").appendElement("itemID").setText(itemID);
    return applyEdit(action);
}

Result<void> RunningOrderAuthor::swapItems(std::string_view roID, std::string_view storyID,
                                           std::string_view itemA, std::string_view itemB) {
    Message action = makeElementAction(roID, "SWAP");
    XmlNode& body = *action.body();
    body.appendElement("element_target").appendElement("storyID").setText(storyID);
    XmlNode& source = body.appendElement("element_source");
    source.appendElement("itemID").setText(itemA);
    source.appendElement("itemID").setText(itemB);
    return applyEdit(action);
}

XmlNode RunningOrderAuthor::makeStory(std::string_view storyID, std::string_view storySlug,
                                      std::string_view storyNum) {
    XmlNode story;
    story.name = "story";
    story.appendElement("storyID").setText(storyID);
    if (!storySlug.empty()) {
        story.appendElement("storySlug").setText(storySlug);
    }
    if (!storyNum.empty()) {
        story.appendElement("storyNum").setText(storyNum);
    }
    return story;
}

XmlNode RunningOrderAuthor::makeItem(std::string_view itemID, std::string_view objID,
                                     std::string_view mosID, std::string_view itemSlug) {
    XmlNode item;
    item.name = "item";
    item.appendElement("itemID").setText(itemID);
    if (!itemSlug.empty()) {
        item.appendElement("itemSlug").setText(itemSlug);
    }
    item.appendElement("objID").setText(objID);
    item.appendElement("mosID").setText(mosID);
    return item;
}

Result<std::vector<imos::Seq>> RunningOrderAuthor::commit(std::string_view roID) {
    const std::string id{roID};
    const XmlNode* want = desired_.runningOrder(id);
    const auto devIt = device_.find(id);
    const bool devExists = devIt != device_.end();
    if (want == nullptr && !devExists) {
        return imos::makeError(imos::ErrorCode::NotFound, "unknown roID '" + id + "'");
    }

    std::vector<Message> messages;
    bool full = false;
    if (want == nullptr) {
        Message del = imos::codec::makeMessage(MessageType::RoDelete);
        imos::codec::view::RoDelete{*del.body()}.setRoID(id);
        messages.push_back(std::move(del));
    } else if (!devExists || !devIt->second.valid) {
        // New to the device, or its state is unknown after a NACK: send the
        // complete RO (roReplace upserts on the device model).
        Message whole =
            imos::codec::makeMessage(devExists ? MessageType::RoReplace : MessageType::RoCreate);
        whole.body()->children = want->children;
        messages.push_back(std::move(whole));
        full = true;
    } else {
        const XmlNode& have = devIt->second.node;
        if (headerSerialization(have, "story") != headerSerialization(*want, "story")) {
            Message meta = imos::codec::makeMessage(MessageType::RoMetadataReplace);
            for (const XmlNode& child : want->children) {
                if (!child.isElement("story")) {
                    meta.body()->children.push_back(child);
                }
            }
            messages.push_back(std::move(meta));
        }
        diffSequence(have, *want, storyLevel(), id, messages);
        if (messages.size() > config_.replaceThreshold) {
            // The incremental delta is bigger than a wholesale replacement.
            messages.clear();
            Message whole = imos::codec::makeMessage(MessageType::RoReplace);
            whole.body()->children = want->children;
            messages.push_back(std::move(whole));
            full = true;
        }
    }

    if (want != nullptr) {
        DeviceRo& dev = device_[id];
        dev.node = *want;
        dev.valid = true;
        if (full) {
            dev.roAir.reset();  // a full create/replace resets device readiness
        }
        const std::optional<std::string> air = desired_.readyToAir(id);
        if (air.has_value() && air != dev.roAir) {
            Message ready = imos::codec::makeMessage(MessageType::RoReadyToAir);
            imos::codec::view::RoReadyToAir view{*ready.body()};
            view.setRoID(id);
            view.setRoAir(*air);
            messages.push_back(std::move(ready));
            dev.roAir = air;
        }
    } else {
        device_.erase(id);
    }

    std::vector<imos::Seq> seqs;
    seqs.reserve(messages.size());
    for (Message& message : messages) {
        Result<imos::Seq> seq = session_->enqueue(std::move(message));
        if (!seq.ok()) {
            if (const auto it = device_.find(id); it != device_.end()) {
                it->second.valid = false;  // device state unknown; replace next
            }
            return std::move(seq).error();
        }
        pending_.emplace(seq.value(), id);
        seqs.push_back(seq.value());
    }
    return seqs;
}

Result<std::vector<imos::Seq>> RunningOrderAuthor::commitAll() {
    std::set<std::string> ids;
    for (const std::string& id : desired_.runningOrderIDs()) {
        ids.insert(id);
    }
    for (const auto& [id, dev] : device_) {
        ids.insert(id);
    }
    std::vector<imos::Seq> seqs;
    for (const std::string& id : ids) {
        Result<std::vector<imos::Seq>> committed = commit(id);
        if (!committed.ok()) {
            return committed;
        }
        seqs.insert(seqs.end(), committed.value().begin(), committed.value().end());
    }
    return seqs;
}

bool RunningOrderAuthor::handle(const imos::SessionEvent& event) {
    if (event.channel != imos::Channel::Ro) {
        return false;  // store seqs are per-channel; ours are all ro
    }
    const auto it = pending_.find(event.seq);
    if (it == pending_.end()) {
        return false;
    }
    switch (event.type) {
        case imos::SessionEventType::AckReceived: {
            CommitEvent resolved;
            resolved.roID = it->second;
            resolved.seq = event.seq;
            pending_.erase(it);
            if (event.message.type() == MessageType::RoAck) {
                resolved.ok = !imos::profiles::roAckIsNack(event.message);
                resolved.roStatus = imos::profiles::roAckStatusText(event.message);
                resolved.statuses = imos::profiles::roAckStatuses(event.message);
            } else {
                resolved.ok = true;  // any non-roAck reply completed the exchange
            }
            if (!resolved.ok) {
                if (const auto dev = device_.find(resolved.roID); dev != device_.end()) {
                    dev->second.valid = false;  // next commit falls back to roReplace
                }
            }
            events_.push_back(std::move(resolved));
            return true;
        }
        case imos::SessionEventType::DeliveryFailed: {
            CommitEvent resolved;
            resolved.roID = it->second;
            resolved.seq = event.seq;
            resolved.deliveryFailed = true;
            pending_.erase(it);
            if (const auto dev = device_.find(resolved.roID); dev != device_.end()) {
                dev->second.valid = false;
            }
            events_.push_back(std::move(resolved));
            return true;
        }
        default:
            return false;
    }
}

std::optional<CommitEvent> RunningOrderAuthor::nextEvent() {
    if (events_.empty()) {
        return std::nullopt;
    }
    CommitEvent event = std::move(events_.front());
    events_.pop_front();
    return event;
}

std::optional<Message> RunningOrderAuthor::runningOrder(std::string_view roID) {
    const std::string id{roID};
    const XmlNode* want = desired_.runningOrder(id);
    if (want == nullptr) {
        return std::nullopt;  // deleted/unknown → the responder NACKs the roReq
    }
    Message list = imos::codec::makeMessage(MessageType::RoList);
    list.body()->children = want->children;
    // The device rebuilds from this snapshot: it becomes the projection, and
    // readiness resets with it (a rebuilt RO carries no roAir state).
    DeviceRo& dev = device_[id];
    dev.node = *want;
    dev.valid = true;
    dev.roAir.reset();
    return list;
}

}  // namespace incs
