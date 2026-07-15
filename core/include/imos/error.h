// SPDX-FileCopyrightText: 2026 Javier O. Cordero Pérez
// SPDX-License-Identifier: Apache-2.0
//
// Error/Result vocabulary of the public C++ API. No exceptions cross the API
// boundary (plan §4.1 "no surprises at boundaries"); fallible operations
// return Result<T>.

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <variant>

namespace imos {

enum class ErrorCode : std::uint16_t {
    Ok = 0,
    InvalidArgument,
    ParseError,         ///< malformed XML or message structure
    EncodingError,      ///< invalid or misconfigured character encoding
    LimitExceeded,      ///< size/depth/inflation cap hit (plan §12 hardening)
    UnknownMessage,     ///< well-formed but unrecognized message element
    ProtocolViolation,  ///< message illegal in the current state or version
    WrongState,
    NotFound,
    DataCorrupt,  ///< store record failed CRC or structural checks
    IoError,      ///< propagated from an injected side-effect interface
};

struct Error {
    ErrorCode code = ErrorCode::Ok;
    std::string message;
};

inline Error makeError(ErrorCode code, std::string message) {
    return Error{code, std::move(message)};
}

/// Minimal expected-like result carrier: either a T or an Error.
template <typename T>
class [[nodiscard]] Result {
public:
    Result(T value) : storage_(std::move(value)) {}      // NOLINT(google-explicit-constructor)
    Result(Error error) : storage_(std::move(error)) {}  // NOLINT(google-explicit-constructor)

    [[nodiscard]] bool ok() const noexcept { return storage_.index() == 0; }
    explicit operator bool() const noexcept { return ok(); }

    [[nodiscard]] T& value() & { return std::get<0>(storage_); }
    [[nodiscard]] const T& value() const& { return std::get<0>(storage_); }
    [[nodiscard]] T&& value() && { return std::get<0>(std::move(storage_)); }

    [[nodiscard]] const Error& error() const& { return std::get<1>(storage_); }
    [[nodiscard]] Error&& error() && { return std::get<1>(std::move(storage_)); }

private:
    std::variant<T, Error> storage_;
};

template <>
class [[nodiscard]] Result<void> {
public:
    Result() = default;
    Result(Error error) : error_(std::move(error)) {}  // NOLINT(google-explicit-constructor)

    [[nodiscard]] bool ok() const noexcept { return !error_.has_value(); }
    explicit operator bool() const noexcept { return ok(); }

    [[nodiscard]] const Error& error() const& { return *error_; }
    [[nodiscard]] Error&& error() && { return *std::move(error_); }

private:
    std::optional<Error> error_;
};

}  // namespace imos
