// SPDX-License-Identifier: MIT
#include <dtv/host_ts/px4_ts_core.h>
#include <dtv/userland/host_runtime/control_socket.h>
#include <dtv/userland/identity.h>

#include "pix_ts_profile.h"

#include <cstdio>
#include <string>
#include <vector>

namespace {

using px4::userland::cli::Px4TsArguments;
using px4::userland::cli::parse_px4_ts_product_arguments;

Px4TsArguments parse_product(std::vector<std::string> values)
{
    std::vector<const char*> argv;
    for (const auto& value : values) argv.push_back(value.c_str());
    return parse_px4_ts_product_arguments(static_cast<int>(argv.size()), argv.data(),
                                          pix::core_integration::kPixTsProfile);
}

bool check(bool condition, const char* message)
{
    if (!condition) std::fprintf(stderr, "FAIL: %s\n", message);
    return condition;
}

std::string canonical_daemon_instance(const char* spelling)
{
    const auto instance = dtv::userland::identity::canonicalize_instance(spelling);
    if (!instance) return std::string();
    return std::string(instance.value().bytes.data(), instance.value().length);
}

}  // namespace

int main()
{
    bool ok = true;
    for (const char* spelling : {"RoomA", "rooma", "ROOMA", "receiver.left"}) {
        const auto matched = parse_product({"pix-ts", "--instance", spelling,
                                            "--receiver", "0", "-c", "T13"});
        ok &= check(matched.valid && matched.instance == spelling,
                    "product profile preserves the explicit instance spelling");
    }
    for (const char* rejected : {".", "..", "room/a", "room:a", "00001205000960"}) {
        ok &= check(!parse_product({"pix-ts", "--instance", rejected,
                                    "--receiver", "0", "-c", "T13"}).valid,
                    "product profile rejects an invalid instance token");
    }
    const std::string too_long(81U, 'a');
    ok &= check(!parse_product({"pix-ts", "--instance", too_long,
                                "--receiver", "0", "-c", "T13"}).valid,
                "product profile rejects an over-long instance token");

    for (const char* spelling : {"RoomA", "rooma"}) {
        const auto ts_arguments = parse_product({"pix-ts", "--instance", spelling,
                                                 "--receiver", "0", "-c", "T13",
                                                 "--runtime-dir", "/tmp/pix-ts-case"});
        const std::string daemon_instance = canonical_daemon_instance(spelling);
        ok &= check(ts_arguments.valid && !daemon_instance.empty() &&
                        ts_arguments.instance == daemon_instance,
                    "TS and pixd agree on the instance spelling");
        const auto ts_endpoint = dtv::userland::host_runtime::control_endpoint_path(
            {ts_arguments.runtime_directory, "pix-userland", ts_arguments.instance});
        const auto daemon_endpoint = dtv::userland::host_runtime::control_endpoint_path(
            {"/tmp/pix-ts-case", "pix-userland", daemon_instance});
        ok &= check(ts_endpoint && daemon_endpoint &&
                        ts_endpoint.value() == daemon_endpoint.value(),
                    "TS and pixd derive the same control endpoint");
        if (ts_endpoint) {
            const std::string suffix = std::string("/") + spelling + "/control.sock";
            ok &= check(ts_endpoint.value().find(suffix) != std::string::npos,
                        "control endpoint keeps the instance spelling");
        }
    }
    return ok ? 0 : 1;
}
