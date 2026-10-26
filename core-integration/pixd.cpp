// SPDX-License-Identifier: MIT
#include "pix_runtime.h"

#include <dtv/userland/host_listener_context/enclosure_context.h>
#include <dtv/userland/host_runtime/control_socket.h>
#include <dtv/userland/discovery_json.h>
#include "dtv/userland/identity.h"
#include <dtv/userland/json.h>
#include <libusb.h>
#ifdef _WIN32
#include <px4/windows_tuner_nonce.h>
#include "px4_windows_args.h"
#include <dtv/userland/host_facade/windows_stdin.h>
#else
#include <px4/posix_tuner_nonce.h>
#endif
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

#if defined(PIX_RUNTIME_TEST_CLOSE_BUDGET_MS)
extern "C" int dt295_actual_fixture_open(const char*);
extern "C" void dt295_actual_fixture_configure_presence(unsigned, int);
extern "C" void dt295_actual_fixture_set_card_sample(unsigned, unsigned, unsigned);
#endif

struct Arguments final {
    std::string device;
    std::string instance;
    std::string runtime_directory;
#if !defined(PIX_EMBEDDED_ASSETS)
    std::string assets = "assets";
#endif
    bool group = false;
#ifdef _WIN32
    bool exit_on_stdin_eof = false;
#endif
};

void usage(FILE* output) noexcept
{
    std::fprintf(output,
#if defined(PIX_EMBEDDED_ASSETS)
        "usage: pixd --device serial:SELECTOR|usb:SELECTOR "
#else
        "usage: pixd --device serial:SELECTOR|usb:SELECTOR [--assets DIR] "
#endif
        "[--runtime-dir DIR] [--instance TOKEN] [--group] [-h|--help]\n"
        "       pixd --version\n"
        "       pixd --models\n"
        "       pixd --list-json\n"
        "       --device accepts serial:VALUE or usb:BUS-PORT[.PORT...]\n"
        "       serial:VALUE is 1..512 UTF-8 bytes without control characters.\n"
        "       --instance is 1..64 ASCII bytes, starts with a letter or digit,\n"
        "       then uses letters, digits, '.', '_' or '-'; avoid a trailing dot\n"
        "       and Windows reserved device names.\n"
        "       --runtime-dir must leave room for the complete local endpoint path;\n"
        "       its byte limit depends on this build's native socket path capacity.\n"
        "       If too long, shorten --runtime-dir or --instance.\n"
#ifdef _WIN32
        "       Windows: --exit-on-stdin-eof stops when parent stdin closes\n"
#endif
        );
}

bool parse(int argc, char** argv, Arguments& out, bool& help,
           const char*& error) noexcept
{
    bool device = false, instance = false, runtime = false;
#if !defined(PIX_EMBEDDED_ASSETS)
    bool assets = false;
#endif
    for (int i = 1; i < argc; ++i) {
        const std::string_view option(argv[i]);
        if (option == "-h" || option == "--help") { help = true; return true; }
#ifdef _WIN32
        if (option == "--exit-on-stdin-eof") {
            if (out.exit_on_stdin_eof) {
                error = "--exit-on-stdin-eof may be used once";
                return false;
            }
            out.exit_on_stdin_eof = true;
            continue;
        }
#endif
        if (option == "--group") {
            if (out.group) {
                error = "--group may be used once";
                return false;
            }
            out.group = true;
            continue;
        }
        const bool value_option = option == "--device" || option == "--instance" ||
            option == "--runtime-dir"
#if !defined(PIX_EMBEDDED_ASSETS)
            || option == "--assets"
#endif
            ;
        if (!value_option) {
            error = "unknown option";
            return false;
        }
        if (i + 1 >= argc) {
            error = option == "--device" ? "--device requires a selector value" :
                option == "--instance" ? "--instance requires a token value" :
                option == "--runtime-dir" ? "--runtime-dir requires a directory value" :
                "--assets requires a directory value";
            return false;
        }
        const char* value = argv[++i];
        if (option == "--device" && !device) { out.device = value; device = true; }
        else if (option == "--instance" && !instance) { out.instance = value; instance = true; }
        else if (option == "--runtime-dir" && !runtime) { out.runtime_directory = value; runtime = true; }
#if !defined(PIX_EMBEDDED_ASSETS)
        else if (option == "--assets" && !assets) { out.assets = value; assets = true; }
#endif
        else if (option == "--instance") {
            error = "--instance may be used once";
            return false;
        } else if (option == "--device" || option == "--runtime-dir" ||
                   option == "--assets") {
            error = "option may be used once";
            return false;
        } else {
            error = "unknown option";
            return false;
        }
    }
    if (!device) {
        error = "--device is required";
        return false;
    }
    if (out.group && (!runtime || out.runtime_directory.empty())) {
        error = "--group requires an explicit non-empty --runtime-dir";
        return false;
    }
    if (out.device.rfind("serial:", 0U) != 0U &&
        out.device.rfind("usb:", 0U) != 0U) {
        error = "--device must use serial:VALUE or usb:BUS-PORT[.PORT...]";
        return false;
    }
    if (out.device == "serial:" || out.device == "usb:") {
        error = "--device selector is empty; provide a serial or USB bus-port path";
        return false;
    }
    return true;
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
        receiver.systems.mask = 0x03U;
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

void print_endpoint_error(const char* program, std::string_view endpoint,
                          const EndpointDiagnostic& diagnostic) noexcept
{
    if (diagnostic.reason == EndpointDiagnosticReason::path_too_long) {
        std::fprintf(stderr,
            "%s: %.*s endpoint path is too long (%zu path bytes; %zu bytes including NUL; this build's native capacity is %zu bytes). Shorten --runtime-dir or --instance.\n",
            program, static_cast<int>(endpoint.size()), endpoint.data(),
            diagnostic.path_bytes, diagnostic.required_bytes,
            diagnostic.native_capacity);
        return;
    }
    if (diagnostic.reason == EndpointDiagnosticReason::runtime_directory) {
        std::fprintf(stderr,
            "%s: --runtime-dir must be a non-empty absolute local path on this platform.\n",
            program);
        return;
    }
    std::fprintf(stderr, "%s: cannot form the %.*s endpoint path (invalid %s component).\n",
        program, static_cast<int>(endpoint.size()), endpoint.data(),
        diagnostic.reason == EndpointDiagnosticReason::product ? "product" :
        diagnostic.reason == EndpointDiagnosticReason::instance ? "instance" : "endpoint");
}

void print_listener_error(const ListenerDiagnostic& diagnostic, Error error) noexcept
{
    if (diagnostic.reason == ListenerDiagnosticReason::endpoint_path) {
        print_endpoint_error("pixd", diagnostic.endpoint_name, diagnostic.endpoint);
        return;
    }
    const char* reason = "runtime directory could not be checked";
    const char* advice = "Verify the runtime directory configuration and retry.";
    switch (diagnostic.reason) {
    case ListenerDiagnosticReason::runtime_directory_missing:
        reason = "runtime directory is missing";
        advice = "Create it or choose another --runtime-dir.";
        break;
    case ListenerDiagnosticReason::runtime_directory_lookup:
        reason = "runtime directory lookup failed";
        advice = "Verify that --runtime-dir can be inspected.";
        break;
    case ListenerDiagnosticReason::runtime_directory_type:
        reason = "runtime path is not a directory";
        advice = "Choose a directory for --runtime-dir.";
        break;
    case ListenerDiagnosticReason::runtime_directory_owner:
        reason = "runtime directory has an unexpected owner";
        advice = "Check --runtime-dir ownership.";
        break;
    case ListenerDiagnosticReason::runtime_directory_mode:
        reason = "runtime directory permissions are too broad";
        advice = "Restrict --runtime-dir permissions.";
        break;
    case ListenerDiagnosticReason::runtime_directory_security:
        reason = "runtime directory security check failed";
        advice = "Review the platform runtime directory security requirements.";
        break;
    default:
        if (error == Error::BUSY) {
            std::fprintf(stderr,
                "pixd: the PIX endpoint namespace is already in use. Stop the existing pixd for this instance or choose another --instance.\n");
            return;
        }
        if (error == Error::INVALID_ARGUMENT) {
            advice = "Verify --runtime-dir and --instance.";
        } else if (error == Error::NOT_READY) {
            advice = "Choose another runtime endpoint or retry after it is available.";
        }
        std::fprintf(stderr,
            "pixd: cannot reserve the PIX endpoint namespace (error=%u); %s\n",
            static_cast<unsigned>(error), advice);
        return;
    }
    std::fprintf(stderr,
        "pixd: cannot reserve the PIX endpoint namespace: %s. %s\n",
        reason, advice);
}

}  // namespace

int run(int argc, char** argv)
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
    const char* parse_error = "invalid command line";
    if (!parse(argc, argv, arguments, help, parse_error)) {
        std::fprintf(stderr, "pixd: %s\n", parse_error);
        usage(stderr);
        return 2;
    }
    if (help) { usage(stdout); return 0; }

    const auto parsed_selector = dtv::userland::identity::parse_device_selector(arguments.device);
    if (!parsed_selector) {
        std::fprintf(stderr,
            "pixd: invalid --device selector; serial:VALUE requires 1..512 valid UTF-8 bytes without control characters, and usb:VALUE requires a valid PIX USB topology.\n");
        return 2;
    }
    if (parsed_selector.value().kind ==
        dtv::userland::identity::SelectorKind::usb_topology) {
        if (parsed_selector.value().topology.count != 1U) {
            std::fprintf(stderr,
                "pixd: PIX accepts one usb:BUS-PORT[.PORT...] topology per --device.\n");
            return 2;
        }
        usb_selector selector{};
        if (usb_selector_parse(arguments.device.c_str() + 4U, &selector) != 0 ||
            selector.kind != USB_SELECT_PATH) {
            std::fprintf(stderr,
                "pixd: invalid --device USB topology; use usb:BUS-PORT[.PORT...] with decimal bus and port numbers.\n");
            return 2;
        }
    }
#if defined(PIX_RUNTIME_TEST_CLOSE_BUDGET_MS)
#if defined(PIX_EMBEDDED_ASSETS)
    const bool fake_dt295 = false;
#else
    const bool fake_dt295 = std::getenv("PIX_TEST_DT295_FAKE") != nullptr &&
                            arguments.device == "serial:PIXDT29501";
    if (fake_dt295) {
        std::string bundle = arguments.assets;
        if (!bundle.empty() && bundle.back() != '/') bundle.push_back('/');
        bundle += "dt295.assets";
        if (dt295_actual_fixture_open(bundle.c_str()) != 0) return 70;
        const char* raw_value = std::getenv("PIX_TEST_DT295_RAW");
        const unsigned raw_state = raw_value == nullptr ? 3U :
            static_cast<unsigned>(std::strtoul(raw_value, nullptr, 10));
        const int notification = std::getenv("PIX_TEST_DT295_NOTIFICATION") != nullptr;
        dt295_actual_fixture_configure_presence(raw_state, notification);
    }
#endif
#endif

    const auto located = pix::runtime::locate(arguments.device);
    if (!located) {
        std::fprintf(stderr, "PIX_STAGE locate error=%u\n", static_cast<unsigned>(located.error()));
        return fail(located.error());
    }
    pix::runtime::RuntimeFailureDiagnostic family_diagnostic{};
#if defined(PIX_EMBEDDED_ASSETS)
    auto family_result = pix::runtime::Family::create(
        located.value(), nullptr, &family_diagnostic);
#else
    auto family_result = pix::runtime::Family::create(
        located.value(), arguments.assets.c_str(), &family_diagnostic);
#endif
    if (!family_result) {
        std::fprintf(stderr, "PIX_STAGE family error=%u\n", static_cast<unsigned>(family_result.error()));
        if (family_diagnostic.asset[0] != '\0') {
#if defined(PIX_EMBEDDED_ASSETS)
            std::fprintf(stderr, "pixd: embedded PIX asset %s: %s",
                family_diagnostic.asset.data(), family_diagnostic.detail.data());
#else
            std::fprintf(stderr, "pixd: --assets entry %s: %s",
                family_diagnostic.asset.data(), family_diagnostic.detail.data());
#endif
            if (family_diagnostic.byte_counts_valid) {
                std::fprintf(stderr, " (expected %zu bytes, found %s%zu)",
                    family_diagnostic.expected_bytes,
                    family_diagnostic.actual_is_lower_bound ? "at least " : "",
                    family_diagnostic.actual_bytes);
            }
#if defined(PIX_EMBEDDED_ASSETS)
            std::fprintf(stderr, ". The packaged PIX asset bundle is invalid.\n");
#else
            std::fprintf(stderr, ". Check the PIX asset files selected by --assets and retry.\n");
#endif
        }
        return fail(family_result.error());
    }
    std::unique_ptr<EnclosureFamily> family(std::move(family_result.value()));
#if defined(PIX_RUNTIME_TEST_CLOSE_BUDGET_MS)
    if (fake_dt295) {
        const auto opened = family->tuner_backend().open_receiver(0U);
        if (!opened) {
            std::fprintf(stderr, "PIX_TEST_DT295 open failed=%u\n",
                         static_cast<unsigned>(opened.error()));
            return 70;
        }
        const char* reinsert_value = std::getenv("PIX_TEST_DT295_REINSERT_RAW");
        if (reinsert_value != nullptr) {
            const unsigned raw_state = static_cast<unsigned>(
                std::strtoul(reinsert_value, nullptr, 10));
            dt295_actual_fixture_set_card_sample(raw_state, 1U, 0U);
        }
    }
#endif

    const auto resolved_selector = dtv::userland::identity::resolve_selector(
        dtv::userland::identity::Product::pix, parsed_selector.value());
    if (!resolved_selector) return fail(resolved_selector.error());
    const auto automatic_instance = dtv::userland::identity::auto_instance(
        resolved_selector.value());
    if (!automatic_instance) return fail(automatic_instance.error());
    std::string instance;
    if (arguments.instance.empty()) {
        instance.assign(automatic_instance.value().bytes.data(),
                        automatic_instance.value().length);
    } else {
        const auto canonical = dtv::userland::identity::canonicalize_instance(
            arguments.instance);
        if (!canonical) {
            std::fprintf(stderr,
                "pixd: invalid --instance; use 1..64 ASCII bytes, start with a letter or digit, then use letters, digits, '.', '_' or '-'; avoid a trailing dot and Windows reserved device names.\n");
            return 2;
        }
        instance.assign(canonical.value().bytes.data(), canonical.value().length);
    }

    std::unique_ptr<TunerNonceSource> nonce_source(new (std::nothrow)
#ifdef _WIN32
        px4::userland::ipc::windows::WindowsTunerNonceSource());
#else
        px4::userland::ipc::posix::PosixTunerNonceSource());
#endif
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
    // STATUS is visible only after publish() succeeds below.
    config.context.state = dtv::userland::wire::ContextState::ready;
    for (std::size_t i = 0U; i < sizeof(config.stream_engine_epoch); ++i)
        config.stream_engine_epoch = (config.stream_engine_epoch << 8U) |
            epoch.value()[i];
    if (config.stream_engine_epoch == 0U) config.stream_engine_epoch = 1U;
    config.stream_arm_budget_ms = 5000U;
    auto assembled = EnclosureContext::assemble(
        std::move(family), std::move(nonce_source), config);
    if (!assembled) return fail(assembled.error());
    std::unique_ptr<EnclosureContext> context(std::move(assembled.value()));
    ListenerDiagnostic listener_diagnostic{};
    const auto reserved = context->reserve_namespace(&listener_diagnostic);
    if (!reserved) {
        std::fprintf(stderr, "PIX_STAGE reserve error=%u\n", static_cast<unsigned>(reserved.error()));
        print_listener_error(listener_diagnostic, reserved.error());
        return fail(reserved.error());
    }
    if (!px4::userland::px4d::install_signal_handlers()) return 70;
#ifdef _WIN32
    px4::userland::cli::StdinEofMonitor eof_monitor;
    if (arguments.exit_on_stdin_eof &&
        !eof_monitor.start(::GetStdHandle(STD_INPUT_HANDLE),
            [](void*) noexcept { px4::userland::px4d::request_stop(); }, nullptr)) return 70;
#endif
    const auto published = context->publish();
    if (!published) {
        std::fprintf(stderr, "PIX_STAGE publish error=%u\n", static_cast<unsigned>(published.error()));
        return fail(published.error());
    }
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
#ifdef _WIN32
    eof_monitor.stop();
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

int main(int argc, char** argv)
{
#ifdef _WIN32
    std::vector<std::string> owned = px4::userland::cli::windows_argv_utf8(argc, argv);
    if (owned.empty()) return 2;
    std::vector<char*> views;
    views.reserve(owned.size());
    for (std::string& value : owned) views.push_back(value.data());
    const int result = run(static_cast<int>(views.size()), views.data());
    px4::userland::px4d::notify_cleanup_complete();
    return result;
#else
    return run(argc, argv);
#endif
}
