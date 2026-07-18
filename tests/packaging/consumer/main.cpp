// SPDX-FileCopyrightText: 2026 Javier O. Cordero Pérez
// SPDX-License-Identifier: Apache-2.0
//
// A clean consumer of the installed ImaginaryMOS package. It touches every
// installed library so that linking exercises the whole package — in the
// static (fat-archive) case, resolving symbols from the folded libxml2, zlib,
// and IXWebSocket proves those deps were bundled correctly.

#include <cstdio>

#include <imos/imos.h>    // imos role
#include <imos/imos_c.h>  // C ABI (imos_capi)
#include <imos/store/file_store.h>
#include <imos/transports/ws40.h>
#include <imos/version.h>  // core: coreVersion / dependencyVersions
#include <incs/incs.h>     // incs role

int main() {
    // core + the folded vendored deps: dependencyVersions() reads the libxml2
    // and zlib version strings, forcing those symbols to resolve.
    const auto core = imos::coreVersion();
    const auto deps = imos::dependencyVersions();
    std::printf("imos core   : %.*s\n", static_cast<int>(core.size()), core.data());
    std::printf("  libxml2   : %.*s\n", static_cast<int>(deps.libxml2.size()), deps.libxml2.data());
    std::printf("  zlib      : %.*s\n", static_cast<int>(deps.zlib.size()), deps.zlib.data());

    // Role libraries.
    const auto iv = imos::version();
    const auto nv = incs::version();
    std::printf("imos role   : %.*s / %.*s\n", static_cast<int>(imos::libraryName().size()),
                imos::libraryName().data(), static_cast<int>(iv.size()), iv.data());
    std::printf("incs role   : %.*s / %.*s\n", static_cast<int>(incs::libraryName().size()),
                incs::libraryName().data(), static_cast<int>(nv.size()), nv.data());

    // C ABI.
    std::printf("imos_capi   : %s (abi %d)\n", imos_version(), imos_abi_version());

    // Force the transport and store objects — and thus the folded IXWebSocket —
    // to link, without opening sockets or files. Taking the address of a
    // non-virtual member function is an odr-use that pulls its object in.
    auto client_start = &imos::transports::ws40::Ws40Client::start;
    auto store_stats = &imos::store::FileMessageStore::recoveryStats;
    const bool linked = (client_start != nullptr) && (store_stats != nullptr);
    std::printf("transports  : %s\n", linked ? "linked" : "MISSING");
    std::printf("store_file  : %s\n", linked ? "linked" : "MISSING");

    return linked ? 0 : 1;
}
