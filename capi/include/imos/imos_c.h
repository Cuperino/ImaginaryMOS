/*
 * SPDX-FileCopyrightText: 2026 Javier O. Cordero Pérez
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef IMOS_C_H
#define IMOS_C_H

/*
 * C ABI for ImaginaryMOS (plan §8.2). Opaque handles, integer status codes,
 * UTF-8 strings with explicit ownership, no C++ type in any signature. This
 * header is the binary-stability surface; the C++ API is only source-stable.
 *
 * At M0 the surface is intentionally tiny — enough to pin the ABI-version and
 * symbol-visibility conventions the rest of the ABI will follow.
 */

#include <imos/imos_capi_export.h>

#ifdef __cplusplus
extern "C" {
#endif

/** ABI revision. Bumped on any incompatible change to this header. */
#define IMOS_ABI_VERSION 1

/** Status codes returned across the C ABI. */
typedef enum imos_status {
    IMOS_OK = 0,
    IMOS_ERR_INVALID_ARGUMENT = 1,
    IMOS_ERR_UNSUPPORTED = 2,
    IMOS_ERR_INTERNAL = 3
} imos_status;

/** Semantic version string of the library, e.g. "1.0.0" (library-owned). */
IMOS_CAPI_API const char* imos_version(void);

/** The ABI revision this build was compiled against (IMOS_ABI_VERSION). */
IMOS_CAPI_API int imos_abi_version(void);

/** Human-readable, library-owned message for a status code. Never NULL. */
IMOS_CAPI_API const char* imos_status_message(imos_status status);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* IMOS_C_H */
