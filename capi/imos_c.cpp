// SPDX-FileCopyrightText: 2026 Javier O. Cordero Pérez
// SPDX-License-Identifier: Apache-2.0

#include <imos/imos_c.h>

#include <imos/version.h>

// No exception may cross the C ABI (plan §8.2); every entry point here is
// noexcept-by-construction. Real handlers gain a catch-all-to-status wrapper.

extern "C" {

const char* imos_version(void) {
    return IMOS_VERSION_STRING;
}

int imos_abi_version(void) {
    return IMOS_ABI_VERSION;
}

const char* imos_status_message(imos_status status) {
    switch (status) {
        case IMOS_OK:
            return "ok";
        case IMOS_ERR_INVALID_ARGUMENT:
            return "invalid argument";
        case IMOS_ERR_UNSUPPORTED:
            return "unsupported";
        case IMOS_ERR_INTERNAL:
            return "internal error";
    }
    return "unknown status";
}

}  // extern "C"
