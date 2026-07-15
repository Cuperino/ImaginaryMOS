// SPDX-FileCopyrightText: 2026 Javier O. Cordero Pérez
// SPDX-License-Identifier: Apache-2.0
//
// M0 smoke test: confirms every first-party library links and reports sane
// version/linkage information. Real protocol conformance suites arrive in M1+.

#include <gtest/gtest.h>

#include <imos/imos.h>
#include <imos/imos_c.h>
#include <imos/version.h>
#include <incs/incs.h>

#if IMOS_HAVE_TRANSPORTS
#include <imos/transports.h>
#endif

TEST(Smoke, CoreVersion) {
    EXPECT_EQ(imos::coreVersion(), "1.0.0");
}

TEST(Smoke, DependencyLinkage) {
    const auto deps = imos::dependencyVersions();
    EXPECT_FALSE(deps.libxml2.empty());  // libxml2 header/lib wired in
    EXPECT_FALSE(deps.zlib.empty());     // zlib header/lib wired in
}

TEST(Smoke, RoleLibraries) {
    EXPECT_EQ(imos::libraryName(), "imos");
    EXPECT_EQ(incs::libraryName(), "incs");
    EXPECT_EQ(imos::version(), incs::version());
}

TEST(Smoke, CAbi) {
    EXPECT_STREQ(imos_version(), "1.0.0");
    EXPECT_EQ(imos_abi_version(), IMOS_ABI_VERSION);
    EXPECT_STREQ(imos_status_message(IMOS_OK), "ok");
    EXPECT_STREQ(imos_status_message(IMOS_ERR_UNSUPPORTED), "unsupported");
}

#if IMOS_HAVE_TRANSPORTS
TEST(Smoke, TransportBackend) {
    EXPECT_TRUE(imos::transports::ixwebsocketAvailable());
    // The probe must report exactly what this build configured
    // (IMOS_ENABLE_TLS → IXWebSocket USE_TLS; the linux-tls preset is ON).
    EXPECT_EQ(imos::transports::tlsEnabled(), static_cast<bool>(IMOS_TLS_EXPECTED));
}
#endif
