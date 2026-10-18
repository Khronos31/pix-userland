// SPDX-License-Identifier: MIT
#ifndef DTV_USERLAND_ERROR_H
#define DTV_USERLAND_ERROR_H

#include <cstdint>
#include <utility>

namespace dtv::userland {

enum class Error : std::uint8_t {
    OK = 0,
    INVALID_ARGUMENT = 1,
    VERSION_MISMATCH = 2,
    NOT_FOUND = 3,
    BUSY = 4,
    NOT_READY = 5,
    TIMEOUT = 6,
    USB_IO = 7,
    DISCONNECTED = 8,
    PROTOCOL_ERROR = 9,
    FIRMWARE_REJECTED = 10,
    UNSUPPORTED = 11,
    NO_CARD = 12,
    CARD_REMOVED = 13,
    BUFFER_TOO_SMALL = 14,
    SLOW_CONSUMER = 15,
    // Additive value for the bounded wire codecs: a well-formed record whose
    // encoded size exceeds the control payload cap. Not used by the foundation.
    RESOURCE_LIMIT = 17,
    INTERNAL = 255,
};

static_assert(sizeof(Error) == sizeof(std::uint8_t));

const char* error_string(Error error) noexcept;
// Returns the symbol for a recognized u32 code, "OK" for code 0, or nullptr
// for an unknown code. Present error records still require a nonzero code and
// reject 0 during record validation.
const char* error_symbol(std::uint32_t code) noexcept;

template <typename T>
class Result final {
public:
    static Result success(T value) noexcept
    {
        return Result(true, Error::OK, std::move(value));
    }

    static Result failure(Error error) noexcept
    {
        return Result(false, error, T{});
    }

    bool has_value() const noexcept { return has_value_; }
    explicit operator bool() const noexcept { return has_value_; }
    Error error() const noexcept { return error_; }
    T& value() noexcept { return value_; }
    const T& value() const noexcept { return value_; }

private:
    Result(bool has_value, Error error, T&& value) noexcept
        : has_value_(has_value), error_(error), value_(std::move(value))
    {
    }

    bool has_value_;
    Error error_;
    T value_;
};

template <>
class Result<void> final {
public:
    static Result success() noexcept { return Result(true, Error::OK); }

    static Result failure(Error error) noexcept { return Result(false, error); }

    bool has_value() const noexcept { return has_value_; }
    explicit operator bool() const noexcept { return has_value_; }
    Error error() const noexcept { return error_; }

private:
    Result(bool has_value, Error error) noexcept : has_value_(has_value), error_(error) {}

    bool has_value_;
    Error error_;
};

}  // namespace dtv::userland

#endif  // DTV_USERLAND_ERROR_H
