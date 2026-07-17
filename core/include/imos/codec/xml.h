// SPDX-FileCopyrightText: 2026 Javier O. Cordero Pérez
// SPDX-License-Identifier: Apache-2.0
//
// Lossless XML document model of the codec (plan §4.5). Parsing goes through
// libxml2's xmlreader (confined to the implementation; nothing libxml2
// appears here) into this value-type tree, which retains everything needed
// to re-emit the input byte-cleanly: unknown subtrees, comments, processing
// instructions, whitespace text, and the empty-element form (<tag/> vs
// <tag></tag>). This retention is what implements the spec's unknown-tag
// rule and the opaque-payload guarantee — mosPayload subtrees ride through
// unmodified.

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <imos/core_export.h>
#include <imos/error.h>

namespace imos::codec {

enum class XmlNodeKind : std::uint8_t {
    Element,
    Text,
    CData,
    Comment,
    ProcessingInstruction,
};

struct XmlAttribute {
    std::string name;
    std::string value;
};

struct XmlNode {
    XmlNodeKind kind = XmlNodeKind::Element;
    std::string name;   ///< element name or PI target
    std::string value;  ///< content for Text/CData/Comment/PI nodes
    /// True when the source spelled this element <name/>; preserved so the
    /// serializer reproduces the original form byte-cleanly.
    bool emptyTag = false;
    std::vector<XmlAttribute> attributes;
    std::vector<XmlNode> children;

    [[nodiscard]] bool isElement(std::string_view elementName) const noexcept {
        return kind == XmlNodeKind::Element && name == elementName;
    }

    /// Concatenated character data of an element's immediate Text/CData
    /// children (the raw value for non-element nodes). No trimming: field
    /// formats (§6) are applied by higher layers, not the tree.
    [[nodiscard]] IMOS_CORE_API std::string text() const;

    /// Replace this element's children with a single text node (or nothing
    /// when `value` is empty, keeping the element empty).
    IMOS_CORE_API void setText(std::string_view textValue);

    [[nodiscard]] IMOS_CORE_API XmlNode* firstChildElement(std::string_view childName);
    [[nodiscard]] IMOS_CORE_API const XmlNode* firstChildElement(std::string_view childName) const;

    /// The text() of the first child element named `childName`, or "" when no
    /// such child exists (the owning "text or empty" convenience the profile
    /// layers reach for; cf. detail::childText, which returns an optional view
    /// for the generated typed views).
    [[nodiscard]] IMOS_CORE_API std::string childText(std::string_view childName) const;
    [[nodiscard]] IMOS_CORE_API std::size_t countChildElements(std::string_view childName) const;
    [[nodiscard]] IMOS_CORE_API XmlNode* childElementAt(std::string_view childName,
                                                        std::size_t index);

    [[nodiscard]] IMOS_CORE_API std::optional<std::string_view> attribute(
        std::string_view attributeName) const;
    IMOS_CORE_API void setAttribute(std::string_view attributeName, std::string_view value);

    /// Append a child element (returns it). The child starts empty with
    /// emptyTag set so unfilled elements serialize as <name/>.
    IMOS_CORE_API XmlNode& appendElement(std::string_view childName);
    IMOS_CORE_API XmlNode& appendText(std::string_view textValue);
};

namespace detail {

// Helpers backing the generated typed views (imos/codec/gen/views.h).
// `order` is the element's content-model child order; insertion places new
// children before the first sibling that belongs later in the model.
IMOS_CORE_API std::optional<std::string_view> childText(const XmlNode& parent,
                                                        std::string_view name);
IMOS_CORE_API std::optional<std::string_view> childTextAt(const XmlNode& parent,
                                                          std::string_view name, std::size_t index);
IMOS_CORE_API XmlNode& ensureChild(XmlNode& parent, std::string_view name,
                                   std::span<const std::string_view> order);
IMOS_CORE_API XmlNode& appendChild(XmlNode& parent, std::string_view name,
                                   std::span<const std::string_view> order);
IMOS_CORE_API void setChildText(XmlNode& parent, std::string_view name, std::string_view value,
                                std::span<const std::string_view> order);
IMOS_CORE_API XmlNode& appendChildText(XmlNode& parent, std::string_view name,
                                       std::string_view value,
                                       std::span<const std::string_view> order);

}  // namespace detail

/// Hardening caps applied before/while parsing (plan §12). Parsing rejects
/// input over `maxDocumentBytes` outright and aborts beyond `maxDepth`.
struct XmlParseLimits {
    std::size_t maxDocumentBytes = 4U * 1024U * 1024U;
    int maxDepth = 64;
};

/// A parsed document: the element tree plus the verbatim bytes around the
/// root element (XML declaration, prolog/epilog comments and whitespace),
/// kept as raw text so serialization is byte-clean.
struct XmlDocument {
    std::string leading;
    XmlNode root;
    std::string trailing;
};

/// Parse UTF-8 XML. libxml2 runs hardened: no network, no DTD loading, no
/// entity substitution (plan §12); well-formedness is required, validity is
/// not (strict/lenient duality, plan §4.1).
IMOS_CORE_API Result<XmlDocument> parseXmlDocument(std::string_view utf8,
                                                   const XmlParseLimits& limits = {});

/// Serialize back to UTF-8. Escaping is minimal ('&', '<', and "]]>" in
/// text; '&', '<', '"' in attribute values) so retained content re-emits
/// byte-identically wherever the source did not use optional escapes.
[[nodiscard]] IMOS_CORE_API std::string serializeXmlDocument(const XmlDocument& document);
[[nodiscard]] IMOS_CORE_API std::string serializeXmlNode(const XmlNode& node);

}  // namespace imos::codec
