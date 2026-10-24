// SPDX-License-Identifier: GPL-2.0-only
#include "dtv/userland/identity.h"

#include "host_ctl.h"

#if defined(_WIN32)
#include "px4_windows_args.h"
#endif

#include <cstring>
#include <string>
#include <vector>

namespace {

namespace wire = dtv::userland::wire;
namespace identity = dtv::userland::identity;

bool matches_pix_serial(const wire::ListResponse& list,
                        const identity::ResolvedIdentity& expected) noexcept
{
    if (!list.enclosure.present || expected.kind != identity::IdentityKind::serial)
        return false;
    const wire::Identity& observed = list.enclosure.value.identity;
    if (observed.uniqueness != wire::Uniqueness::unique_in_scan ||
        observed.serial_state != wire::IdentitySerialState::present ||
        !observed.serial.present || list.functions.count != 1U ||
        !list.functions.items[0U].present)
        return false;
    const wire::UsbFunction& function = list.functions.items[0U];
    if (function.vid != 0x06b8U || function.pid != 0x103bU) return false;
    const auto& serial = observed.serial;
    return serial.value.length == expected.serial.length &&
           std::memcmp(serial.value.bytes.data(), expected.serial.bytes.data(),
                       expected.serial.length) == 0;
}

const dtv::userland::host_ctl::Profile& profile() noexcept
{
    static const dtv::userland::host_ctl::Profile value{
        "pixctl", "pix-userland", PX4CTL_PRODUCT_VERSION,
        identity::Product::pix, false, true, nullptr, &matches_pix_serial, false, true};
    return value;
}

}  // namespace

int main(int argc, char** argv)
{
#if defined(_WIN32)
    const std::vector<std::string> owned_argv =
        px4::userland::cli::windows_argv_utf8(argc, argv);
    std::vector<const char*> argv_views;
    argv_views.reserve(owned_argv.size());
    for (const std::string& value : owned_argv) argv_views.push_back(value.c_str());
    return dtv::userland::host_ctl::run_control_cli(
        static_cast<int>(argv_views.size()), argv_views.data(), profile());
#else
    return dtv::userland::host_ctl::run_control_cli(
        argc, const_cast<const char* const*>(argv), profile());
#endif
}
