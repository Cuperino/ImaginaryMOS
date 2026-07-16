// SPDX-FileCopyrightText: 2026 Javier O. Cordero Pérez
// SPDX-License-Identifier: Apache-2.0

#include <imos/codec/story_body.h>

#include <imos/codec/message.h>

namespace imos::codec {

namespace {

/// ASCII-case-insensitive "true" — the spec spells the attribute
/// Read1stMEMasBody="true"/"false" but dialects vary the case.
bool attributeIsTrue(std::string_view value) noexcept {
    if (value.size() != 4) {
        return false;
    }
    constexpr std::string_view lower = "true";
    for (std::size_t i = 0; i < 4; ++i) {
        const char c = value[i];
        if ((c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c) != lower[i]) {
            return false;
        }
    }
    return true;
}

}  // namespace

ParagraphRuns::ParagraphRuns(const XmlNode& p) {
    runs_.reserve(p.children.size());
    for (const XmlNode& child : p.children) {
        Run run;
        run.node = &child;
        switch (child.kind) {
            case XmlNodeKind::Text:
            case XmlNodeKind::CData:
                run.kind = Kind::Text;
                run.text = child.value;
                break;
            case XmlNodeKind::Element:
                if (child.name == "em") {
                    run.kind = Kind::Em;
                } else if (child.name == "pi") {
                    run.kind = Kind::Pi;
                } else if (child.name == "tab") {
                    run.kind = Kind::Tab;
                } else {
                    run.kind = Kind::Raw;
                }
                if (run.kind != Kind::Raw) {
                    run.text = child.text();
                }
                break;
            case XmlNodeKind::Comment:
            case XmlNodeKind::ProcessingInstruction:
                run.kind = Kind::Raw;
                break;
        }
        runs_.push_back(std::move(run));
    }
}

Result<ResolvedStoryBody> resolveStoryBody(const XmlNode& roStorySend) {
    ResolvedStoryBody resolved;
    resolved.storyBody = roStorySend.firstChildElement("storyBody");
    if (resolved.storyBody == nullptr) {
        return makeError(ErrorCode::ProtocolViolation, "roStorySend has no storyBody");
    }
    const std::optional<std::string_view> attr = resolved.storyBody->attribute("Read1stMEMasBody");
    if (!attr.has_value() || !attributeIsTrue(*attr)) {
        return resolved;  // the literal paragraphs are the body
    }
    // The first MEM block of the roStorySend substitutes for the body. The
    // spec places it as storyBody's sibling (roStorySend's content model);
    // the block itself — mosPayload included — stays a verbatim reference.
    resolved.agencyMetadata = roStorySend.firstChildElement("mosExternalMetadata");
    if (resolved.agencyMetadata == nullptr) {
        return makeError(ErrorCode::ProtocolViolation,
                         "Read1stMEMasBody is true but the roStorySend carries no "
                         "mosExternalMetadata block");
    }
    return resolved;
}

Result<ResolvedStoryBody> resolveStoryBody(const Message& message) {
    if (message.type() != MessageType::RoStorySend) {
        return makeError(ErrorCode::InvalidArgument, "not a roStorySend message");
    }
    const XmlNode* body = message.body();
    if (body == nullptr) {
        return makeError(ErrorCode::InvalidArgument, "roStorySend has no body");
    }
    return resolveStoryBody(*body);
}

}  // namespace imos::codec
