// SPDX-License-Identifier: MIT
#include <dtv/userland/tuning.h>

namespace dtv::tuning {
namespace {

using userland::wire::Selection;
using userland::wire::System;

bool is_system(System system) noexcept
{
    return system == System::isdb_t || system == System::isdb_s;
}

bool parse_two_digits(std::string_view text, std::size_t offset,
                      unsigned* value) noexcept
{
    if (offset + 2U > text.size() || text[offset] < '0' || text[offset] > '9' ||
        text[offset + 1U] < '0' || text[offset + 1U] > '9') {
        return false;
    }
    *value = static_cast<unsigned>(text[offset] - '0') * 10U +
             static_cast<unsigned>(text[offset + 1U] - '0');
    return true;
}

struct Channel final {
    System system;
    std::uint64_t frequency_hz;
    bool has_slot;
    std::uint16_t slot;
};

bool parse_channel(std::string_view text, Channel* channel) noexcept
{
    if (text.empty() || text.size() > 7U) {
        return false;
    }

    unsigned number = 0U;
    if (text.size() == 2U && parse_two_digits(text, 0U, &number) &&
        number >= 13U && number <= 62U) {
        channel->system = System::isdb_t;
        channel->frequency_hz = 473142857ULL + (number - 13U) * 6000000ULL;
        channel->has_slot = false;
        channel->slot = 0U;
        return true;
    }
    if (text.size() == 3U && text[0] == 'T' &&
        parse_two_digits(text, 1U, &number) && number >= 13U && number <= 62U) {
        channel->system = System::isdb_t;
        channel->frequency_hz = 473142857ULL + (number - 13U) * 6000000ULL;
        channel->has_slot = false;
        channel->slot = 0U;
        return true;
    }

    if (text.size() >= 4U && text[0] == 'B' && text[1] == 'S' &&
        parse_two_digits(text, 2U, &number) && number >= 1U && number <= 23U &&
        (number & 1U) != 0U) {
        channel->system = System::isdb_s;
        channel->frequency_hz = 1049480000ULL + ((number - 1U) / 2U) * 38360000ULL;
        channel->has_slot = false;
        channel->slot = 0U;
        if (text.size() == 4U) {
            return true;
        }
        if (text.size() < 6U || text[4] != '_') {
            return false;
        }
        const auto suffix = text.substr(5U);
        if (suffix.size() == 1U && suffix[0] >= '0' && suffix[0] <= '9') {
            channel->slot = static_cast<std::uint16_t>(suffix[0] - '0');
        } else if (suffix.size() == 2U && suffix[0] == '1' &&
                   suffix[1] >= '0' && suffix[1] <= '1') {
            channel->slot = static_cast<std::uint16_t>(10U + suffix[1] - '0');
        } else {
            return false;
        }
        channel->has_slot = true;
        return true;
    }

    if (text.size() == 3U && text[0] == 'C' && text[1] == 'S' &&
        text[2] >= '2' && text[2] <= '8' && ((text[2] - '0') % 2) == 0) {
        number = static_cast<unsigned>(text[2] - '0');
        channel->system = System::isdb_s;
        channel->frequency_hz = 1613000000ULL + ((number - 2U) / 2U) * 40000000ULL;
        channel->has_slot = false;
        channel->slot = 0U;
        return true;
    }
    if (text.size() == 4U && text[0] == 'C' && text[1] == 'S' &&
        parse_two_digits(text, 2U, &number) && number >= 10U && number <= 24U &&
        (number & 1U) == 0U) {
        channel->system = System::isdb_s;
        channel->frequency_hz = 1613000000ULL + ((number - 2U) / 2U) * 40000000ULL;
        channel->has_slot = false;
        channel->slot = 0U;
        return true;
    }
    if (text.size() == 4U && text[0] == 'C' && text[1] == 'S' && text[2] == '0' &&
        (text[3] == '2' || text[3] == '4' || text[3] == '6' || text[3] == '8')) {
        number = static_cast<unsigned>(text[3] - '0');
        channel->system = System::isdb_s;
        channel->frequency_hz = 1613000000ULL + ((number - 2U) / 2U) * 40000000ULL;
        channel->has_slot = false;
        channel->slot = 0U;
        return true;
    }
    return false;
}

bool valid_frequency(std::uint64_t frequency_hz) noexcept
{
    return frequency_hz >= 1U && frequency_hz <= 9007199254740991ULL;
}

}  // namespace

userland::Result<Resolved> resolve(const Request& request) noexcept
{
    using userland::Error;
    using userland::Result;

    const bool has_channel = request.channel.has_value();
    const bool has_frequency = request.frequency_hz.has_value();
    if (has_channel == has_frequency) {
        return Result<Resolved>::failure(Error::INVALID_ARGUMENT);
    }

    Channel parsed{};
    System system = System::isdb_t;
    std::uint64_t frequency_hz = 0U;
    if (has_channel) {
        if (!parse_channel(*request.channel, &parsed)) {
            return Result<Resolved>::failure(Error::INVALID_ARGUMENT);
        }
        system = parsed.system;
        frequency_hz = parsed.frequency_hz;
        if (request.system.has_value() &&
            (!is_system(*request.system) || *request.system != system)) {
            return Result<Resolved>::failure(Error::INVALID_ARGUMENT);
        }
    } else {
        if (!request.system.has_value() || !is_system(*request.system) ||
            !valid_frequency(*request.frequency_hz)) {
            return Result<Resolved>::failure(Error::INVALID_ARGUMENT);
        }
        system = *request.system;
        frequency_hz = *request.frequency_hz;
    }

    if (request.slot.has_value() && *request.slot > 11U) {
        return Result<Resolved>::failure(Error::INVALID_ARGUMENT);
    }
    if (request.tsid.has_value() && (*request.tsid < 1U || *request.tsid > 65534U)) {
        return Result<Resolved>::failure(Error::INVALID_ARGUMENT);
    }
    if (request.slot.has_value() && request.tsid.has_value()) {
        return Result<Resolved>::failure(Error::INVALID_ARGUMENT);
    }

    Resolved resolved{system, frequency_hz, Selection::automatic, 0U};
    if (system == System::isdb_t) {
        if (request.slot.has_value() || request.tsid.has_value()) {
            return Result<Resolved>::failure(Error::INVALID_ARGUMENT);
        }
        return Result<Resolved>::success(resolved);
    }

    if (has_channel && parsed.has_slot) {
        if (request.slot.has_value() || request.tsid.has_value()) {
            return Result<Resolved>::failure(Error::INVALID_ARGUMENT);
        }
        resolved.selection = Selection::slot;
        resolved.selection_value = parsed.slot;
    } else if (request.tsid.has_value()) {
        resolved.selection = Selection::tsid;
        resolved.selection_value = *request.tsid;
    } else if (request.slot.has_value()) {
        resolved.selection = Selection::slot;
        resolved.selection_value = *request.slot;
    } else if (has_channel && request.channel->substr(0U, 2U) == "CS") {
        resolved.selection = Selection::slot;
        resolved.selection_value = 0U;
    } else {
        return Result<Resolved>::failure(Error::INVALID_ARGUMENT);
    }
    return Result<Resolved>::success(resolved);
}

}  // namespace dtv::tuning
