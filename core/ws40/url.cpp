// SPDX-FileCopyrightText: 2026 Javier O. Cordero Pérez
// SPDX-License-Identifier: Apache-2.0

#include <imos/ws40/url.h>

#include <algorithm>
#include <cctype>
#include <iterator>

namespace imos::ws40 {

namespace {

constexpr std::string_view kMosParams[] = {"mosID", "ncsID", "channel", "passive"};

bool isMosParam(std::string_view key) {
    return std::find(std::begin(kMosParams), std::end(kMosParams), key) != std::end(kMosParams);
}

bool isUnreserved(unsigned char c) {
    return (std::isalnum(c) != 0) || c == '-' || c == '.' || c == '_' || c == '~';
}

int hexValue(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

bool equalsIgnoreCase(std::string_view a, std::string_view b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
               return std::tolower(static_cast<unsigned char>(x)) ==
                      std::tolower(static_cast<unsigned char>(y));
           });
}

/// The query portion of `uri`: everything after the first '?' (fragment
/// stripped), or the whole input when it already looks like a bare query.
std::string_view queryOf(std::string_view uri) {
    std::string_view text = uri;
    if (const std::size_t hash = text.find('#'); hash != std::string_view::npos) {
        text = text.substr(0, hash);
    }
    if (const std::size_t question = text.find('?'); question != std::string_view::npos) {
        return text.substr(question + 1);
    }
    // No '?': a bare query has '=' and no '/'; anything else has no query.
    if (text.find('/') == std::string_view::npos && text.find('=') != std::string_view::npos) {
        return text;
    }
    return {};
}

}  // namespace

std::string percentEncode(std::string_view value) {
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(value.size());
    for (const char c : value) {
        const auto byte = static_cast<unsigned char>(c);
        if (isUnreserved(byte)) {
            out.push_back(c);
        } else {
            out.push_back('%');
            out.push_back(kHex[byte >> 4]);
            out.push_back(kHex[byte & 0x0F]);
        }
    }
    return out;
}

Result<std::string> percentDecode(std::string_view value) {
    std::string out;
    out.reserve(value.size());
    for (std::size_t i = 0; i < value.size(); ++i) {
        if (value[i] != '%') {
            out.push_back(value[i]);
            continue;
        }
        if (i + 2 >= value.size()) {
            return makeError(ErrorCode::ParseError, "truncated %-sequence in URL parameter");
        }
        const int hi = hexValue(value[i + 1]);
        const int lo = hexValue(value[i + 2]);
        if (hi < 0 || lo < 0) {
            return makeError(ErrorCode::ParseError, "malformed %-sequence in URL parameter");
        }
        const int decoded = (hi << 4) | lo;
        if (decoded == 0) {
            return makeError(ErrorCode::ParseError, "encoded NUL in URL parameter");
        }
        out.push_back(static_cast<char>(decoded));
        i += 2;
    }
    return out;
}

Result<ConnectionParams> parseConnectionParams(std::string_view uri) {
    if (uri.size() > kMaxUriBytes) {
        return makeError(ErrorCode::LimitExceeded, "connection URI exceeds the size cap");
    }
    const std::string_view query = queryOf(uri);

    ConnectionParams params;
    bool sawMosId = false;
    bool sawNcsId = false;
    bool sawChannel = false;
    bool sawPassive = false;

    std::string_view rest = query;
    while (!rest.empty()) {
        const std::size_t amp = rest.find('&');
        const std::string_view pair = amp == std::string_view::npos ? rest : rest.substr(0, amp);
        rest = amp == std::string_view::npos ? std::string_view{} : rest.substr(amp + 1);
        if (pair.empty()) {
            continue;  // tolerate "a=1&&b=2"
        }

        const std::size_t equals = pair.find('=');
        const std::string_view rawKey =
            equals == std::string_view::npos ? pair : pair.substr(0, equals);
        const std::string_view rawValue =
            equals == std::string_view::npos ? std::string_view{} : pair.substr(equals + 1);

        if (!isMosParam(rawKey)) {
            params.extras.emplace_back(std::string{rawKey}, std::string{rawValue});
            continue;
        }

        Result<std::string> value = percentDecode(rawValue);
        if (!value.ok()) {
            return makeError(value.error().code,
                             std::string{rawKey} + ": " + value.error().message);
        }
        if (value.value().empty()) {
            return makeError(ErrorCode::InvalidArgument,
                             std::string{rawKey} + " parameter is empty");
        }

        if (rawKey == "mosID") {
            if (sawMosId) {
                return makeError(ErrorCode::InvalidArgument, "duplicate mosID parameter");
            }
            sawMosId = true;
            params.mosID = std::move(value).value();
        } else if (rawKey == "ncsID") {
            if (sawNcsId) {
                return makeError(ErrorCode::InvalidArgument, "duplicate ncsID parameter");
            }
            sawNcsId = true;
            params.ncsID = std::move(value).value();
        } else if (rawKey == "channel") {
            if (sawChannel) {
                return makeError(ErrorCode::InvalidArgument, "duplicate channel parameter");
            }
            sawChannel = true;
            const std::optional<Channel> channel = channelFromName(value.value());
            if (!channel.has_value()) {
                return makeError(ErrorCode::InvalidArgument,
                                 "channel must be mom, ro, or aux (got '" + value.value() + "')");
            }
            params.channel = *channel;
        } else {  // passive
            if (sawPassive) {
                return makeError(ErrorCode::InvalidArgument, "duplicate passive parameter");
            }
            sawPassive = true;
            if (!equalsIgnoreCase(value.value(), "true")) {
                return makeError(ErrorCode::InvalidArgument,
                                 "passive accepts only 'true' (got '" + value.value() + "')");
            }
            params.passive = true;
        }
    }

    std::string missing;
    const auto note = [&missing](std::string_view name) {
        if (!missing.empty()) {
            missing += ", ";
        }
        missing += name;
    };
    if (!sawMosId) {
        note("mosID");
    }
    if (!sawNcsId) {
        note("ncsID");
    }
    if (!sawChannel) {
        note("channel");
    }
    if (!missing.empty()) {
        return makeError(ErrorCode::InvalidArgument,
                         "connection URL is missing required parameter(s): " + missing);
    }
    return params;
}

std::string composeQuery(const ConnectionParams& params) {
    std::string query;
    for (const auto& [key, value] : params.extras) {
        if (!query.empty()) {
            query += '&';
        }
        query += key;
        if (!value.empty()) {
            query += '=';
            query += value;
        }
    }
    const auto append = [&query](std::string_view key, std::string_view value) {
        if (!query.empty()) {
            query += '&';
        }
        query += key;
        query += '=';
        query += percentEncode(value);
    };
    append("mosID", params.mosID);
    append("ncsID", params.ncsID);
    append("channel", channelName(params.channel));
    if (params.passive) {
        append("passive", "true");
    }
    return query;
}

std::string composeUrl(std::string_view baseUrl, const ConnectionParams& params) {
    std::string url{baseUrl};
    url += baseUrl.find('?') == std::string_view::npos ? '?' : '&';
    url += composeQuery(params);
    return url;
}

}  // namespace imos::ws40
