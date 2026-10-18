// SPDX-License-Identifier: GPL-2.0-or-later
#include "pix_runtime.h"

#include "dtv/userland/identity.h"
#include "px4/posix_tuner_nonce.h"
#include "px4d_signals.h"

#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>

namespace {

using namespace px4::userland;
using namespace px4::userland::ipc::posix;

struct Arguments final {
    std::string device;
    std::string instance;
    std::string runtime_directory;
    std::string assets = "assets";
};

void usage(FILE* output) noexcept
{
    std::fprintf(output,
        "usage: pixd --device serial:SELECTOR|usb:SELECTOR [--assets DIR] "
        "[--runtime-dir DIR] [--instance TOKEN] [-h|--help]\n");
}

bool parse(int argc, char** argv, Arguments& out, bool& help) noexcept
{
    bool device = false, instance = false, runtime = false, assets = false;
    for (int i = 1; i < argc; ++i) {
        const std::string_view option(argv[i]);
        if (option == "-h" || option == "--help") { help = true; return true; }
        if (i + 1 >= argc) return false;
        const char* value = argv[++i];
        if (option == "--device" && !device) { out.device = value; device = true; }
        else if (option == "--instance" && !instance) { out.instance = value; instance = true; }
        else if (option == "--runtime-dir" && !runtime) { out.runtime_directory = value; runtime = true; }
        else if (option == "--assets" && !assets) { out.assets = value; assets = true; }
        else return false;
    }
    return device && (out.device.rfind("serial:", 0U) == 0U ||
                      out.device.rfind("usb:", 0U) == 0U);
}

int fail(Error error) noexcept
{
    std::fprintf(stderr, "pixd: %s\n", error_string(error));
    return error == Error::INVALID_ARGUMENT ? 2 : error == Error::NOT_FOUND ? 3 : 70;
}

}  // namespace

int main(int argc, char** argv)
{
    Arguments arguments;
    bool help = false;
    if (!parse(argc, argv, arguments, help)) { usage(stderr); return 2; }
    if (help) { usage(stdout); return 0; }

    const auto located = pix::runtime::locate_dt181(arguments.device);
    if (!located) return fail(located.error());
    auto family_result = pix::runtime::Family::create(located.value(), arguments.assets.c_str());
    if (!family_result) return fail(family_result.error());
    std::unique_ptr<EnclosureFamily> family(std::move(family_result.value()));
    auto assembled = EnclosureContext::assemble(
        std::move(family), std::unique_ptr<TunerNonceSource>(new (std::nothrow) PosixTunerNonceSource()));
    if (!assembled) return fail(assembled.error());
    std::unique_ptr<EnclosureContext> context(std::move(assembled.value()));

    const auto parsed_selector = dtv::userland::identity::parse_device_selector(arguments.device);
    if (!parsed_selector) return fail(parsed_selector.error());
    const auto resolved = dtv::userland::identity::resolve_selector(
        dtv::userland::identity::Product::pix, parsed_selector.value());
    if (!resolved) return fail(resolved.error());
    const auto automatic = dtv::userland::identity::auto_instance(resolved.value());
    if (!automatic) return fail(automatic.error());
    const std::string lease_identity(automatic.value().bytes.data(), automatic.value().length);
    std::string instance;
    if (arguments.instance.empty()) {
        instance.assign(automatic.value().bytes.data(), automatic.value().length);
    } else {
        const auto canonical = dtv::userland::identity::canonicalize_instance(arguments.instance);
        if (!canonical) return fail(Error::INVALID_ARGUMENT);
        instance.assign(canonical.value().bytes.data(), canonical.value().length);
    }
    std::string runtime_directory = arguments.runtime_directory;
    if (runtime_directory.empty()) {
        const auto selected = select_publish_runtime_root();
        if (!selected) return fail(selected.error());
        runtime_directory.assign(selected.value().data());
    }
    EnclosurePublication publication{};
    publication.runtime_directory = runtime_directory;
    publication.instance = instance;
    publication.lease_identity = lease_identity;
    const auto reserved = context->reserve_namespace(publication);
    if (!reserved) return fail(reserved.error());

    PosixTunerNonceSource nonce_source;
    const auto epoch = nonce_source.generate();
    if (!epoch) return fail(epoch.error());
    ControlContext control{};
    const auto listed = pix::runtime::build_list(located.value(), instance, "0.0.0-dev",
                                                  epoch.value(), control);
    if (!listed) return fail(listed.error());
    const auto published = context->publish(control);
    if (!published) return fail(published.error());
    if (!px4::userland::px4d::install_signal_handlers()) return 70;
    std::fprintf(stderr, "pixd ready: model=PIX-DT181-PU0 endpoint=%s\n",
                 context->endpoint_path());
    Error loop_error = Error::OK;
    while (!px4::userland::px4d::stop_requested()) {
        const auto polled = context->poll_once(Timeout{100U});
        if (!polled) { loop_error = polled.error(); break; }
    }
#ifdef PIX_RUNTIME_TEST_CLOSE_BUDGET_MS
    const ContextCloseResult closed = context->close(
        std::chrono::milliseconds(PIX_RUNTIME_TEST_CLOSE_BUDGET_MS));
#else
    const ContextCloseResult closed = context->close(std::chrono::milliseconds(15000));
#endif
#ifdef PIX_RUNTIME_TEST_CLOSE_BUDGET_MS
    if (!closed.quiesced) std::fprintf(stderr, "PIX_CLOSE deadline_expired=1\n");
#endif
    if (!closed.quiesced) return 70;
    if (loop_error != Error::OK) return fail(loop_error);
    if (closed.error != Error::OK) return fail(closed.error);
    return 0;
}
