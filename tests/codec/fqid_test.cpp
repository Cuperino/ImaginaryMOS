// SPDX-FileCopyrightText: 2026 Javier O. Cordero Pérez
// SPDX-License-Identifier: Apache-2.0
//
// FQID parse / compose / classify tables (Profile 6, plan §3.2 row 6). The
// spec gives only the dotted form and four example IDs; every tie-break the
// implementation had to decide — positional 3-label reading, exact
// case-sensitive comparison, absent-scope matching — is pinned here.

#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <imos/fqid.h>

using imos::classify;
using imos::Fqid;
using imos::Locality;
using imos::localityName;
using imos::parseFqid;

namespace {

TEST(Fqid, ParsesTheSpecExampleForms) {
    // The spec's own four examples, plus the 3-label form it implies.
    struct Row {
        std::string id;
        std::string family, machine, location, enterprise;
    };
    const std::vector<Row> rows = {
        {"aveed.server2.camden.cbs.mos", "aveed", "server2", "camden", "cbs"},
        {"tornado.mach2.wjla.allbritton.mos", "tornado", "mach2", "wjla", "allbritton"},
        {"Quantuml.VidServ2.mos", "Quantuml", "VidServ2", "", ""},
        {"Sonny.point77.city.company.mos", "Sonny", "point77", "city", "company"},
        // 3-label: the third label is positionally the location.
        {"aveed.server2.camden.mos", "aveed", "server2", "camden", ""},
    };
    for (const Row& row : rows) {
        const auto parsed = parseFqid(row.id);
        ASSERT_TRUE(parsed.ok()) << row.id << ": " << parsed.error().message;
        EXPECT_EQ(parsed.value().family, row.family) << row.id;
        EXPECT_EQ(parsed.value().machine, row.machine) << row.id;
        EXPECT_EQ(parsed.value().location, row.location) << row.id;
        EXPECT_EQ(parsed.value().enterprise, row.enterprise) << row.id;
        // compose ∘ parse is the identity on valid ids.
        EXPECT_EQ(parsed.value().compose(), row.id);
    }
}

TEST(Fqid, RejectsNonFqidShapes) {
    const std::vector<std::string> rejected = {
        "",                    // nothing
        "mos",                 // no labels at all
        ".mos",                // one empty label
        "server2.mos",         // machine without family
        "a.b.c.d.e.mos",       // five labels: more than the convention allows
        "a..mos",              // empty label
        "a..b.mos",            // empty label between others
        "aveed.server2",       // no .mos
        "aveed.server2.MOS",   // comparison is case-sensitive, "MOS" != "mos"
        "aveed.server2.mos ",  // trailing space breaks the final label
        "VTR-1",               // a plain legacy mosID
        "aveed.server2.moss",  // wrong final label
    };
    for (const std::string& id : rejected) {
        EXPECT_FALSE(parseFqid(id).ok()) << "'" << id << "' should not parse";
    }
}

TEST(Fqid, ClassifiesAgainstFullyQualifiedSelf) {
    const auto self = parseFqid("aveed.server1.camden.cbs.mos");
    ASSERT_TRUE(self.ok());
    const struct {
        std::string id;
        Locality locality;
    } rows[] = {
        {"aveed.server1.camden.cbs.mos", Locality::Self},
        // Family never affects locality: a different-family server on the
        // same machine still means nothing to transfer.
        {"tornado.server1.camden.cbs.mos", Locality::Self},
        {"aveed.server2.camden.cbs.mos", Locality::SameEnterprise},
        // The same machine name in another city is a different machine.
        {"aveed.server1.newyork.cbs.mos", Locality::SameEnterprise},
        {"aveed.server9.newyork.cbs.mos", Locality::SameEnterprise},
        {"tornado.mach2.wjla.allbritton.mos", Locality::Foreign},
        // Absent scope only matches absent scope: an enterprise-less FQID
        // shares no transfer scope with an enterprise-scoped one.
        {"aveed.server1.camden.mos", Locality::Foreign},
        {"Quantuml.VidServ2.mos", Locality::Foreign},
        {"VTR-1", Locality::NotFqid},
        {"aveed.server1.camden.cbs.MOS", Locality::NotFqid},
    };
    for (const auto& row : rows) {
        EXPECT_EQ(classify(self.value(), row.id), row.locality)
            << row.id << " should be " << localityName(row.locality);
    }
}

TEST(Fqid, ClassifiesAgainstUnscopedSelf) {
    const auto self = parseFqid("Quantuml.VidServ2.mos");
    ASSERT_TRUE(self.ok());
    const struct {
        std::string id;
        Locality locality;
    } rows[] = {
        {"Quantuml.VidServ2.mos", Locality::Self},
        {"Other.VidServ2.mos", Locality::Self},
        {"Quantuml.VidServ3.mos", Locality::SameEnterprise},
        // Both enterprise-less: same (unspecified) scope, another machine.
        {"aveed.server2.camden.mos", Locality::SameEnterprise},
        {"aveed.server2.camden.cbs.mos", Locality::Foreign},
        // Comparison is exact: a case-variant machine is a different
        // machine (still in the same empty enterprise scope).
        {"quantuml.vidserv2.mos", Locality::SameEnterprise},
    };
    for (const auto& row : rows) {
        EXPECT_EQ(classify(self.value(), row.id), row.locality)
            << row.id << " should be " << localityName(row.locality);
    }
}

TEST(Fqid, LocalityNamesAreStable) {
    EXPECT_EQ(localityName(Locality::Self), "Self");
    EXPECT_EQ(localityName(Locality::SameEnterprise), "SameEnterprise");
    EXPECT_EQ(localityName(Locality::Foreign), "Foreign");
    EXPECT_EQ(localityName(Locality::NotFqid), "NotFqid");
}

}  // namespace
