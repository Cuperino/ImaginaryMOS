// SPDX-FileCopyrightText: 2026 Javier O. Cordero Pérez
// SPDX-License-Identifier: Apache-2.0
//
// The classic-MOS ⇄ 3.8.2-WSDL shape transform (m6-plan §2). Everything is
// table-driven off the WSDL fixture's pinned surface: per-operation recipes
// (header element, hoisted fields, input wrapper), the ArrayOfX container
// set, and the handful of renames. The ws3 unit suite cross-checks these
// tables against fixtures/wsdl/MOSWebService-3.8.2.wsdl so drift fails.

#include <imos/transports/ws3_transform.h>

#include <algorithm>
#include <array>
#include <string>
#include <utility>
#include <vector>

namespace imos::transports::ws3 {

namespace {

using codec::Message;
using codec::MessageType;
using codec::XmlNode;
using codec::XmlNodeKind;

std::string_view localName(std::string_view name) {
    const std::size_t colon = name.rfind(':');
    return colon == std::string_view::npos ? name : name.substr(colon + 1);
}

XmlNode* findChildLocal(XmlNode& parent, std::string_view name) {
    for (XmlNode& child : parent.children) {
        if (child.kind == XmlNodeKind::Element && localName(child.name) == name) {
            return &child;
        }
    }
    return nullptr;
}

XmlNode makeElement(std::string_view name) {
    XmlNode node;
    node.kind = XmlNodeKind::Element;
    node.name = std::string{name};
    node.emptyTag = true;
    return node;
}

void appendTextChild(XmlNode& parent, std::string_view name, std::string_view text) {
    XmlNode& child = parent.appendElement(name);
    child.setText(text);
    parent.emptyTag = false;
}

// ------------------------------------------------------------- recipes ----

/// A field the WSDL hoists out of the classic body onto the operation
/// wrapper (as a sibling of the input element).
struct HoistRule {
    const char* soapName = nullptr;
    const char* classicName = nullptr;
    bool fromAttribute = false;
};

struct OpRecipe {
    MessageType request = MessageType::Unknown;
    /// The mosHeader element name — "mosHeader_input" everywhere except
    /// roDelete's bare "mosHeader" (a WSDL quirk, kept verbatim).
    const char* header = "mosHeader_input";
    /// The <op>_input element holding the remaining classic children;
    /// nullptr when the operation has hoists only.
    const char* inputWrapper = nullptr;
    /// When set, the input wrapper's content is this classic *child*
    /// subtree rather than the body's remaining children (mosItemReplace:
    /// the <item> child becomes item_input).
    const char* contentFromChild = nullptr;
    std::array<HoistRule, 3> hoists{};
    /// Elements inside the input wrapper whose classic form is an
    /// *attribute* on the body element (the WSDL has no attributes).
    std::array<const char*, 2> inputAttributes{};
    /// The classic reply the response carries (data type, or the ack).
    MessageType reply = MessageType::MosAck;
    /// Second result element when the WSDL pairs a data result with an ack
    /// result (mosReqObj/roReq/roReqAll); nullptr otherwise.
    const char* ackResult = nullptr;
};

const std::array<OpRecipe, 24>& recipes() {
    static const std::array<OpRecipe, 24> kRecipes = {{
        {MessageType::Heartbeat,
         "mosHeader_input",
         "heartbeat_input",
         nullptr,
         {},
         {},
         MessageType::Heartbeat,
         nullptr},
        {MessageType::ReqMachInfo,
         "mosHeader_input",
         nullptr,
         nullptr,
         {},
         {},
         MessageType::ListMachInfo,
         nullptr},
        {MessageType::MosObj,
         "mosHeader_input",
         "mosObj_input",
         nullptr,
         {},
         {},
         MessageType::MosAck,
         nullptr},
        {MessageType::MosReqObj,
         "mosHeader_input",
         nullptr,
         nullptr,
         {{{"objID", "objID", false}}},
         {},
         MessageType::MosObj,
         "mosAckResult"},
        {MessageType::MosReqAll,
         "mosHeader_input",
         nullptr,
         nullptr,
         {{{"pause_input", "pause", false}}},
         {},
         MessageType::MosAck,
         nullptr},
        {MessageType::MosListAll,
         "mosHeader_input",
         "mosListAll_input",
         nullptr,
         {},
         {},
         MessageType::MosAck,
         nullptr},
        {MessageType::MosObjCreate,
         "mosHeader_input",
         "mosObjCreate_input",
         nullptr,
         {},
         {},
         MessageType::MosAck,
         nullptr},
        {MessageType::MosReqObjList,
         "mosHeader_input",
         "mosReqObjList_input",
         nullptr,
         {{{"username", "username", true}}},
         {},
         MessageType::MosObjList,
         nullptr},
        {MessageType::MosReqSearchableSchema,
         "mosHeader_input",
         nullptr,
         nullptr,
         {{{"username", "username", true}}},
         {},
         MessageType::MosListSearchableSchema,
         nullptr},
        {MessageType::MosReqObjAction,
         "mosHeader_input",
         "mosReqObjAction_input",
         nullptr,
         {},
         {{"operation", "objID"}},
         MessageType::MosAck,
         nullptr},
        {MessageType::RoCreate,
         "mosHeader_input",
         "roCreate_input",
         nullptr,
         {},
         {},
         MessageType::RoAck,
         nullptr},
        {MessageType::RoReplace,
         "mosHeader_input",
         "roReplace_input",
         nullptr,
         {},
         {},
         MessageType::RoAck,
         nullptr},
        {MessageType::RoDelete,
         "mosHeader",
         nullptr,
         nullptr,
         {{{"roID", "roID", false}}},
         {},
         MessageType::RoAck,
         nullptr},
        {MessageType::RoReq,
         "mosHeader_input",
         nullptr,
         nullptr,
         {{{"roID", "roID", false}}},
         {},
         MessageType::RoList,
         "roAckResult"},
        {MessageType::RoMetadataReplace,
         "mosHeader_input",
         "roMetadataReplace_input",
         nullptr,
         {},
         {},
         MessageType::RoAck,
         nullptr},
        {MessageType::RoElementAction,
         "mosHeader_input",
         "roElementAction_input",
         nullptr,
         {{{"operation", "operation", true}}},
         {},
         MessageType::RoAck,
         nullptr},
        {MessageType::RoElementStat,
         "mosHeader_input",
         "roElementStat_input",
         nullptr,
         {{{"element", "element", true}}},
         {},
         MessageType::RoAck,
         nullptr},
        {MessageType::RoReadyToAir,
         "mosHeader_input",
         nullptr,
         nullptr,
         {{{"roID", "roID", false}, {"roAir", "roAir", false}}},
         {},
         MessageType::RoAck,
         nullptr},
        {MessageType::MosItemReplace,
         "mosHeader_input",
         "item_input",
         "item",
         {{{"roID", "roID", false}, {"storyID", "storyID", false}}},
         {},
         MessageType::RoAck,
         nullptr},
        {MessageType::RoReqAll,
         "mosHeader_input",
         nullptr,
         nullptr,
         {},
         {},
         MessageType::RoListAll,
         "roAckResult"},
        {MessageType::RoStorySend,
         "mosHeader_input",
         "roStorySend_input",
         nullptr,
         {},
         {},
         MessageType::RoAck,
         nullptr},
        {MessageType::RoItemCue,
         "mosHeader_input",
         "roItemCue_input",
         nullptr,
         {},
         {},
         MessageType::RoAck,
         nullptr},
        {MessageType::RoCtrl,
         "mosHeader_input",
         "roCtrl_input",
         nullptr,
         {},
         {},
         MessageType::RoAck,
         nullptr},
        {MessageType::RoReqStoryAction,
         "mosHeader_input",
         "roStorySend_input",
         nullptr,
         {{{"operation", "operation", true},
           {"leaseLock", "leaseLock", true},
           {"username", "username", true}}},
         {},
         MessageType::RoAck,
         nullptr},
    }};
    return kRecipes;
}

const OpRecipe* recipeFor(MessageType type) {
    for (const OpRecipe& recipe : recipes()) {
        if (recipe.request == type) {
            return &recipe;
        }
    }
    return nullptr;
}

const OpRecipe* recipeForOp(std::string_view operation) {
    const MessageType type = codec::messageTypeFromName(operation);
    return type == MessageType::Unknown ? nullptr : recipeFor(type);
}

std::string opName(const OpRecipe& recipe) {
    return std::string{codec::messageInfo(recipe.request).wireName};
}

// -------------------------------------------- array containers, renames ----

/// The .NET ArrayOfX pattern of the 3.8.2 schema: classic repeated <x>
/// elements become one container holding <x_type> members. The container
/// almost always keeps the classic name; item's container is "Item".
struct ArrayRule {
    const char* classicName;
    const char* container;
    const char* member;
};

constexpr std::array<ArrayRule, 8> kArrayRules = {{
    {"mosExternalMetadata", "mosExternalMetadata", "mosExternalMetadata_type"},
    {"story", "story", "story_type"},
    {"item", "Item", "item_type"},
    {"objPath", "objPath", "objPath_type"},
    {"objProxyPath", "objProxyPath", "objProxyPath_type"},
    {"searchField", "searchField", "searchField_type"},
    {"defaultActiveX", "defaultActiveX", "ActiveX_type"},
    // mosListAll's batch (ArrayOfMosObj_type). Safe as a generic rule: the
    // <list>-of-<mosObj> shape (mosObjList) is renamed to mosList with
    // mosObj_type members *before* the grouping pass sees it.
    {"mosObj", "mosObj", "mosObj_type"},
}};

void classicToSoapTree(XmlNode& parent);

/// Group all children of `parent` named `rule.classicName` into one
/// container at the first occurrence.
void groupChildren(XmlNode& parent, const ArrayRule& rule) {
    std::size_t first = parent.children.size();
    std::vector<XmlNode> members;
    for (std::size_t i = 0; i < parent.children.size();) {
        XmlNode& child = parent.children[i];
        if (child.kind == XmlNodeKind::Element && child.name == rule.classicName) {
            if (first == parent.children.size()) {
                first = i;
            }
            XmlNode member = makeElement(rule.member);
            member.attributes = std::move(child.attributes);
            member.children = std::move(child.children);
            member.emptyTag = member.children.empty();
            members.push_back(std::move(member));
            parent.children.erase(parent.children.begin() + static_cast<std::ptrdiff_t>(i));
        } else {
            ++i;
        }
    }
    if (members.empty()) {
        return;
    }
    XmlNode container = makeElement(rule.container);
    container.children = std::move(members);
    container.emptyTag = false;
    parent.children.insert(parent.children.begin() + static_cast<std::ptrdiff_t>(first),
                           std::move(container));
}

void classicToSoapTree(XmlNode& parent) {
    // Renames and special containers first, then the generic array grouping,
    // then recursion (members were classic subtrees and get their own pass).
    for (XmlNode& child : parent.children) {
        if (child.kind != XmlNodeKind::Element) {
            continue;
        }
        if (child.name == "element_target") {
            child.name = "element_target_type";
        } else if (child.name == "ro") {
            child.name = "ro_type";  // roListAll's blocks (ArrayOfRo_type)
        } else if (child.name == "list") {
            // mosObjList: classic <list> of <mosObj> ⇄ <mosList> of
            // <mosObj_type> (container kept, both names change).
            child.name = "mosList";
            for (XmlNode& entry : child.children) {
                if (entry.isElement("mosObj")) {
                    entry.name = "mosObj_type";
                }
            }
        }
    }
    for (const ArrayRule& rule : kArrayRules) {
        groupChildren(parent, rule);
    }
    for (XmlNode& child : parent.children) {
        if (child.kind != XmlNodeKind::Element) {
            continue;
        }
        const bool isContainer =
            std::any_of(kArrayRules.begin(), kArrayRules.end(),
                        [&](const ArrayRule& rule) { return child.name == rule.container; });
        if (isContainer) {
            for (XmlNode& member : child.children) {
                classicToSoapTree(member);
            }
        } else {
            classicToSoapTree(child);
        }
    }
}

void soapToClassicTree(XmlNode& parent) {
    for (std::size_t i = 0; i < parent.children.size();) {
        XmlNode& child = parent.children[i];
        if (child.kind != XmlNodeKind::Element) {
            ++i;
            continue;
        }
        const std::string_view name = localName(child.name);
        if (name == "element_target_type") {
            child.name = "element_target";
            soapToClassicTree(child);
            ++i;
            continue;
        }
        if (name == "ro_type") {
            child.name = "ro";
            soapToClassicTree(child);
            ++i;
            continue;
        }
        if (name == "mosList") {
            child.name = "list";
            for (XmlNode& entry : child.children) {
                if (localName(entry.name) == "mosObj_type") {
                    entry.name = "mosObj";
                }
            }
            soapToClassicTree(child);
            ++i;
            continue;
        }
        const ArrayRule* matched = nullptr;
        for (const ArrayRule& rule : kArrayRules) {
            if (name == rule.container) {
                matched = &rule;
                break;
            }
        }
        const bool explode = matched != nullptr && !child.children.empty() &&
                             std::all_of(child.children.begin(), child.children.end(),
                                         [&](const XmlNode& member) {
                                             return member.kind != XmlNodeKind::Element ||
                                                    localName(member.name) == matched->member;
                                         }) &&
                             findChildLocal(child, matched->member) != nullptr;
        if (!explode) {
            soapToClassicTree(child);
            ++i;
            continue;
        }
        std::vector<XmlNode> replacements;
        for (XmlNode& member : child.children) {
            if (member.kind != XmlNodeKind::Element) {
                continue;
            }
            XmlNode classic = makeElement(matched->classicName);
            classic.attributes = std::move(member.attributes);
            classic.children = std::move(member.children);
            classic.emptyTag = classic.children.empty();
            soapToClassicTree(classic);
            replacements.push_back(std::move(classic));
        }
        const auto at = parent.children.begin() + static_cast<std::ptrdiff_t>(i);
        parent.children.erase(at);
        parent.children.insert(parent.children.begin() + static_cast<std::ptrdiff_t>(i),
                               std::make_move_iterator(replacements.begin()),
                               std::make_move_iterator(replacements.end()));
        i += replacements.size();
    }
}

// ------------------------------------------------- listMachInfo mapping ----

constexpr std::array<const char*, 10> kMachInfoScalars = {
    "manufacturer", "model", "hwRev", "swRev", "DOM", "SN", "ID", "time", "opTime", "mosRev",
};

/// classic <listMachInfo> children → MachInfo_type children: profiles are
/// flattened onto mosProfile0..7 (the deviceType attribute has no slot in
/// the 3.8.2 schema and is dropped on this leg).
XmlNode machInfoToSoap(const XmlNode& classicBody) {
    XmlNode result = makeElement("placeholder");
    for (const char* scalar : kMachInfoScalars) {
        if (const XmlNode* child = classicBody.firstChildElement(scalar)) {
            appendTextChild(result, scalar, child->text());
        }
    }
    if (const XmlNode* profiles = classicBody.firstChildElement("supportedProfiles")) {
        for (const XmlNode& profile : profiles->children) {
            if (!profile.isElement("mosProfile")) {
                continue;
            }
            const std::optional<std::string_view> number = profile.attribute("number");
            if (number.has_value() && number->size() == 1 && *number >= "0" && *number <= "7") {
                appendTextChild(result, "mosProfile" + std::string{*number}, profile.text());
            }
        }
    }
    for (const XmlNode& child : classicBody.children) {
        if (child.isElement("defaultActiveX")) {
            result.children.push_back(child);
            result.emptyTag = false;
        }
    }
    classicToSoapTree(result);
    return result;
}

/// MachInfo_type children → classic <listMachInfo> children.
void machInfoToClassic(XmlNode& result, XmlNode& classicBody) {
    soapToClassicTree(result);
    for (const char* scalar : kMachInfoScalars) {
        if (XmlNode* child = findChildLocal(result, scalar)) {
            appendTextChild(classicBody, scalar, child->text());
        }
    }
    XmlNode profiles = makeElement("supportedProfiles");
    for (int i = 0; i <= 7; ++i) {
        if (XmlNode* flag = findChildLocal(result, "mosProfile" + std::to_string(i))) {
            XmlNode& entry = profiles.appendElement("mosProfile");
            entry.setAttribute("number", std::to_string(i));
            entry.setText(flag->text());
            profiles.emptyTag = false;
        }
    }
    if (!profiles.children.empty()) {
        classicBody.children.push_back(std::move(profiles));
    }
    for (XmlNode& child : result.children) {
        if (child.kind == XmlNodeKind::Element && localName(child.name) == "defaultActiveX") {
            child.name = "defaultActiveX";
            classicBody.children.push_back(std::move(child));
        }
    }
    classicBody.emptyTag = classicBody.children.empty();
}

// ------------------------------------------------------ SOAP envelopes ----

std::string wrapEnvelope(const XmlNode& bodyChild) {
    return "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
           "<soap:Envelope xmlns:soap=\"" +
           std::string{kSoapEnvelopeNs} + "\"><soap:Body>" + codec::serializeXmlNode(bodyChild) +
           "</soap:Body></soap:Envelope>";
}

/// The first element child of soap:Body, copied out of the parsed document.
Result<XmlNode> unwrapEnvelope(std::string_view envelope) {
    Result<codec::XmlDocument> parsed = codec::parseXmlDocument(envelope);
    if (!parsed.ok()) {
        return std::move(parsed).error();
    }
    XmlNode& root = parsed.value().root;
    if (localName(root.name) != "Envelope") {
        return makeError(ErrorCode::ProtocolViolation,
                         "expected a SOAP Envelope, got <" + root.name + ">");
    }
    XmlNode* body = findChildLocal(root, "Body");
    if (body == nullptr) {
        return makeError(ErrorCode::ProtocolViolation, "SOAP Envelope has no Body");
    }
    for (XmlNode& child : body->children) {
        if (child.kind == XmlNodeKind::Element) {
            return std::move(child);
        }
    }
    return makeError(ErrorCode::ProtocolViolation, "SOAP Body is empty");
}

void fillHeader(XmlNode& header, const Message& message) {
    appendTextChild(header, "mosID", message.mosID().value_or(""));
    appendTextChild(header, "ncsID", message.ncsID().value_or(""));
    appendTextChild(header, "messageID", message.messageID().value_or(""));
}

/// Synthesized ack element for the dual-result responses (m6-plan §2.2):
/// when the session's reply is the data message, the ack half is implicit.
XmlNode synthesizedAck(const OpRecipe& recipe, const XmlNode* dataBody) {
    XmlNode ack = makeElement(recipe.ackResult);
    if (std::string_view{recipe.ackResult} == "roAckResult") {
        appendTextChild(ack, "roID",
                        dataBody != nullptr ? dataBody->childText("roID") : std::string{});
        appendTextChild(ack, "roStatus", "OK");
    } else {
        appendTextChild(ack, "objID",
                        dataBody != nullptr ? dataBody->childText("objID") : std::string{});
        appendTextChild(ack, "status", "ACK");
    }
    return ack;
}

}  // namespace

// ---------------------------------------------------------- public API ----

bool isWs3Operation(codec::MessageType type) noexcept {
    return recipeFor(type) != nullptr;
}

Result<SoapRequest> buildSoapRequest(const Message& message) {
    const OpRecipe* recipe = recipeFor(message.type());
    if (recipe == nullptr) {
        return makeError(ErrorCode::Unsupported, "<" + std::string{message.info().wireName} +
                                                     "> is not an operation of the 3.8.2 WSDL");
    }
    const std::string op = opName(*recipe);

    XmlNode wrapper = makeElement(op);
    wrapper.setAttribute("xmlns", kMosWebServicesNs);
    XmlNode& header = wrapper.appendElement(recipe->header);
    fillHeader(header, message);
    wrapper.emptyTag = false;

    // Work on a copy of the body: hoists consume fields, the rest becomes
    // the input wrapper's content.
    XmlNode work = message.body() != nullptr ? *message.body() : makeElement(op);
    for (const HoistRule& hoist : recipe->hoists) {
        if (hoist.soapName == nullptr) {
            break;
        }
        if (hoist.fromAttribute) {
            const std::optional<std::string_view> value = work.attribute(hoist.classicName);
            if (value.has_value()) {
                appendTextChild(wrapper, hoist.soapName, *value);
            }
            continue;
        }
        for (std::size_t i = 0; i < work.children.size(); ++i) {
            if (work.children[i].isElement(hoist.classicName)) {
                XmlNode hoisted = std::move(work.children[i]);
                work.children.erase(work.children.begin() + static_cast<std::ptrdiff_t>(i));
                hoisted.name = hoist.soapName;
                wrapper.children.push_back(std::move(hoisted));
                break;
            }
        }
    }
    if (recipe->inputWrapper != nullptr) {
        XmlNode input = makeElement(recipe->inputWrapper);
        XmlNode* content = &work;
        if (recipe->contentFromChild != nullptr) {
            content = work.firstChildElement(recipe->contentFromChild);
        }
        if (content != nullptr) {
            // Classic attributes have no SOAP slot; they become leading
            // elements of the input (the schema models them that way —
            // mosReqObjAction's operation/objID).
            for (const codec::XmlAttribute& attribute : content->attributes) {
                appendTextChild(input, attribute.name, attribute.value);
            }
            input.children.insert(input.children.end(),
                                  std::make_move_iterator(content->children.begin()),
                                  std::make_move_iterator(content->children.end()));
            input.emptyTag = input.children.empty();
        }
        classicToSoapTree(input);
        wrapper.children.push_back(std::move(input));
    }

    SoapRequest request;
    request.operation = op;
    request.soapAction =
        std::string{kMosWebServicesNs.substr(0, kMosWebServicesNs.size() - 1)} + "/" + op;
    request.envelope = wrapEnvelope(wrapper);
    request.messageId = std::string{message.messageID().value_or("")};
    return request;
}

Result<InboundCall> parseSoapRequest(std::string_view envelope) {
    Result<XmlNode> unwrapped = unwrapEnvelope(envelope);
    if (!unwrapped.ok()) {
        return std::move(unwrapped).error();
    }
    XmlNode wrapper = std::move(unwrapped).value();
    const std::string op{localName(wrapper.name)};
    const OpRecipe* recipe = recipeForOp(op);
    if (recipe == nullptr) {
        return makeError(ErrorCode::UnknownMessage, "<" + op + "> is not a WSDL operation");
    }

    Message message = codec::makeMessage(recipe->request);
    XmlNode* header = findChildLocal(wrapper, recipe->header);
    if (header == nullptr) {
        header = findChildLocal(wrapper, "mosHeader_input");
    }
    if (header == nullptr) {
        header = findChildLocal(wrapper, "mosHeader");
    }
    std::string messageId;
    if (header != nullptr) {
        message.setMosID(header->childText("mosID"));
        message.setNcsID(header->childText("ncsID"));
        messageId = header->childText("messageID");
        message.setMessageID(messageId);
    }

    XmlNode* body = message.body();
    if (body == nullptr) {
        return makeError(ErrorCode::ProtocolViolation, "message skeleton has no body");
    }
    for (const HoistRule& hoist : recipe->hoists) {
        if (hoist.soapName == nullptr) {
            break;
        }
        XmlNode* hoisted = findChildLocal(wrapper, hoist.soapName);
        if (hoisted == nullptr) {
            continue;
        }
        if (hoist.fromAttribute) {
            body->setAttribute(hoist.classicName, hoisted->text());
        } else {
            appendTextChild(*body, hoist.classicName, hoisted->text());
        }
    }
    if (recipe->inputWrapper != nullptr) {
        if (XmlNode* input = findChildLocal(wrapper, recipe->inputWrapper)) {
            soapToClassicTree(*input);
            for (const char* attributeName : recipe->inputAttributes) {
                if (attributeName == nullptr) {
                    break;
                }
                for (std::size_t i = 0; i < input->children.size(); ++i) {
                    XmlNode& element = input->children[i];
                    if (element.kind == XmlNodeKind::Element &&
                        localName(element.name) == attributeName) {
                        body->setAttribute(attributeName, element.text());
                        input->children.erase(input->children.begin() +
                                              static_cast<std::ptrdiff_t>(i));
                        break;
                    }
                }
            }
            XmlNode* destination = body;
            if (recipe->contentFromChild != nullptr) {
                destination = &body->appendElement(recipe->contentFromChild);
            }
            destination->children.insert(destination->children.end(),
                                         std::make_move_iterator(input->children.begin()),
                                         std::make_move_iterator(input->children.end()));
            destination->emptyTag = destination->children.empty();
            body->emptyTag = body->children.empty();
        }
    }
    message.refreshType();

    InboundCall call;
    call.message = std::move(message);
    call.operation = op;
    call.messageId = std::move(messageId);
    return call;
}

Result<std::string> buildSoapResponse(const Message& reply, std::string_view operation) {
    const OpRecipe* recipe = recipeForOp(operation);
    if (recipe == nullptr) {
        return makeError(ErrorCode::UnknownMessage,
                         "<" + std::string{operation} + "> is not a WSDL operation");
    }
    const std::string op = opName(*recipe);
    XmlNode wrapper = makeElement(op + "Response");
    wrapper.setAttribute("xmlns", kMosWebServicesNs);
    XmlNode result = makeElement(op + "Result");

    const MessageType replyType = reply.type();
    const XmlNode* replyBody = reply.body();
    const bool isAckReply = replyType == MessageType::MosAck || replyType == MessageType::RoAck;

    if (replyType == recipe->reply && !isAckReply) {
        // Data reply → data result (+ implicit-ack result where paired).
        if (replyType == MessageType::ListMachInfo) {
            XmlNode converted =
                replyBody != nullptr ? machInfoToSoap(*replyBody) : makeElement("x");
            result.children = std::move(converted.children);
        } else if (replyBody != nullptr) {
            XmlNode content = *replyBody;
            classicToSoapTree(content);
            result.attributes = content.attributes;
            result.children = std::move(content.children);
        }
        result.emptyTag = result.children.empty();
        wrapper.children.push_back(std::move(result));
        if (recipe->ackResult != nullptr) {
            wrapper.children.push_back(synthesizedAck(*recipe, replyBody));
        }
    } else if (isAckReply) {
        if (recipe->reply == replyType || recipe->ackResult == nullptr) {
            // Ack-only operation (or a NACK where the WSDL has just the one
            // result slot): the ack rides the result element.
            if (replyBody != nullptr) {
                result.children = replyBody->children;
                result.emptyTag = result.children.empty();
            }
            wrapper.children.push_back(std::move(result));
        } else {
            // NACK on a dual-result operation: empty data result, real ack.
            wrapper.children.push_back(std::move(result));
            XmlNode ack = makeElement(recipe->ackResult);
            if (replyBody != nullptr) {
                ack.children = replyBody->children;
                ack.emptyTag = ack.children.empty();
            }
            wrapper.children.push_back(std::move(ack));
        }
    } else {
        return makeError(
            ErrorCode::ProtocolViolation,
            "reply <" + std::string{reply.info().wireName} + "> does not answer <" + op + ">");
    }
    wrapper.emptyTag = false;
    return wrapEnvelope(wrapper);
}

Result<std::vector<Message>> parseSoapResponse(std::string_view envelope,
                                               std::string_view operation, std::string_view mosID,
                                               std::string_view ncsID, std::string_view messageId) {
    const OpRecipe* recipe = recipeForOp(operation);
    if (recipe == nullptr) {
        return makeError(ErrorCode::UnknownMessage,
                         "<" + std::string{operation} + "> is not a WSDL operation");
    }
    Result<XmlNode> unwrapped = unwrapEnvelope(envelope);
    if (!unwrapped.ok()) {
        return std::move(unwrapped).error();
    }
    XmlNode wrapper = std::move(unwrapped).value();
    if (localName(wrapper.name) == "Fault") {
        return makeError(ErrorCode::ProtocolViolation,
                         "SOAP Fault: " + wrapper.childText("faultstring"));
    }
    const std::string op = opName(*recipe);
    if (localName(wrapper.name) != op + "Response") {
        return makeError(ErrorCode::ProtocolViolation,
                         "expected <" + op + "Response>, got <" + wrapper.name + ">");
    }

    const auto synthesize = [&](MessageType type) {
        Message message = codec::makeMessage(type);
        message.setMosID(mosID);
        message.setNcsID(ncsID);
        message.setMessageID(messageId);
        return message;
    };

    XmlNode* result = findChildLocal(wrapper, op + "Result");
    const bool dataOp = recipe->reply != MessageType::MosAck && recipe->reply != MessageType::RoAck;
    const bool resultHasContent =
        result != nullptr &&
        std::any_of(result->children.begin(), result->children.end(),
                    [](const XmlNode& child) { return child.kind == XmlNodeKind::Element; });

    std::vector<Message> replies;
    if (dataOp && !resultHasContent && recipe->ackResult != nullptr) {
        // Empty data slot: the real answer is the ack result (NACK path).
        Message message = synthesize(recipe->reply == MessageType::MosObj ||
                                             recipe->reply == MessageType::MosObjList
                                         ? MessageType::MosAck
                                         : MessageType::RoAck);
        if (XmlNode* ack = findChildLocal(wrapper, recipe->ackResult)) {
            soapToClassicTree(*ack);
            message.body()->children = std::move(ack->children);
            message.body()->emptyTag = message.body()->children.empty();
        }
        message.refreshType();
        replies.push_back(std::move(message));
        return replies;
    }
    if (result == nullptr) {
        return makeError(ErrorCode::ProtocolViolation,
                         "<" + op + "Response> carries no <" + op + "Result>");
    }

    Message message = synthesize(recipe->reply);
    XmlNode* body = message.body();
    if (recipe->reply == MessageType::ListMachInfo) {
        machInfoToClassic(*result, *body);
    } else {
        soapToClassicTree(*result);
        for (const codec::XmlAttribute& attribute : result->attributes) {
            if (attribute.name != "xmlns" && localName(attribute.name) != "xmlns") {
                body->setAttribute(attribute.name, attribute.value);
            }
        }
        body->children = std::move(result->children);
        body->emptyTag = body->children.empty();
    }
    message.refreshType();
    replies.push_back(std::move(message));
    return replies;
}

std::string buildSoapFault(std::string_view faultCode, std::string_view faultString) {
    XmlNode fault = makeElement("soap:Fault");
    appendTextChild(fault, "faultcode", "soap:" + std::string{faultCode});
    appendTextChild(fault, "faultstring", faultString);
    return wrapEnvelope(fault);
}

}  // namespace imos::transports::ws3
