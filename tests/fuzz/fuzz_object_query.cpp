// SPDX-FileCopyrightText: 2026 Javier O. Cordero Pérez
// SPDX-License-Identifier: Apache-2.0
//
// Fuzz surface (M5, Profile 3): the subset-XPath parser and generalSearch
// evaluator behind mosReqObjList — a brand-new text-input surface — plus the
// full query parse/evaluate path when the input happens to be a well-formed
// mosReqObjList document. Seeded from the spec fixtures and a small set of
// raw XPath expressions.

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <imos/codec/message.h>
#include <imos/profiles/object_query.h>

namespace {

const imos::codec::Message& corpusObject() {
    static const imos::codec::Message object = [] {
        auto parsed = imos::codec::parseMessage(
            "<mos><mosID>m</mosID><ncsID>n</ncsID><messageID>1</messageID>"
            "<mosObj><objID>O1</objID><objSlug>man bites dog</objSlug>"
            "<mosExternalMetadata><mosSchema>http://s/1</mosSchema>"
            "<mosPayload><Presenter>Bob</Presenter><Video><Length>90</Length></Video>"
            "</mosPayload></mosExternalMetadata></mosObj></mos>");
        return std::move(parsed).value();
    }();
    return object;
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    if (size > (1U << 16)) {
        return 0;
    }
    const std::string_view input{reinterpret_cast<const char*>(data), size};

    // 1. The raw expression surfaces: parse must never crash, and a parsed
    //    field must evaluate against a real payload without incident.
    imos::Result<imos::profiles::FieldExpression> field = imos::profiles::parseFieldXPath(input);
    if (field.ok()) {
        const imos::codec::XmlNode* body = corpusObject().body();
        if (const imos::codec::XmlNode* metadata = body->firstChildElement("mosExternalMetadata")) {
            if (const imos::codec::XmlNode* payload = metadata->firstChildElement("mosPayload")) {
                (void)imos::profiles::evaluateField(field.value(), *payload);
            }
        }
    }
    (void)imos::profiles::evaluateGeneralSearch(input, "a man and his dog went to town");

    // 2. The message surface: a well-formed mosReqObjList exercises the full
    //    parse → evaluate → answer-build path.
    imos::codec::XmlParseLimits limits;
    limits.maxDocumentBytes = 1U << 16;
    limits.maxDepth = 32;
    imos::Result<imos::codec::Message> message = imos::codec::parseMessage(input, limits);
    if (message.ok() && message.value().type() == imos::codec::MessageType::MosReqObjList) {
        imos::Result<imos::profiles::ObjectQuery> query =
            imos::profiles::parseObjectQuery(message.value());
        if (query.ok()) {
            const std::vector<const imos::codec::Message*> corpus{&corpusObject()};
            imos::Result<imos::profiles::ObjectQueryPage> page =
                imos::profiles::evaluateQuery(query.value(), corpus);
            if (page.ok()) {
                (void)imos::profiles::buildMosObjList(query.value(), page.value());
            }
            (void)imos::profiles::buildMosReqObjList(query.value());
        }
    }
    return 0;
}
