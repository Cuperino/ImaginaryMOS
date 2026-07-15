// SPDX-FileCopyrightText: 2026 Javier O. Cordero Pérez
// SPDX-License-Identifier: Apache-2.0
//
// XmlNode tree helpers and the byte-faithful serializer (imos/codec/xml.h).

#include <imos/codec/xml.h>

#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>

namespace imos::codec {

namespace {

void appendEscapedText(std::string& out, std::string_view text) {
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (c == '&') {
            out += "&amp;";
        } else if (c == '<') {
            out += "&lt;";
        } else if (c == '>' && i >= 2 && text[i - 1] == ']' && text[i - 2] == ']') {
            out += "&gt;";  // "]]>" may not appear literally in character data
        } else {
            out += c;
        }
    }
}

void appendEscapedAttribute(std::string& out, std::string_view value) {
    for (const char c : value) {
        if (c == '&') {
            out += "&amp;";
        } else if (c == '<') {
            out += "&lt;";
        } else if (c == '"') {
            out += "&quot;";
        } else {
            out += c;
        }
    }
}

void serializeNodeInto(std::string& out, const XmlNode& node) {
    switch (node.kind) {
        case XmlNodeKind::Text:
            appendEscapedText(out, node.value);
            return;
        case XmlNodeKind::CData:
            out += "<![CDATA[";
            out += node.value;
            out += "]]>";
            return;
        case XmlNodeKind::Comment:
            out += "<!--";
            out += node.value;
            out += "-->";
            return;
        case XmlNodeKind::ProcessingInstruction:
            out += "<?";
            out += node.name;
            if (!node.value.empty()) {
                out += ' ';
                out += node.value;
            }
            out += "?>";
            return;
        case XmlNodeKind::Element:
            break;
    }
    out += '<';
    out += node.name;
    for (const XmlAttribute& attribute : node.attributes) {
        out += ' ';
        out += attribute.name;
        out += "=\"";
        appendEscapedAttribute(out, attribute.value);
        out += '"';
    }
    if (node.children.empty() && node.emptyTag) {
        out += "/>";
        return;
    }
    out += '>';
    for (const XmlNode& child : node.children) {
        serializeNodeInto(out, child);
    }
    out += "</";
    out += node.name;
    out += '>';
}

std::size_t orderIndex(std::span<const std::string_view> order, std::string_view name) {
    const auto it = std::find(order.begin(), order.end(), name);
    return it == order.end() ? order.size() : static_cast<std::size_t>(it - order.begin());
}

// Insert a new element among the parent's children at its content-model
// position: before the first sibling element that belongs strictly later in
// `order`. Names absent from the model (unknown/retained tags) sort last, so
// new children land before unknown trailing content but never inside it.
XmlNode& insertOrdered(XmlNode& parent, std::string_view name,
                       std::span<const std::string_view> order) {
    const std::size_t rank = orderIndex(order, name);
    auto position = parent.children.end();
    for (auto it = parent.children.begin(); it != parent.children.end(); ++it) {
        if (it->kind != XmlNodeKind::Element) {
            continue;
        }
        if (orderIndex(order, it->name) > rank) {
            position = it;
            break;
        }
    }
    XmlNode child;
    child.name.assign(name.data(), name.size());
    child.emptyTag = true;
    const auto inserted = parent.children.insert(position, std::move(child));
    return *inserted;
}

}  // namespace

std::string XmlNode::text() const {
    if (kind != XmlNodeKind::Element) {
        return value;
    }
    std::string out;
    for (const XmlNode& child : children) {
        if (child.kind == XmlNodeKind::Text || child.kind == XmlNodeKind::CData) {
            out += child.value;
        }
    }
    return out;
}

void XmlNode::setText(std::string_view textValue) {
    children.clear();
    if (!textValue.empty()) {
        appendText(textValue);
    }
    emptyTag = textValue.empty();
}

XmlNode* XmlNode::firstChildElement(std::string_view childName) {
    for (XmlNode& child : children) {
        if (child.isElement(childName)) {
            return &child;
        }
    }
    return nullptr;
}

const XmlNode* XmlNode::firstChildElement(std::string_view childName) const {
    for (const XmlNode& child : children) {
        if (child.isElement(childName)) {
            return &child;
        }
    }
    return nullptr;
}

std::size_t XmlNode::countChildElements(std::string_view childName) const {
    std::size_t count = 0;
    for (const XmlNode& child : children) {
        if (child.isElement(childName)) {
            ++count;
        }
    }
    return count;
}

XmlNode* XmlNode::childElementAt(std::string_view childName, std::size_t index) {
    for (XmlNode& child : children) {
        if (child.isElement(childName)) {
            if (index == 0) {
                return &child;
            }
            --index;
        }
    }
    return nullptr;
}

std::optional<std::string_view> XmlNode::attribute(std::string_view attributeName) const {
    for (const XmlAttribute& entry : attributes) {
        if (entry.name == attributeName) {
            return std::string_view{entry.value};
        }
    }
    return std::nullopt;
}

void XmlNode::setAttribute(std::string_view attributeName, std::string_view attributeValue) {
    for (XmlAttribute& entry : attributes) {
        if (entry.name == attributeName) {
            entry.value.assign(attributeValue.data(), attributeValue.size());
            return;
        }
    }
    attributes.push_back(XmlAttribute{std::string{attributeName}, std::string{attributeValue}});
}

XmlNode& XmlNode::appendElement(std::string_view childName) {
    XmlNode child;
    child.name.assign(childName.data(), childName.size());
    child.emptyTag = true;
    children.push_back(std::move(child));
    emptyTag = false;
    return children.back();
}

XmlNode& XmlNode::appendText(std::string_view textValue) {
    XmlNode child;
    child.kind = XmlNodeKind::Text;
    child.value.assign(textValue.data(), textValue.size());
    children.push_back(std::move(child));
    emptyTag = false;
    return children.back();
}

namespace detail {

std::optional<std::string_view> childText(const XmlNode& parent, std::string_view name) {
    const XmlNode* child = parent.firstChildElement(name);
    if (child == nullptr) {
        return std::nullopt;
    }
    // Views return borrowed text; multi-node character data (CDATA splits)
    // is rare enough that the first text child is the contract here, and
    // XmlNode::text() remains available for the concatenated form.
    for (const XmlNode& sub : child->children) {
        if (sub.kind == XmlNodeKind::Text || sub.kind == XmlNodeKind::CData) {
            return std::string_view{sub.value};
        }
    }
    return std::string_view{};
}

std::optional<std::string_view> childTextAt(const XmlNode& parent, std::string_view name,
                                            std::size_t index) {
    for (const XmlNode& child : parent.children) {
        if (!child.isElement(name)) {
            continue;
        }
        if (index-- == 0) {
            for (const XmlNode& sub : child.children) {
                if (sub.kind == XmlNodeKind::Text || sub.kind == XmlNodeKind::CData) {
                    return std::string_view{sub.value};
                }
            }
            return std::string_view{};
        }
    }
    return std::nullopt;
}

XmlNode& ensureChild(XmlNode& parent, std::string_view name,
                     std::span<const std::string_view> order) {
    if (XmlNode* existing = parent.firstChildElement(name)) {
        return *existing;
    }
    return insertOrdered(parent, name, order);
}

XmlNode& appendChild(XmlNode& parent, std::string_view name,
                     std::span<const std::string_view> order) {
    // Repeated children group together: insert after the last sibling of the
    // same name when one exists, otherwise at the content-model position.
    for (auto it = parent.children.rbegin(); it != parent.children.rend(); ++it) {
        if (it->isElement(name)) {
            XmlNode child;
            child.name.assign(name.data(), name.size());
            child.emptyTag = true;
            const auto inserted = parent.children.insert(it.base(), std::move(child));
            return *inserted;
        }
    }
    return insertOrdered(parent, name, order);
}

void setChildText(XmlNode& parent, std::string_view name, std::string_view value,
                  std::span<const std::string_view> order) {
    ensureChild(parent, name, order).setText(value);
}

XmlNode& appendChildText(XmlNode& parent, std::string_view name, std::string_view value,
                         std::span<const std::string_view> order) {
    XmlNode& child = appendChild(parent, name, order);
    child.setText(value);
    return child;
}

}  // namespace detail

std::string serializeXmlNode(const XmlNode& node) {
    std::string out;
    serializeNodeInto(out, node);
    return out;
}

std::string serializeXmlDocument(const XmlDocument& document) {
    std::string out;
    out += document.leading;
    serializeNodeInto(out, document.root);
    out += document.trailing;
    return out;
}

}  // namespace imos::codec
