// SPDX-License-Identifier: MIT
#include "pix_runtime.h"

#include <dtv/userland/host_listener_context/enclosure_context.h>
#include <dtv/userland/host_runtime/control_socket.h>
#include <dtv/userland/discovery_json.h>
#include "dtv/userland/identity.h"
#include <dtv/userland/json.h>
#include <libusb.h>
#include <px4/posix_tuner_nonce.h>
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
using namespace dtv::userland::host_listener_context;
using namespace dtv::userland::host_runtime;

struct Arguments final {
    std::string device;
    std::string instance;
    std::string runtime_directory;
    std::string assets = "assets";
    bool group = false;
};

void usage(FILE* output) noexcept
{
    std::fprintf(output,
        "usage: pixd --device serial:SELECTOR|usb:SELECTOR [--assets DIR] "
        "[--runtime-dir DIR] [--instance TOKEN] [--group] [-h|--help]\n"
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
        if (option == "--group") {
            if (out.group) return false;
            out.group = true;
            continue;
        }
        if (i + 1 >= argc) return false;
        const char* value = argv[++i];
        if (option == "--device" && !device) { out.device = value; device = true; }
        else if (option == "--instance" && !instance) { out.instance = value; instance = true; }
        else if (option == "--runtime-dir" && !runtime) { out.runtime_directory = value; runtime = true; }
        else if (option == "--assets" && !assets) { out.assets = value; assets = true; }
        else return false;
    }
    return device && (!out.group || (runtime && !out.runtime_directory.empty())) &&
           (out.device.rfind("serial:", 0U) == 0U ||
            out.device.rfind("usb:", 0U) == 0U);
}

int write_models()
{
    static constexpr char text[] =
        "PIX-DT181-PU0 06b8:102b\n"
        "PIX-DT295 06b8:103b\n";
    return std::fwrite(text, 1U, sizeof(text) - 1U, stdout) == sizeof(text) - 1U &&
                   std::fflush(stdout) == 0
        ? 0 : 70;
}


// Passive USB observation is separate from the actual receiver runtime.
int write_discovery_json()
{
    namespace w = dtv::userland::wire;
    auto copy = [](auto& dest, std::string_view value) -> bool {
        if (value.empty() || value.size() > dest.bytes.size()) return false;
        dest.length = static_cast<std::uint16_t>(value.size());
        std::memcpy(dest.bytes.data(), value.data(), value.size());
        return true;
    };
    auto set_optional = [&copy](auto& dest, std::string_view value) -> bool {
        dest.present = true;
        return copy(dest.value, value);
    };
    libusb_context* context = nullptr;
    const int started = libusb_init(&context);
    if (started != LIBUSB_SUCCESS) {
        std::fprintf(stderr, "pixd: USB discovery initialization failed (%d)\n", started);
        return 7;
    }
    libusb_device** usb_devices = nullptr;
    const ssize_t count = libusb_get_device_list(context, &usb_devices);
    if (count < 0) {
        std::fprintf(stderr, "pixd: USB enumeration failed (%d)\n",
                     static_cast<int>(count));
        libusb_exit(context);
        return 7;
    }
    std::vector<w::DiscoveryDevice> devices;
    std::vector<w::DiscoveryReceiver> receivers;
    std::vector<w::DiscoveryEnclosure> enclosures;
    std::vector<w::DiscoveryUsbObservation> ungrouped;
    const auto capacity = static_cast<std::size_t>(count);
    devices.reserve(capacity);
    receivers.reserve(capacity);
    enclosures.reserve(capacity);
    ungrouped.reserve(capacity);
    for (ssize_t i = 0; i < count; ++i) {
        libusb_device* usb = usb_devices[i];
        libusb_device_descriptor desc{};
        if (libusb_get_device_descriptor(usb, &desc) != LIBUSB_SUCCESS) continue;
        if (desc.idVendor != 0x06b8U ||
            (desc.idProduct != 0x102bU && desc.idProduct != 0x103bU)) continue;
        const bool dt181 = desc.idProduct == 0x102bU;
        const char* model = dt181 ? "PIX-DT181-PU0" : "PIX-DT295";
        w::DiscoveryDevice device{};
        device.device = static_cast<std::uint16_t>(devices.size());
        device.vid = desc.idVendor;
        device.pid = desc.idProduct;
        if (desc.iSerialNumber != 0U) {
            libusb_device_handle* handle = nullptr;
            if (libusb_open(usb, &handle) == LIBUSB_SUCCESS && handle) {
                unsigned char characters[256]{};
                const int length = libusb_get_string_descriptor_ascii(
                    handle, desc.iSerialNumber, characters, sizeof(characters));
                if (length > 0 &&
                    static_cast<std::size_t>(length) <= device.serial.value.bytes.size()) {
                    bool valid = true;
                    for (int n = 0; n < length; ++n)
                        if (characters[n] < 0x21U || characters[n] > 0x7eU)
                            valid = false;
                    if (valid) {
                        device.serial.present = true;
                        device.serial.value.length = static_cast<std::uint16_t>(length);
                        std::memcpy(device.serial.value.bytes.data(), characters,
                                    static_cast<std::size_t>(length));
                    }
                }
                libusb_close(handle);
            }
        }
        device.location.bus = {true, libusb_get_bus_number(usb)};
        device.location.address = {true, libusb_get_device_address(usb)};
        std::uint8_t ports[8]{};
        const int port_count = libusb_get_port_numbers(usb, ports, sizeof(ports));
        bool canonical_location = false;
        if (port_count > 0 && port_count <= static_cast<int>(sizeof(ports))) {
            std::string path = std::to_string(device.location.bus.value);
            for (int port = 0; port < port_count; ++port) {
                path.push_back(port == 0 ? '-' : '.');
                path += std::to_string(ports[port]);
            }
            if (dtv::userland::identity::parse_device_selector("usb:" + path) &&
                set_optional(device.location.port, path)) canonical_location = true;
        }
        if (!canonical_location) {
            w::DiscoveryUsbObservation observation{};
            observation.serial = device.serial;
            observation.model.present = true;
            if (!copy(observation.model.value, model)) {
                libusb_free_device_list(usb_devices, 1);
                libusb_exit(context);
                return 70;
            }
            observation.vid = device.vid;
            observation.pid = device.pid;
            observation.status = w::DiscoveryObservationStatus::unknown;
            observation.location = device.location;
            ungrouped.push_back(observation);
            continue;
        }
        w::DiscoveryReceiver receiver{};
        receiver.receiver = 0U;
        receiver.device = device.device;
        receiver.local = 0U;
        receiver.systems.mask = dt181 ? 0x03U : 0x01U;
        receivers.push_back(receiver);
        device.receivers_known = true;
        device.receiver_count = 1U;
        device.receivers = &receivers.back();
        devices.push_back(device);
        w::DiscoveryEnclosure enclosure{};
        enclosure.enclosure = static_cast<std::uint16_t>(enclosures.size());
        enclosure.model.present = true;
        if (!copy(enclosure.model.value, model)) {
            libusb_free_device_list(usb_devices, 1);
            libusb_exit(context);
            return 70;
        }
        enclosure.serial = device.serial;
        enclosure.status = w::DiscoveryEnclosureStatus::ready;
        enclosure.readers_known = true;
        enclosure.device_count = 1U;
        enclosure.devices = &devices.back();
        enclosures.push_back(enclosure);
    }
    libusb_free_device_list(usb_devices, 1);
    libusb_exit(context);

    w::DiscoveryScan scan{};
    scan.complete = true;
    if (!copy(scan.product, "pix-userland") ||
        !copy(scan.version, PIX_RUNTIME_VERSION)) return 70;
    scan.enclosure_count = enclosures.size();
    scan.enclosures = enclosures.data();
    scan.ungrouped_usb_device_count = ungrouped.size();
    scan.ungrouped_usb_devices = ungrouped.data();
    const auto size = w::discovery_json_size(scan);
    if (!size) return 70;
    std::vector<std::uint8_t> bytes(size.value());
    const auto result = w::encode_discovery_json(scan, {bytes.data(), bytes.size()});
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
    if (!located) {
        std::fprintf(stderr, "PIX_STAGE locate error=%u\n", static_cast<unsigned>(located.error()));
        return fail(located.error());
    }
    auto family_result = pix::runtime::Family::create(located.value(), arguments.assets.c_str());
    if (!family_result) {
        std::fprintf(stderr, "PIX_STAGE family error=%u\n", static_cast<unsigned>(family_result.error()));
        return fail(family_result.error());
    }
    std::unique_ptr<EnclosureFamily> family(std::move(family_result.value()));

    const auto parsed_selector = dtv::userland::identity::parse_device_selector(arguments.device);
    if (!parsed_selector) return fail(parsed_selector.error());
    const auto resolved = dtv::userland::identity::resolve_selector(
        dtv::userland::identity::Product::pix, parsed_selector.value());
    if (!resolved) return fail(resolved.error());
    const auto automatic = dtv::userland::identity::auto_instance(resolved.value());
    if (!automatic) return fail(automatic.error());
    std::string instance;
    if (arguments.instance.empty()) {
        instance.assign(automatic.value().bytes.data(), automatic.value().length);
    } else {
        const auto canonical = dtv::userland::identity::canonicalize_instance(arguments.instance);
        if (!canonical) return fail(Error::INVALID_ARGUMENT);
        instance.assign(canonical.value().bytes.data(), canonical.value().length);
    }
    std::unique_ptr<TunerNonceSource> nonce_source(new (std::nothrow)
        px4::userland::ipc::posix::PosixTunerNonceSource());
    if (!nonce_source) return fail(Error::RESOURCE_LIMIT);
    const auto epoch = nonce_source->generate();
    if (!epoch) return fail(epoch.error());
    dtv::userland::wire::ListResponse list{};
    const auto listed = pix::runtime::build_list(located.value(), PIX_RUNTIME_VERSION,
                                                  epoch.value(), list);
    if (!listed) return fail(listed.error());
    ContextConfig config{};
    config.runtime_directory = arguments.runtime_directory;
    if (arguments.group)
        config.access = dtv::userland::host_runtime::EndpointAccess::shared_group;
    config.product = "pix-userland";
    config.instance = instance;
    config.server.epoch = epoch.value();
    config.server.producer = list.producer;
    config.canonical_list = list;
    config.context.epoch = epoch.value();
    config.context.instance.present = true;
    config.context.instance.value.length = static_cast<std::uint16_t>(instance.size());
    std::memcpy(config.context.instance.value.bytes.data(), instance.data(), instance.size());
    for (std::size_t i = 0U; i < sizeof(config.stream_engine_epoch); ++i)
        config.stream_engine_epoch = (config.stream_engine_epoch << 8U) |
            epoch.value()[i];
    if (config.stream_engine_epoch == 0U) config.stream_engine_epoch = 1U;
    config.stream_arm_budget_ms = 5000U;
    auto assembled = EnclosureContext::assemble(
        std::move(family), std::move(nonce_source), config);
    if (!assembled) return fail(assembled.error());
    std::unique_ptr<EnclosureContext> context(std::move(assembled.value()));
    const auto reserved = context->reserve_namespace();
    if (!reserved) {
        std::fprintf(stderr, "PIX_STAGE reserve error=%u\n", static_cast<unsigned>(reserved.error()));
        return fail(reserved.error());
    }
    const auto published = context->publish();
    if (!published) {
        std::fprintf(stderr, "PIX_STAGE publish error=%u\n", static_cast<unsigned>(published.error()));
        return fail(published.error());
    }
    if (!px4::userland::px4d::install_signal_handlers()) return 70;
    std::fprintf(stderr, "pixd ready: model=%s endpoint=%s\n",
                 model_name(located.value().model),
                 config.runtime_directory.empty() ? "Core-selected" : config.runtime_directory.c_str());
    Error loop_error = Error::OK;
    while (!px4::userland::px4d::stop_requested()) {
        const auto polled = context->poll_once(Timeout{100U});
        if (!polled) {
            loop_error = polled.error();
            std::fprintf(stderr, "PIX_CONTEXT_POLL error=%u\n",
                         static_cast<unsigned>(loop_error));
            break;
        }
    }
#ifdef PIX_RUNTIME_TEST_CLOSE_BUDGET_MS
    const auto closed = context->close(Timeout{PIX_RUNTIME_TEST_CLOSE_BUDGET_MS});
#else
    const auto closed = context->close(Timeout{15000U});
#endif
#ifdef PIX_RUNTIME_TEST_CLOSE_BUDGET_MS
    if (closed && closed.value() != CloseState::quiesced)
        std::fprintf(stderr, "PIX_CLOSE deadline_expired=1\n");
#endif
    if (!closed || closed.value() != CloseState::quiesced) {
        std::fprintf(stderr, "PIX_CONTEXT_CLOSE result=%s error=%u\n",
            closed ? "not-quiesced" : "failed",
            closed ? 0U : static_cast<unsigned>(closed.error()));
        return 70;
    }
    if (loop_error != Error::OK) return fail(loop_error);
    return 0;
}
