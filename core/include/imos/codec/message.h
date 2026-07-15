// SPDX-FileCopyrightText: 2026 Javier O. Cordero Pérez
// SPDX-License-Identifier: Apache-2.0
//
// The MOS message: a typed handle over the retained XML tree. The tree is
// the single source of truth — typed accessors and the generated views
// read/write it in place — so unknown content survives every round trip and
// serialization of an unmodified message is byte-clean.

#pragma once

#include <optional>
#include <string>
#include <string_view>

#include <imos/codec/gen/message_types.h>
#include <imos/codec/xml.h>
#include <imos/core_export.h>
#include <imos/error.h>
#include <imos/protocol.h>

namespace imos::codec {

class IMOS_CORE_API Message {
public:
    Message() = default;
    explicit Message(XmlDocument document);

    /// The retained document; root is the <mos> element.
    [[nodiscard]] XmlDocument& document() noexcept { return doc_; }
    [[nodiscard]] const XmlDocument& document() const noexcept { return doc_; }

    [[nodiscard]] MessageType type() const noexcept { return type_; }
    [[nodiscard]] const MessageInfo& info() const noexcept { return messageInfo(type_); }

    /// The message element inside <mos> (first non-envelope element child),
    /// or nullptr for an empty wrapper. For unknown message types this is
    /// still the unrecognized element, retained verbatim.
    [[nodiscard]] XmlNode* body() noexcept;
    [[nodiscard]] const XmlNode* body() const noexcept;

    // Envelope fields. Getters are lenient (absent -> nullopt) because the
    // shapes differ per message family (EnvelopeKind) and dialects stray.
    [[nodiscard]] std::optional<std::string_view> mosID() const;
    [[nodiscard]] std::optional<std::string_view> ncsID() const;
    [[nodiscard]] std::optional<std::string_view> messageID() const;
    void setMosID(std::string_view value);
    void setNcsID(std::string_view value);
    void setMessageID(std::string_view value);

    /// Re-derive the cached message type after direct tree edits.
    void refreshType() noexcept;

private:
    XmlDocument doc_;
    MessageType type_ = MessageType::Unknown;
};

/// Parse one UTF-8 MOS document (root must be <mos>). Unknown message
/// elements yield MessageType::Unknown rather than an error — the spec's
/// unknown-tag rule applies at the message level too; rejecting is a
/// session-layer policy decision.
IMOS_CORE_API Result<Message> parseMessage(std::string_view utf8,
                                           const XmlParseLimits& limits = {});

/// Serialize to UTF-8 bytes (wire encoding is applied by imos/encoding).
[[nodiscard]] IMOS_CORE_API std::string serializeMessage(const Message& message);

/// Construct a canonical skeleton for `type`: <mos> wrapper, envelope
/// children per the message's EnvelopeKind, and an empty body element.
/// Built messages serialize compactly (no indentation).
[[nodiscard]] IMOS_CORE_API Message makeMessage(MessageType type);

}  // namespace imos::codec
