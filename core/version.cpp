// SPDX-FileCopyrightText: 2026 Javier O. Cordero Pérez
// SPDX-License-Identifier: Apache-2.0

#include <imos/version.h>

#include <libxml/xmlversion.h>
#include <zlib.h>

namespace imos {

std::string_view coreVersion() noexcept {
    return IMOS_VERSION_STRING;
}

DependencyVersions dependencyVersions() noexcept {
    // Referencing these string constants also proves at link time that the
    // pinned libxml2 and zlib headers/libraries are wired into the build.
    return DependencyVersions{
        .libxml2 = LIBXML_DOTTED_VERSION,
        .zlib = ZLIB_VERSION,
    };
}

}  // namespace imos
