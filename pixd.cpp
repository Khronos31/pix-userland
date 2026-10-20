// SPDX-License-Identifier: GPL-2.0-or-later
#include "pix_runtime.h"

#include "dtv/userland/identity.h"
#include <dtv/userland/json.h>
#include <libusb.h>
#include "px4/posix_tuner_nonce.h"
#include "px4d_signals.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <vector>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <random>
#include <limits>

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
        "[--runtime-dir DIR] [--instance TOKEN] [-h|--help]\n"
        "       pixd --version\n"
        "       pixd --models\n"
        "       pixd --list-json\n");
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

int write_models()
{
    namespace w = dtv::userland::wire;
    // The product supports these USB models, independent of which are
    // currently attached. The daemon's runtime paths are verified separately.
    w::SupportedModel models[2]{};
    const char* names[] = {"PIX-DT181-PU0", "PIX-DT295"};
    const w::SupportedUsbId ids[] = {{0x06b8U, 0x102bU}, {0x06b8U, 0x103bU}};
    for (std::size_t i = 0; i < 2; ++i) {
        const std::size_t size = std::strlen(names[i]);
        if (size == 0U || size > models[i].name.bytes.size()) return 70;
        models[i].name.length = static_cast<std::uint16_t>(size);
        std::memcpy(models[i].name.bytes.data(), names[i], size);
        models[i].usb_ids.count = 1U;
        models[i].usb_ids.items[0] = ids[i];
    }
    const w::SupportedModels list{{models, 2U}};
    const auto size = w::models_text_size(list);
    if (!size) return 70;
    std::vector<std::uint8_t> bytes(size.value());
    const auto encoded = w::encode_models_text(list, {bytes.data(), bytes.size()});
    if (!encoded || encoded.value() != bytes.size() ||
        std::fwrite(bytes.data(), 1U, bytes.size(), stdout) != bytes.size() ||
        std::fflush(stdout) != 0) return 70;
    return 0;
}


// Passive USB observation is separate from the actual receiver runtime.
// DT295 may be physically identified by USB serial even before its existing
// capture backend is connected to the public pixd daemon.
int write_discovery_json()
{
    namespace w = dtv::userland::wire;
    auto copy = [](auto& dest, const char* text) -> bool {
        const std::size_t n = std::strlen(text);
        if (n == 0U || n > dest.bytes.size()) return false;
        dest.length = static_cast<std::uint16_t>(n);
        std::memcpy(dest.bytes.data(), text, n);
        return true;
    };
    libusb_context* context = nullptr;
    const int started = libusb_init(&context);
    if (started != LIBUSB_SUCCESS) {
        std::fprintf(stderr, "pixd: USB discovery initialization failed (%d)\n", started);
        return 7;
    }
    libusb_device** devices = nullptr;
    const ssize_t count = libusb_get_device_list(context, &devices);
    if (count < 0) {
        std::fprintf(stderr, "pixd: USB enumeration failed (%d)\n",
                     static_cast<int>(count));
        libusb_exit(context);
        return 7;
    }
    std::vector<w::DiscoveryFunction> functions;
    std::vector<w::DiscoveryEnclosure> enclosures;
    for (ssize_t i = 0; i < count; ++i) {
        libusb_device* device = devices[i];
        libusb_device_descriptor desc{};
        if (libusb_get_device_descriptor(device, &desc) != LIBUSB_SUCCESS)
            continue;
        if (desc.idVendor != 0x06b8U ||
            (desc.idProduct != 0x102bU && desc.idProduct != 0x103bU))
            continue;
        if (functions.size() >= std::numeric_limits<std::uint16_t>::max()) {
            libusb_free_device_list(devices, 1);
            libusb_exit(context);
            return 70;
        }
        const bool public_runtime = desc.idProduct == 0x102bU;
        const char* model = public_runtime ? "PIX-DT181-PU0" : "PIX-DT295";
        w::DiscoveryFunction function{};
        auto& usb = function.observation;
        usb.id = static_cast<std::uint16_t>(functions.size());
        usb.vid = desc.idVendor;
        usb.pid = desc.idProduct;
        usb.mode = w::Mode::runtime;
        usb.present = true;
        // The DT295 exposes a usable USB iSerialNumber descriptor. Read it
        // without claiming a USB interface or changing firmware/device mode.
        usb.serial_state = desc.iSerialNumber == 0U
            ? w::SerialState::absent : w::SerialState::unreadable;
        if (desc.iSerialNumber != 0U) {
            libusb_device_handle* handle = nullptr;
            if (libusb_open(device, &handle) == LIBUSB_SUCCESS && handle) {
                unsigned char characters[256]{};
                const int length = libusb_get_string_descriptor_ascii(
                    handle, desc.iSerialNumber, characters, sizeof(characters));
                if (length > 0 &&
                    static_cast<std::size_t>(length) <= usb.serial.value.bytes.size()) {
                    bool valid = true;
                    for (int n = 0; n < length; ++n)
                        if (characters[n] < 0x21U || characters[n] > 0x7eU)
                            valid = false;
                    usb.serial_state = valid ? w::SerialState::present
                                             : w::SerialState::invalid;
                    if (valid) {
                        usb.serial.present = true;
                        usb.serial.value.length = static_cast<std::uint16_t>(length);
                        std::memcpy(usb.serial.value.bytes.data(), characters,
                                    static_cast<std::size_t>(length));
                    }
                } else if (length > 0) {
                    usb.serial_state = w::SerialState::invalid;
                }
                libusb_close(handle);
            }
        }
        usb.model.present = true;
        if (!copy(usb.model.value.id, model) ||
            !copy(usb.model.value.name, model)) {
            libusb_free_device_list(devices, 1);
            libusb_exit(context);
            return 70;
        }
        usb.location.bus = {true, libusb_get_bus_number(device)};
        usb.location.address = {true, libusb_get_device_address(device)};
        std::uint8_t ports[8]{};
        const int nports = libusb_get_port_numbers(device, ports, sizeof(ports));
        bool canonical_location = false;
        if (nports > 0 && nports <= static_cast<int>(sizeof(ports))) {
            std::string path = std::to_string(usb.location.bus.value);
            for (int port = 0; port < nports; ++port) {
                path.push_back(port == 0 ? '-' : '.');
                path += std::to_string(ports[port]);
            }
            const std::string selector = "usb:" + path;
            if (dtv::userland::identity::parse_device_selector(selector) &&
                path.size() <= usb.location.usb_path.value.bytes.size() &&
                selector.size() <= function.selector_candidates.items[0].value.bytes.size()) {
                usb.location.usb_path.present = true;
                usb.location.usb_path.value.length = static_cast<std::uint16_t>(path.size());
                std::memcpy(usb.location.usb_path.value.bytes.data(),
                            path.data(), path.size());
                auto& candidate = function.selector_candidates.items[0];
                candidate.value.length = static_cast<std::uint16_t>(selector.size());
                std::memcpy(candidate.value.bytes.data(), selector.data(), selector.size());
                candidate.state = public_runtime
                    ? w::DiscoveryCandidateState::usable
                    : w::DiscoveryCandidateState::unavailable;
                function.selector_candidates.count = 1U;
                canonical_location = true;
            }
        }
        // Physical enclosure identity is known even when DT295 has no
        // published pixd streaming backend. Do not publish fake receivers.
        const bool bound = canonical_location;
        if (bound) {
            function.grouping = w::DiscoveryGroupingState::bound;
            function.enclosure_id = {true, static_cast<std::uint16_t>(enclosures.size())};
        } else {
            function.grouping = w::DiscoveryGroupingState::unresolved;
            function.grouping_reason.present = copy(function.grouping_reason.value,
                public_runtime ? "usb_path_unavailable" : "not_available_in_public_daemon");
        }
        functions.push_back(std::move(function));
        if (!bound) continue;
        w::DiscoveryEnclosure enclosure{};
        enclosure.descriptor.id = static_cast<std::uint16_t>(enclosures.size());
        enclosure.descriptor.model.present = true;
        if (!copy(enclosure.descriptor.model.value.id, model) ||
            !copy(enclosure.descriptor.model.value.name, model)) {
            libusb_free_device_list(devices, 1);
            libusb_exit(context);
            return 70;
        }
        const auto& detected = functions.back().observation;
        enclosure.descriptor.identity.serial_state =
            detected.serial_state == w::SerialState::present
                ? w::IdentitySerialState::present
                : detected.serial_state == w::SerialState::absent
                    ? w::IdentitySerialState::absent : w::IdentitySerialState::unknown;
        enclosure.descriptor.identity.uniqueness =
            detected.serial_state == w::SerialState::absent
                ? w::Uniqueness::not_applicable : w::Uniqueness::unknown;
        if (detected.serial.present) {
            enclosure.descriptor.identity.serial.present = true;
            enclosure.descriptor.identity.serial.value = detected.serial.value;
        }
        if (public_runtime)
            enclosure.descriptor.limits.max_concurrent_streams = {true, 1U};
        enclosure.topology.state = public_runtime ? w::TopologyState::complete
                                                  : w::TopologyState::unknown;
        enclosure.topology.expected_function_count = {true, 1U};
        enclosure.usb_function_ids.count = 1U;
        enclosure.usb_function_ids.items[0] = functions.back().observation.id;
        enclosure.selector_candidates.count = 1U;
        enclosure.selector_candidates.items[0] =
            functions.back().selector_candidates.items[0];
        if (public_runtime) {
        enclosure.receivers.present = true;
        enclosure.receivers.value.count = 1U;
        auto& receiver = enclosure.receivers.value.items[0];
        receiver.id = 0U;
        receiver.systems.mask = 3U; // Existing DT181 runtime: ISDB-T/S.
        receiver.binding.present = true;
        receiver.binding.value = {functions.back().observation.id, 0U};
        // Same default LNA behavior already reported by pix_runtime::build_list.
        receiver.capabilities.controls |= w::kControlBitLna;
        receiver.capabilities.lna.entries.count = 2U;
        receiver.capabilities.lna.entries.items[0].system = w::System::isdb_t;
        receiver.capabilities.lna.entries.items[0].default_value = {true, false};
        receiver.capabilities.lna.entries.items[1].system = w::System::isdb_s;
        receiver.capabilities.lna.entries.items[1].default_value = {true, true};
        enclosure.readers.present = true; // DT181 public runtime reports [].
        } // DT295: intentionally omit unknown public receiver/reader collections.
        enclosures.push_back(std::move(enclosure));
    }
    libusb_free_device_list(devices, 1);
    libusb_exit(context);

    w::DiscoveryResponse document{};
    if (!copy(document.producer.product, "pix-userland") ||
        !copy(document.producer.version, PIX_RUNTIME_VERSION)) return 70;
    std::random_device entropy;
    for (auto& byte : document.scan_id)
        byte = static_cast<std::uint8_t>(entropy());
    document.usb_functions = {functions.data(), functions.size()};
    document.enclosures = {enclosures.data(), enclosures.size()};
    const auto size = w::discovery_json_size(document);
    if (!size) return 70;
    std::vector<std::uint8_t> bytes(size.value());
    const auto result = w::encode_discovery_json(document, {bytes.data(), bytes.size()});
    if (!result || result.value() != bytes.size() ||
        std::fwrite(bytes.data(), 1U, bytes.size(), stdout) != bytes.size() ||
        std::fflush(stdout) != 0) return 70;
    return 0;
}

int fail(Error error) noexcept
{
    std::fprintf(stderr, "pixd: %s\n", error_string(error));
    return error == Error::INVALID_ARGUMENT ? 2 : error == Error::NOT_FOUND ? 3 : 70;
}

// "pixd ready" reports the model the running enclosure family was built from.
// DT295 selection is resolved by the locator; whether its daemon backend can
// start is decided by Family::create, not by this label.
const char* model_name(pix::runtime::Model model) noexcept
{
    return model == pix::runtime::Model::dt295 ? "PIX-DT295" : "PIX-DT181-PU0";
}

}  // namespace

int main(int argc, char** argv)
{
    if (argc == 2 && argv != nullptr && argv[1] != nullptr) {
        const std::string_view option(argv[1]);
        if (option == "--version") {
            std::printf("pix-userland %s\n", PIX_RUNTIME_VERSION);
            return 0;
        }
        if (option == "--models") return write_models();
        if (option == "--list-json") return write_discovery_json();
    }
    Arguments arguments;
    bool help = false;
    if (!parse(argc, argv, arguments, help)) { usage(stderr); return 2; }
    if (help) { usage(stdout); return 0; }

    const auto located = pix::runtime::locate(arguments.device);
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
    std::fprintf(stderr, "pixd ready: model=%s endpoint=%s\n",
                 model_name(located.value().model), context->endpoint_path());
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
