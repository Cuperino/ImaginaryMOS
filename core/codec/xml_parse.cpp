// SPDX-FileCopyrightText: 2026 Javier O. Cordero Pérez
// SPDX-License-Identifier: Apache-2.0
//
// XML parsing via libxml2's xmlreader (stream/reader mode per plan §4.5),
// hardened per plan §12: no network, no DTD loading, no entity substitution,
// hard caps on document size and nesting depth. libxml2 stays confined to
// this translation unit.

#include <imos/codec/xml.h>

#include <climits>
#include <cstddef>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include <libxml/parser.h>
#include <libxml/xmlreader.h>

namespace imos::codec {

namespace {

constexpr int kParseOptions = XML_PARSE_NONET;

constexpr bool isXmlWhitespace(char c) noexcept {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

struct ErrorCapture {
    std::string first;
};

void captureStructuredError(void* userData, const xmlError* error) {
    auto* capture = static_cast<ErrorCapture*>(userData);
    if (capture == nullptr || error == nullptr || error->message == nullptr) {
        return;
    }
    if (capture->first.empty()) {
        capture->first = "line " + std::to_string(error->line) + ": " + error->message;
        while (!capture->first.empty() && capture->first.back() == '\n') {
            capture->first.pop_back();
        }
    }
}

std::string_view readerName(xmlTextReaderPtr reader) {
    const xmlChar* name = xmlTextReaderConstName(reader);
    return name == nullptr ? std::string_view{}
                           : std::string_view{reinterpret_cast<const char*>(name)};
}

std::string_view readerValue(xmlTextReaderPtr reader) {
    const xmlChar* value = xmlTextReaderConstValue(reader);
    return value == nullptr ? std::string_view{}
                            : std::string_view{reinterpret_cast<const char*>(value)};
}

/// Verbatim prolog before the root element start tag: XML declaration,
/// comments, PIs, whitespace. The input is already known well-formed.
std::string_view scanLeading(std::string_view utf8) {
    std::size_t pos = 0;
    while (pos < utf8.size()) {
        if (isXmlWhitespace(utf8[pos])) {
            ++pos;
            continue;
        }
        if (utf8.substr(pos).starts_with("<!--")) {
            const std::size_t end = utf8.find("-->", pos + 4);
            pos = end == std::string_view::npos ? utf8.size() : end + 3;
            continue;
        }
        if (utf8.substr(pos).starts_with("<?")) {
            const std::size_t end = utf8.find("?>", pos + 2);
            pos = end == std::string_view::npos ? utf8.size() : end + 2;
            continue;
        }
        break;  // root start tag
    }
    return utf8.substr(0, pos);
}

/// Verbatim epilog after the root element: whitespace, comments, PIs.
std::string_view scanTrailing(std::string_view utf8, std::size_t leadingSize) {
    std::size_t end = utf8.size();
    while (end > leadingSize) {
        if (isXmlWhitespace(utf8[end - 1])) {
            --end;
            continue;
        }
        if (end >= 3 && utf8.substr(end - 3, 3) == "-->") {
            const std::size_t open = utf8.rfind("<!--", end - 3);
            if (open == std::string_view::npos || open < leadingSize) {
                break;
            }
            end = open;
            continue;
        }
        if (end >= 2 && utf8.substr(end - 2, 2) == "?>") {
            const std::size_t open = utf8.rfind("<?", end - 2);
            if (open == std::string_view::npos || open < leadingSize) {
                break;
            }
            end = open;
            continue;
        }
        break;  // root end tag '>'
    }
    return utf8.substr(end);
}

void initLibxml2Once() {
    static std::once_flag flag;
    std::call_once(flag, [] { xmlInitParser(); });
}

}  // namespace

Result<XmlDocument> parseXmlDocument(std::string_view utf8, const XmlParseLimits& limits) {
    if (utf8.size() > limits.maxDocumentBytes) {
        return makeError(ErrorCode::LimitExceeded, "document of " + std::to_string(utf8.size()) +
                                                       " bytes exceeds the configured cap of " +
                                                       std::to_string(limits.maxDocumentBytes));
    }
    if (utf8.size() > static_cast<std::size_t>(INT_MAX)) {
        return makeError(ErrorCode::LimitExceeded, "document too large for the XML parser");
    }
    initLibxml2Once();

    // Wire payloads are decoded to UTF-8 before parsing (plan §3.4), so the
    // reader is told UTF-8 explicitly rather than trusting any declaration.
    xmlTextReaderPtr reader = xmlReaderForMemory(utf8.data(), static_cast<int>(utf8.size()),
                                                 nullptr, "UTF-8", kParseOptions);
    if (reader == nullptr) {
        return makeError(ErrorCode::ParseError, "failed to create XML reader");
    }
    ErrorCapture capture;
    xmlTextReaderSetStructuredErrorHandler(reader, captureStructuredError, &capture);

    XmlDocument document;
    bool haveRoot = false;
    std::vector<XmlNode*> stack;
    Error failure{};

    int status = 0;
    while ((status = xmlTextReaderRead(reader)) == 1) {
        const int nodeType = xmlTextReaderNodeType(reader);
        if (xmlTextReaderDepth(reader) > limits.maxDepth) {
            failure = makeError(ErrorCode::LimitExceeded,
                                "element nesting exceeds the configured depth cap of " +
                                    std::to_string(limits.maxDepth));
            break;
        }
        switch (nodeType) {
            case XML_READER_TYPE_ELEMENT: {
                XmlNode* node = nullptr;
                if (stack.empty()) {
                    if (haveRoot) {
                        failure = makeError(ErrorCode::ParseError, "multiple root elements");
                        break;
                    }
                    haveRoot = true;
                    node = &document.root;
                } else {
                    stack.back()->children.emplace_back();
                    node = &stack.back()->children.back();
                }
                node->kind = XmlNodeKind::Element;
                node->name = readerName(reader);
                node->emptyTag = xmlTextReaderIsEmptyElement(reader) == 1;
                if (xmlTextReaderHasAttributes(reader) == 1 &&
                    xmlTextReaderMoveToFirstAttribute(reader) == 1) {
                    do {
                        node->attributes.push_back(XmlAttribute{std::string{readerName(reader)},
                                                                std::string{readerValue(reader)}});
                    } while (xmlTextReaderMoveToNextAttribute(reader) == 1);
                    xmlTextReaderMoveToElement(reader);
                }
                if (!node->emptyTag) {
                    stack.push_back(node);
                }
                break;
            }
            case XML_READER_TYPE_END_ELEMENT:
                if (!stack.empty()) {
                    stack.pop_back();
                }
                break;
            case XML_READER_TYPE_TEXT:
            case XML_READER_TYPE_WHITESPACE:
            case XML_READER_TYPE_SIGNIFICANT_WHITESPACE:
                if (!stack.empty()) {
                    XmlNode& child = stack.back()->children.emplace_back();
                    child.kind = XmlNodeKind::Text;
                    child.value = readerValue(reader);
                }
                break;
            case XML_READER_TYPE_CDATA:
                if (!stack.empty()) {
                    XmlNode& child = stack.back()->children.emplace_back();
                    child.kind = XmlNodeKind::CData;
                    child.value = readerValue(reader);
                }
                break;
            case XML_READER_TYPE_COMMENT:
                if (!stack.empty()) {
                    XmlNode& child = stack.back()->children.emplace_back();
                    child.kind = XmlNodeKind::Comment;
                    child.value = readerValue(reader);
                }
                break;  // outside the root it lives in leading/trailing
            case XML_READER_TYPE_PROCESSING_INSTRUCTION:
                if (!stack.empty()) {
                    XmlNode& child = stack.back()->children.emplace_back();
                    child.kind = XmlNodeKind::ProcessingInstruction;
                    child.name = readerName(reader);
                    child.value = readerValue(reader);
                }
                break;
            case XML_READER_TYPE_DOCUMENT_TYPE:
                failure = makeError(ErrorCode::LimitExceeded,
                                    "DOCTYPE is not allowed in MOS messages (hardening)");
                break;
            case XML_READER_TYPE_ENTITY_REFERENCE:
                failure = makeError(ErrorCode::ParseError,
                                    "unresolvable entity reference (entity substitution is "
                                    "disabled)");
                break;
            default:
                break;
        }
        if (failure.code != ErrorCode::Ok) {
            break;
        }
    }

    xmlFreeTextReader(reader);

    if (failure.code != ErrorCode::Ok) {
        return failure;
    }
    if (status < 0 || !capture.first.empty()) {
        return makeError(ErrorCode::ParseError,
                         capture.first.empty() ? std::string{"malformed XML"} : capture.first);
    }
    if (!haveRoot) {
        return makeError(ErrorCode::ParseError, "no root element");
    }

    const std::string_view leading = scanLeading(utf8);
    document.leading = leading;
    document.trailing = scanTrailing(utf8, leading.size());
    return document;
}

}  // namespace imos::codec
