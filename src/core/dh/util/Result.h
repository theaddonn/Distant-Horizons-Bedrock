#pragma once

// Minimal expected-like result type for the exception-free core.
//
//   Result<T>  holds either a T or an Error.
//   Status     = Result<void>: success or an Error.
//
// Intentionally tiny (no monadic map/and_then); the API is a strict subset of
// std::expected so that a later swap to <expected>/expected-lite is mechanical:
// has_value(), operator bool, value(), operator*, operator->, error(), value_or().
// value()/operator* on an error is a DH_ASSERT failure (never UB-silent).

#include "dh/util/Assert.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace dh {

/// Coarse failure classes. Keep these broad; details go in Error::message.
enum class ErrorCode : std::uint16_t {
    Unknown = 0,
    InvalidArgument, ///< caller passed something the callee cannot act on
    OutOfRange,      ///< index/position outside the valid domain
    NotFound,        ///< key/path/record does not exist
    Io,              ///< file system / transport failure
    Corrupt,         ///< decode failed: malformed or truncated input
    Unsupported,     ///< format version / feature not implemented
    Rejected,        ///< target refused the request (shut down, full, closed)
    Cancelled,       ///< operation aborted before completion
    Internal,        ///< invariant violation that was recoverable
};

struct Error {
    ErrorCode   code = ErrorCode::Unknown;
    std::string message;

    Error() = default;
    Error(ErrorCode c, std::string msg) : code(c), message(std::move(msg)) {}
};

inline Error makeError(ErrorCode code, std::string_view message) { return Error{code, std::string(message)}; }

template <class T>
class [[nodiscard]] Result {
public:
    using value_type = T;
    using error_type = Error;

    Result(const T& v) : value_(v) {}         // NOLINT(google-explicit-constructor)
    Result(T&& v) : value_(std::move(v)) {}   // NOLINT(google-explicit-constructor)
    Result(Error e) : error_(std::move(e)) {} // NOLINT(google-explicit-constructor)
    Result(ErrorCode code, std::string_view message) : error_(code, std::string(message)) {}

    bool     has_value() const noexcept { return value_.has_value(); }
    explicit operator bool() const noexcept { return has_value(); }

    T& value() & {
        DH_ASSERT(has_value(), "Result::value() called on an error");
        return *value_;
    }
    const T& value() const& {
        DH_ASSERT(has_value(), "Result::value() called on an error");
        return *value_;
    }
    T&& value() && {
        DH_ASSERT(has_value(), "Result::value() called on an error");
        return std::move(*value_);
    }

    T&       operator*() & { return value(); }
    const T& operator*() const& { return value(); }
    T&&      operator*() && { return std::move(*this).value(); }
    T*       operator->() { return &value(); }
    const T* operator->() const { return &value(); }

    template <class U>
    T value_or(U&& fallback) const& {
        return has_value() ? *value_ : static_cast<T>(std::forward<U>(fallback));
    }

    /// Only meaningful when !has_value(); asserts otherwise.
    const Error& error() const {
        DH_ASSERT(!has_value(), "Result::error() called on a value");
        return error_;
    }
    Error& error() {
        DH_ASSERT(!has_value(), "Result::error() called on a value");
        return error_;
    }

private:
    std::optional<T> value_;
    Error            error_;
};

template <>
class [[nodiscard]] Result<void> {
public:
    using value_type = void;
    using error_type = Error;

    Result() = default;                       ///< success
    Result(Error e) : error_(std::move(e)) {} // NOLINT(google-explicit-constructor)
    Result(ErrorCode code, std::string_view message) : error_(std::in_place, code, std::string(message)) {}

    static Result ok() { return Result{}; }

    bool     has_value() const noexcept { return !error_.has_value(); }
    explicit operator bool() const noexcept { return has_value(); }

    void value() const { DH_ASSERT(has_value(), "Status::value() called on an error"); }

    const Error& error() const {
        DH_ASSERT(!has_value(), "Status::error() called on success");
        return *error_;
    }

private:
    std::optional<Error> error_;
};

using Status = Result<void>;

} // namespace dh
