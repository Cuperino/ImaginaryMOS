// SPDX-FileCopyrightText: 2026 Javier O. Cordero Pérez
// SPDX-License-Identifier: Apache-2.0

#include <imos/codec/message.h>

#include <array>
#include <string>
#include <string_view>
#include <utility>

namespace imos::codec {

namespace {

constexpr std::array<std::string_view, 3> kEnvelopeOrder = {"mosID", "ncsID", "messageID"};

bool isEnvelopeElement(std::string_view name) noexcept {
    return name == "mosID" || name == "ncsID" || name == "messageID";
}

const XmlNode* findBody(const XmlNode& root) noexcept {
    for (const XmlNode& child : root.children) {
        if (child.kind == XmlNodeKind::Element && !isEnvelopeElement(child.name)) {
            return &child;
        }
    }
    return nullptr;
}

}  // namespace

Message::Message(XmlDocument document) : doc_(std::move(document)) {
    refreshType();
}

void Message::refreshType() noexcept {
    const XmlNode* bodyNode = findBody(doc_.root);
    type_ = bodyNode == nullptr ? MessageType::Unknown : messageTypeFromName(bodyNode->name);
}

XmlNode* Message::body() noexcept {
    return const_cast<XmlNode*>(findBody(doc_.root));
}

const XmlNode* Message::body() const noexcept {
    return findBody(doc_.root);
}

std::optional<std::string_view> Message::mosID() const {
    return detail::childText(doc_.root, "mosID");
}

std::optional<std::string_view> Message::ncsID() const {
    return detail::childText(doc_.root, "ncsID");
}

std::optional<std::string_view> Message::messageID() const {
    return detail::childText(doc_.root, "messageID");
}

void Message::setMosID(std::string_view value) {
    detail::setChildText(doc_.root, "mosID", value, kEnvelopeOrder);
}

void Message::setNcsID(std::string_view value) {
    detail::setChildText(doc_.root, "ncsID", value, kEnvelopeOrder);
}

void Message::setMessageID(std::string_view value) {
    detail::setChildText(doc_.root, "messageID", value, kEnvelopeOrder);
}

Result<Message> parseMessage(std::string_view utf8, const XmlParseLimits& limits) {
    Result<XmlDocument> parsed = parseXmlDocument(utf8, limits);
    if (!parsed.ok()) {
        return std::move(parsed).error();
    }
    if (!parsed.value().root.isElement("mos")) {
        return makeError(ErrorCode::ParseError,
                         "root element is <" + parsed.value().root.name + ">, expected <mos>");
    }
    return Message{std::move(parsed).value()};
}

std::string serializeMessage(const Message& message) {
    return serializeXmlDocument(message.document());
}

Message makeMessage(MessageType type) {
    XmlDocument document;
    document.root.name = "mos";
    document.root.emptyTag = true;
    Message message{std::move(document)};

    const MessageInfo& info = messageInfo(type);
    switch (info.envelope) {
        case EnvelopeKind::Full:
            message.setMessageID("");
            [[fallthrough]];
        case EnvelopeKind::IdsOnly:
            // Insert in model order regardless of the setter call order.
            message.setMosID("");
            message.setNcsID("");
            break;
        case EnvelopeKind::None:
            break;
    }
    if (!info.wireName.empty()) {
        message.document().root.appendElement(info.wireName);
    }
    message.refreshType();
    return message;
}

}  // namespace imos::codec
