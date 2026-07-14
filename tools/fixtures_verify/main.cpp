// SPDX-FileCopyrightText: 2026 Javier O. Cordero Pérez
// SPDX-License-Identifier: Apache-2.0
//
// fixtures-verify — the M0 "fixtures verified against spec" gate.
//
// Using the pinned libxml2, it asserts that (1) every non-illustrative spec
// example parses as well-formed XML, (2) the consolidated DTD loads, and
// (3) the XSD schema loads. Illustrative examples (*.illustrative.xml) are the
// spec's own non-well-formed fragments and are reported but not gated (see
// fixtures/README.md). This also exercises the libxml2 build wiring.

#include <libxml/parser.h>
#include <libxml/xmlerror.h>
#include <libxml/xmlschemas.h>
#include <libxml/xmlversion.h>

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>

namespace fs = std::filesystem;

namespace {

// Hardening (plan §12): no network access, and do not substitute entities.
constexpr int kParseOpts = XML_PARSE_NONET | XML_PARSE_NOWARNING | XML_PARSE_NOERROR;

// Floor guarding against gross under-extraction without pinning an exact count.
constexpr int kMinWellFormed = 50;

void silentErrors(void*, const xmlError*) {}

void printErrors(void*, const xmlError* e) {
    if (e != nullptr && e->message != nullptr) {
        std::cerr << "    libxml2 line " << e->line << ": " << e->message;
    }
}

bool parsesWellFormed(const fs::path& file) {
    const std::string path = file.string();
    xmlDoc* doc = xmlReadFile(path.c_str(), nullptr, kParseOpts);
    const bool ok = doc != nullptr;
    if (doc != nullptr) {
        xmlFreeDoc(doc);
    }
    return ok;
}

bool dtdLoads(const fs::path& file) {
    const std::string path = file.string();
    xmlDtd* dtd = xmlParseDTD(nullptr, reinterpret_cast<const xmlChar*>(path.c_str()));
    const bool ok = dtd != nullptr;
    if (dtd != nullptr) {
        xmlFreeDtd(dtd);
    }
    return ok;
}

bool xsdLoads(const fs::path& file) {
    const std::string path = file.string();
    xmlSchemaParserCtxt* ctxt = xmlSchemaNewParserCtxt(path.c_str());
    if (ctxt == nullptr) {
        return false;
    }
    xmlSchema* schema = xmlSchemaParse(ctxt);
    const bool ok = schema != nullptr;
    if (schema != nullptr) {
        xmlSchemaFree(schema);
    }
    xmlSchemaFreeParserCtxt(ctxt);
    return ok;
}

}  // namespace

int main(int argc, char** argv) {
    const fs::path root = (argc > 1) ? fs::path(argv[1]) : fs::path("fixtures");
    const fs::path examples = root / "spec-examples";
    const fs::path dtd = root / "schema" / "mos4.dtd";
    const fs::path xsd = root / "schema" / "mos4.xsd";

    if (!fs::is_directory(examples)) {
        std::cerr << "fixtures-verify: no such directory: " << examples << '\n';
        return 2;
    }

    LIBXML_TEST_VERSION
    const bool debug = std::getenv("IMOS_VERIFY_DEBUG") != nullptr;
    xmlSetStructuredErrorFunc(nullptr, debug ? printErrors : silentErrors);

    int wellFormed = 0;
    int failed = 0;
    int illustrative = 0;

    for (const auto& entry : fs::directory_iterator(examples)) {
        const fs::path& p = entry.path();
        if (p.extension() != ".xml") {
            continue;
        }
        const std::string name = p.filename().string();
        if (name.find(".illustrative.") != std::string::npos) {
            ++illustrative;
            continue;
        }
        if (parsesWellFormed(p)) {
            ++wellFormed;
        } else {
            ++failed;
            std::cerr << "  NOT well-formed: " << name << '\n';
        }
    }

    const bool dtdOk = dtdLoads(dtd);
    const bool xsdOk = xsdLoads(xsd);

    xmlCleanupParser();

    std::cout << "fixtures-verify (libxml2 " << LIBXML_DOTTED_VERSION << ")\n"
              << "  well-formed examples : " << wellFormed << '\n'
              << "  failed examples      : " << failed << '\n'
              << "  illustrative (skipped): " << illustrative << '\n'
              << "  DTD loads            : " << (dtdOk ? "yes" : "no") << '\n'
              << "  XSD loads            : " << (xsdOk ? "yes" : "no") << '\n';

    const bool pass = failed == 0 && dtdOk && xsdOk && wellFormed >= kMinWellFormed;
    std::cout << (pass ? "PASS" : "FAIL") << '\n';
    return pass ? 0 : 1;
}
