// SPDX-FileCopyrightText: 2026 Javier O. Cordero Pérez
// SPDX-License-Identifier: Apache-2.0

#include <imos/ws40/basic_auth.h>

#include <array>
#include <cctype>
#include <cstdint>

namespace imos::ws40 {

namespace {

constexpr std::string_view kAlphabet =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string base64Encode(std::string_view input) {
    std::string out;
    out.reserve(((input.size() + 2) / 3) * 4);
    std::size_t i = 0;
    for (; i + 3 <= input.size(); i += 3) {
        const std::uint32_t triple = (static_cast<std::uint8_t>(input[i]) << 16) |
                                     (static_cast<std::uint8_t>(input[i + 1]) << 8) |
                                     static_cast<std::uint8_t>(input[i + 2]);
        out.push_back(kAlphabet[(triple >> 18) & 0x3F]);
        out.push_back(kAlphabet[(triple >> 12) & 0x3F]);
        out.push_back(kAlphabet[(triple >> 6) & 0x3F]);
        out.push_back(kAlphabet[triple & 0x3F]);
    }
    const std::size_t remain = input.size() - i;
    if (remain == 1) {
        const std::uint32_t v = static_cast<std::uint8_t>(input[i]) << 16;
        out.push_back(kAlphabet[(v >> 18) & 0x3F]);
        out.push_back(kAlphabet[(v >> 12) & 0x3F]);
        out.push_back('=');
        out.push_back('=');
    } else if (remain == 2) {
        const std::uint32_t v = (static_cast<std::uint8_t>(input[i]) << 16) |
                                (static_cast<std::uint8_t>(input[i + 1]) << 8);
        out.push_back(kAlphabet[(v >> 18) & 0x3F]);
        out.push_back(kAlphabet[(v >> 12) & 0x3F]);
        out.push_back(kAlphabet[(v >> 6) & 0x3F]);
        out.push_back('=');
    }
    return out;
}

constexpr std::array<std::int8_t, 256> makeDecodeTable() {
    std::array<std::int8_t, 256> table{};
    table.fill(-1);
    for (std::size_t i = 0; i < kAlphabet.size(); ++i) {
        table[static_cast<std::uint8_t>(kAlphabet[i])] = static_cast<std::int8_t>(i);
    }
    return table;
}

Result<std::string> base64Decode(std::string_view input) {
    static constexpr std::array<std::int8_t, 256> table = makeDecodeTable();
    if (input.size() % 4 != 0 || input.empty()) {
        return makeError(ErrorCode::ParseError, "base64 length is not a multiple of four");
    }
    std::size_t padding = 0;
    if (input.back() == '=') {
        ++padding;
        if (input.size() >= 2 && input[input.size() - 2] == '=') {
            ++padding;
        }
    }
    std::string out;
    out.reserve(input.size() / 4 * 3);
    std::uint32_t accum = 0;
    int bits = 0;
    for (std::size_t i = 0; i < input.size() - padding; ++i) {
        const std::int8_t value = table[static_cast<std::uint8_t>(input[i])];
        if (value < 0) {
            return makeError(ErrorCode::ParseError, "invalid base64 character");
        }
        accum = (accum << 6) | static_cast<std::uint32_t>(value);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<char>((accum >> bits) & 0xFF));
        }
    }
    return out;
}

}  // namespace

Result<std::string> composeBasicAuthorization(const BasicCredentials& credentials) {
    if (credentials.username.find(':') != std::string::npos) {
        return makeError(ErrorCode::InvalidArgument,
                         "Basic-auth username must not contain ':' (RFC 7617)");
    }
    return "Basic " + base64Encode(credentials.username + ":" + credentials.password);
}

Result<BasicCredentials> parseBasicAuthorization(std::string_view headerValue) {
    // Scheme token, case-insensitive, followed by one or more spaces.
    constexpr std::string_view kScheme = "basic";
    if (headerValue.size() < kScheme.size() + 1) {
        return makeError(ErrorCode::ParseError, "Authorization header too short");
    }
    for (std::size_t i = 0; i < kScheme.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(headerValue[i])) != kScheme[i]) {
            return makeError(ErrorCode::ParseError, "Authorization scheme is not Basic");
        }
    }
    std::string_view rest = headerValue.substr(kScheme.size());
    if (rest.empty() || rest.front() != ' ') {
        return makeError(ErrorCode::ParseError, "Authorization scheme is not Basic");
    }
    while (!rest.empty() && rest.front() == ' ') {
        rest.remove_prefix(1);
    }
    Result<std::string> decoded = base64Decode(rest);
    if (!decoded.ok()) {
        return std::move(decoded).error();
    }
    const std::size_t colon = decoded.value().find(':');
    if (colon == std::string::npos) {
        return makeError(ErrorCode::ParseError, "Basic credentials lack the ':' separator");
    }
    BasicCredentials credentials;
    credentials.username = decoded.value().substr(0, colon);
    credentials.password = decoded.value().substr(colon + 1);
    return credentials;
}

bool constantTimeEquals(std::string_view a, std::string_view b) noexcept {
    // Fold the length difference into the accumulator instead of returning
    // early; index b cyclically so every byte of a is examined either way.
    unsigned diff = a.size() ^ b.size();
    const std::size_t bLen = b.empty() ? 1 : b.size();
    for (std::size_t i = 0; i < a.size(); ++i) {
        const char other = b.empty() ? '\0' : b[i % bLen];
        diff |= static_cast<unsigned>(static_cast<unsigned char>(a[i]) ^
                                      static_cast<unsigned char>(other));
    }
    return diff == 0;
}

bool verifyCredentials(const BasicCredentials& presented,
                       const BasicCredentials& expected) noexcept {
    // Single '&' (not '&&') so both comparisons always run.
    return static_cast<bool>(
        static_cast<unsigned>(constantTimeEquals(presented.username, expected.username)) &
        static_cast<unsigned>(constantTimeEquals(presented.password, expected.password)));
}

}  // namespace imos::ws40
