// SPDX-License-Identifier: MIT
#ifndef DTV_USERLAND_TUNING_H
#define DTV_USERLAND_TUNING_H

#include <dtv/userland/records.h>

#include <cstdint>
#include <optional>
#include <string_view>

namespace dtv::tuning {

struct Request final {
    std::optional<std::string_view> channel;
    std::optional<std::uint64_t> frequency_hz;
    std::optional<userland::wire::System> system;
    std::optional<std::uint16_t> slot;
    std::optional<std::uint16_t> tsid;
};

struct Resolved final {
    userland::wire::System system = userland::wire::System::isdb_t;
    std::uint64_t frequency_hz = 0U;
    userland::wire::Selection selection = userland::wire::Selection::automatic;
    std::uint16_t selection_value = 0U;
};

// Resolves the adopted channel/Hz/system/selection contract. This preserves
// requested software Hz; backend quantization and capability checks are later.
userland::Result<Resolved> resolve(const Request& request) noexcept;

}  // namespace dtv::tuning

#endif  // DTV_USERLAND_TUNING_H
