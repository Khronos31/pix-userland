// SPDX-License-Identifier: MIT
#include "pix_runtime.h"
#if defined(PIX_EMBEDDED_ASSETS)
#include "pix_embedded_assets.h"
#endif

#include "pix-dt295-receiver-adapter.h"
#include "pix-firmware-bundle.h"
#include "pix_firmware_manifest_generated.h"

#include "dtv/userland/identity.h"
#include "dtv/userland/wire.h"
#include "pixy.h"
#include "px4/posix_ipc.h"
#include "usb_backend.h"

#include <libusb.h>

#include <algorithm>
#include <chrono>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <new>
#include <string_view>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>
#ifdef _WIN32
#include "px4_windows_args.h"
#include <io.h>
#endif

namespace pix::runtime {
namespace dt181 = ::pix::capture::dt181;
namespace dt295 = ::pix::capture::dt295;
namespace {

using namespace px4::userland;
using namespace px4::userland::ipc::posix;
using dtv::userland::Error;

// common-api-contract.md fixes the per-stream TS queue at an 8 MiB budget.
// ReceiverAdapter sizes its queue in 188-byte packets, so floor the quotient.
constexpr std::size_t kTsQueueByteBudget = 8U * 1024U * 1024U;
constexpr std::size_t kTsQueuePackets =
    kTsQueueByteBudget / dtv::userland::wire::kTsPacketSize;

template <std::size_t N>
bool set_string(dtv::userland::wire::WString<N>& out, std::string_view value) noexcept
{
    if (value.size() > N) return false;
    out.length = static_cast<std::uint16_t>(value.size());
    std::memcpy(out.bytes.data(), value.data(), value.size());
    return true;
}

void set_asset_diagnostic(RuntimeFailureDiagnostic* diagnostic, const char* asset,
                          const char* detail, std::size_t expected = 0U,
                          std::size_t actual = 0U, bool counts_valid = false,
                          bool lower_bound = false) noexcept
{
    if (diagnostic == nullptr) return;
    std::snprintf(diagnostic->asset.data(), diagnostic->asset.size(), "%s", asset);
    std::snprintf(diagnostic->detail.data(), diagnostic->detail.size(), "%.*s",
                  static_cast<int>(diagnostic->detail.size() - 1U), detail);
    diagnostic->expected_bytes = expected;
    diagnostic->actual_bytes = actual;
    diagnostic->byte_counts_valid = counts_valid;
    diagnostic->actual_is_lower_bound = lower_bound;
}

Result<void> load_exact(const std::string& path, const char* asset,
                        std::uint8_t* output, std::size_t expected,
                        RuntimeFailureDiagnostic* diagnostic) noexcept
{
#ifdef _WIN32
    const auto wide = px4::userland::cli::utf8_to_wide_path(path);
    if (wide.empty()) return Result<void>::failure(Error::INVALID_ARGUMENT);
    const int fd = ::_wopen(wide.data(), _O_RDONLY | _O_BINARY | _O_NOINHERIT);
#else
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
#endif
    if (fd < 0) {
        const char* detail = errno == ENOENT || errno == ENOTDIR ? "not found" :
            errno == EACCES || errno == EPERM ? "permission denied" : "open failed";
        set_asset_diagnostic(diagnostic, asset, detail);
        return Result<void>::failure(Error::NOT_FOUND);
    }
#ifdef _WIN32
    struct _stat64 status{};
    const int stat_result = ::_fstat64(fd, &status);
    const bool regular_file = stat_result == 0 &&
        (status.st_mode & _S_IFMT) == _S_IFREG;
#else
    struct stat status{};
    const int stat_result = ::fstat(fd, &status);
    const bool regular_file = stat_result == 0 && S_ISREG(status.st_mode);
#endif
    if (regular_file && status.st_size >= 0 &&
        static_cast<std::uint64_t>(status.st_size) != expected) {
        const std::size_t actual = static_cast<std::size_t>(status.st_size);
        (void)::close(fd);
        set_asset_diagnostic(diagnostic, asset, "file size does not match", expected,
                             actual, true);
        return Result<void>::failure(Error::INVALID_ARGUMENT);
    }
    std::size_t offset = 0U;
    while (offset < expected) {
        const ssize_t count = ::read(fd, output + offset, expected - offset);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) {
            (void)::close(fd);
            set_asset_diagnostic(diagnostic, asset, "read ended before the expected size",
                                 expected, offset, true);
            return Result<void>::failure(Error::INVALID_ARGUMENT);
        }
        offset += static_cast<std::size_t>(count);
    }
    std::uint8_t extra = 0U;
    ssize_t trailing = 0;
    do { trailing = ::read(fd, &extra, 1U); } while (trailing < 0 && errno == EINTR);
    const int close_result = ::close(fd);
    if (trailing > 0) {
        set_asset_diagnostic(diagnostic, asset, "file contains extra data", expected,
                             expected + 1U, true, true);
        return Result<void>::failure(Error::INVALID_ARGUMENT);
    }
    if (trailing < 0 || close_result != 0) {
        set_asset_diagnostic(diagnostic, asset, "file read or close failed", expected,
                             expected, true);
        return Result<void>::failure(Error::INVALID_ARGUMENT);
    }
    return Result<void>::success();
}

class PixTime final : public TunerServiceTime,
                      public dtv::userland::detail::ReceiverClock {
public:
    std::uint64_t monotonic_ms() noexcept override
    {
        return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
    }
    void sleep_ms(std::uint32_t milliseconds) noexcept override
    { std::this_thread::sleep_for(std::chrono::milliseconds(milliseconds)); }
};

class EmptyCardBackend final : public CardServiceBackend {
public:
    Result<void> set_power(bool) noexcept override
    { return Result<void>::failure(Error::NO_CARD); }
    Result<void> initialize_uart() noexcept override
    { return Result<void>::failure(Error::NO_CARD); }
    Result<bool> detect_card() noexcept override { return Result<bool>::success(false); }
};

class Dt295CardBackend final : public CardServiceBackend {
public:
    Result<void> set_power(bool) noexcept override
    { return Result<void>::failure(Error::UNSUPPORTED); }
    Result<void> initialize_uart() noexcept override
    { return Result<void>::failure(Error::UNSUPPORTED); }
    Result<bool> detect_card() noexcept override
    { return Result<bool>::failure(Error::UNSUPPORTED); }
};

class EmptyCardSession final : public CardProtocolSession {
public:
    Result<void> initialize() noexcept override
    { return Result<void>::failure(Error::NO_CARD); }
    Result<std::size_t> transmit(ByteView, MutableByteView) noexcept override
    { return Result<std::size_t>::failure(Error::NO_CARD); }
    bool initialized() const noexcept override { return false; }
    const CardAtrBytes& atr() const noexcept override { return atr_; }
    void invalidate() noexcept override {}
private:
    CardAtrBytes atr_{};
};

template <typename Adapter>
class PixStreamShimT final : public px4::userland::TunerStreamControl {
public:
    explicit PixStreamShimT(Adapter& adapter) noexcept : adapter_(adapter) {}
    Result<void> attach(const TunerAttachment& value) noexcept override
    { return adapter_.attach(convert(value)); }
    Result<void> detach(const TunerAttachment& value) noexcept override
    { return adapter_.detach(convert(value)); }
    Result<TunerStreamCounters> stats(const TunerAttachment& value) const noexcept override
    {
        const auto result = adapter_.stats(convert(value));
        if (!result) return Result<TunerStreamCounters>::failure(result.error());
        return Result<TunerStreamCounters>::success(copy(result.value()));
    }
    Result<TunerStreamFinalSnapshot> final_snapshot(
        const TunerAttachment& value) const noexcept override
    {
        const auto result = adapter_.final_snapshot(convert(value));
        if (!result) return Result<TunerStreamFinalSnapshot>::failure(result.error());
        TunerStreamFinalSnapshot out{};
        out.counters = copy(result.value().counters);
        out.terminal = result.value().terminal;
        return Result<TunerStreamFinalSnapshot>::success(out);
    }
    Result<void> release_final(const TunerAttachment& value) noexcept override
    { return adapter_.release_final(convert(value)); }
    Result<TunerStreamReadResult> read(const TunerAttachment& value,
        MutableByteView output, Timeout timeout) noexcept override
    {
        const auto result = adapter_.read(convert(value), output,
            std::chrono::milliseconds(timeout.milliseconds));
        if (!result) return Result<TunerStreamReadResult>::failure(result.error());
        TunerStreamReadResult out{};
        out.bytes = result.value().bytes;
        out.eof = result.value().eof;
        out.timed_out = result.value().timed_out;
        out.terminal = static_cast<TunerStreamTerminal>(result.value().terminal);
        return Result<TunerStreamReadResult>::success(out);
    }
private:
    static typename Adapter::Attachment convert(const TunerAttachment& value) noexcept
    {
        typename Adapter::Attachment out{};
        out.owner_client_id = value.owner_client_id;
        out.lease_id = value.lease_id;
        out.attachment_id = value.attachment_id;
        out.receiver = value.receiver;
        out.system = value.system == ipc::System::ISDB_T
            ? dtv::userland::detail::ReceiverSystem::terrestrial
            : dtv::userland::detail::ReceiverSystem::satellite;
        out.evict_oldest = value.evict_oldest;
        out.token = value.nonce;
        return out;
    }
    static TunerStreamCounters copy(const dtv::userland::detail::StreamCounters& value) noexcept
    {
        return TunerStreamCounters{value.packets, value.bytes, value.sync_errors,
            value.tei_packets, value.continuity_errors, value.queue_drops,
            value.usb_errors, value.empty_intervals, value.availability};
    }
    Adapter& adapter_;
};

using PixStreamShim = PixStreamShimT<dt181::ReceiverAdapter>;

class PixTunerBackend final : public TunerServiceBackend {
public:
    PixTunerBackend(dt181::ReceiverAdapter& adapter, dt181_session& session) noexcept
        : adapter_(adapter), session_(session) {}
    std::uint8_t receiver_count() const noexcept override { return 1U; }
    bool supports_internal_card_observation() const noexcept override { return true; }
    dtv::userland::wire::Optional<bool> sample_internal_card_inserted() noexcept override
    {
        std::uint8_t status[64]{};
        std::size_t length = sizeof(status);
        if (dt181_session_status(&session_, status, &length) != 0 || length < 4U)
            return {};
        dtv::userland::wire::Optional<bool> observed{};
        observed.present = true;
        observed.value = status[3] != 0U;
        return observed;
    }
    bool receiver_supports(std::uint8_t receiver, ipc::System system) const noexcept override
    { return receiver == 0U && (system == ipc::System::ISDB_T || system == ipc::System::ISDB_S); }
    bool selects_satellite_stream_before_tune() const noexcept override { return true; }
    Result<void> open_receiver(std::uint8_t receiver) noexcept override
    { return adapter_.open(receiver); }
    Result<void> tune_terrestrial(std::uint8_t receiver, std::uint32_t khz,
                                  std::uint32_t timeout) noexcept override
    { return adapt(adapter_.program_hz(receiver, dt181::ReceiverAdapter::ReceiverSystem::terrestrial,
                                       static_cast<std::uint64_t>(khz) * 1000U, timeout)); }
    Result<void> tune_satellite(std::uint8_t receiver, std::uint32_t khz,
                                std::uint32_t timeout) noexcept override
    { return adapt(adapter_.program_hz(receiver, dt181::ReceiverAdapter::ReceiverSystem::satellite,
                                       static_cast<std::uint64_t>(khz) * 1000U, timeout)); }
    Result<TunerHzProgramming> tune_terrestrial_hz(
        std::uint8_t receiver, std::uint64_t hz, std::uint32_t timeout) noexcept override
    { return adapter_.program_hz(receiver, dt181::ReceiverAdapter::ReceiverSystem::terrestrial,
                                 hz, timeout); }
    Result<TunerHzProgramming> tune_satellite_hz(
        std::uint8_t receiver, std::uint64_t hz, std::uint32_t timeout) noexcept override
    { return adapter_.program_hz(receiver, dt181::ReceiverAdapter::ReceiverSystem::satellite,
                                 hz, timeout); }
    TunerStrictProfile strict_profile(std::uint8_t receiver,
                                      ipc::System system) const noexcept override
    {
        if (!receiver_supports(receiver, system)) return TunerStrictProfile{};
        TunerStrictProfile profile{};
        profile.native_hz = true;
        profile.readiness = dtv::userland::detail::TuneReadiness::terminal_lock;
        profile.min_hz = 1U;
        profile.max_hz = std::numeric_limits<std::uint32_t>::max();
        profile.slot_count = system == ipc::System::ISDB_S ? 8U : 0U;
        return profile;
    }
    Result<bool> is_locked(std::uint8_t receiver, ipc::System system) noexcept override
    {
        if (!receiver_supports(receiver, system)) return Result<bool>::failure(Error::UNSUPPORTED);
        return adapter_.is_locked(receiver, system == ipc::System::ISDB_T
            ? dt181::ReceiverAdapter::ReceiverSystem::terrestrial
            : dt181::ReceiverAdapter::ReceiverSystem::satellite);
    }
    Result<void> select_satellite_slot(std::uint8_t receiver, std::uint8_t slot,
                                       std::uint32_t timeout) noexcept override
    { return adapter_.select_satellite_slot(receiver, slot, timeout); }
    Result<void> select_satellite_tsid(std::uint8_t receiver, std::uint16_t tsid,
                                       std::uint32_t timeout) noexcept override
    { return adapter_.select_satellite_tsid(receiver, tsid, timeout); }
    Result<void> close_receiver(std::uint8_t receiver) noexcept override
    { return adapter_.close(receiver); }
    Result<void> begin_tune_power(std::uint8_t receiver, ipc::System system,
                                  std::uint8_t voltage) noexcept override
    { return adapter_.begin_tune_power(receiver, system == ipc::System::ISDB_T
        ? dt181::ReceiverAdapter::ReceiverSystem::terrestrial
        : dt181::ReceiverAdapter::ReceiverSystem::satellite, voltage); }
    Result<void> commit_tune_power(std::uint8_t receiver) noexcept override
    { return adapter_.commit_tune_power(receiver); }
    Result<void> rollback_tune_power(std::uint8_t receiver) noexcept override
    { return adapter_.rollback_tune_power(receiver); }
    void mark_receiver_disconnected(std::uint8_t receiver) noexcept override
    { adapter_.mark_disconnected(receiver); }
    Result<void> shutdown() noexcept override { return adapter_.shutdown(); }
    Result<void> start_capture(std::uint8_t receiver, ipc::System system) noexcept override
    { return adapter_.start_capture(receiver, system == ipc::System::ISDB_T
        ? dt181::ReceiverAdapter::ReceiverSystem::terrestrial
        : dt181::ReceiverAdapter::ReceiverSystem::satellite); }
    Result<void> stop_capture(std::uint8_t receiver, ipc::System system) noexcept override
    { return adapter_.stop_capture(receiver, system == ipc::System::ISDB_T
        ? dt181::ReceiverAdapter::ReceiverSystem::terrestrial
        : dt181::ReceiverAdapter::ReceiverSystem::satellite); }
private:
    static Result<void> adapt(Result<TunerHzProgramming> result) noexcept
    { return result ? Result<void>::success() : Result<void>::failure(result.error()); }
    dt181::ReceiverAdapter& adapter_;
    dt181_session& session_;
};

using Dt295StreamShim = PixStreamShimT<dt295::ReceiverAdapter>;

class Dt295TunerBackend final : public TunerServiceBackend {
public:
    explicit Dt295TunerBackend(dt295::ReceiverAdapter& adapter) noexcept : adapter_(adapter) {}
    std::uint8_t receiver_count() const noexcept override { return 1U; }
    bool supports_internal_card_observation() const noexcept override { return true; }
    dtv::userland::wire::Optional<bool> sample_internal_card_inserted() noexcept override
    {
        dtv::userland::wire::Optional<bool> observed{};
        const auto snapshot = adapter_.sample_card_inserted();
        if (!snapshot.has_value()) return observed;
        observed.present = true;
        observed.value = *snapshot;
        return observed;
    }
    bool receiver_supports(std::uint8_t receiver, ipc::System system) const noexcept override
    { return receiver == 0U && (system == ipc::System::ISDB_T || system == ipc::System::ISDB_S); }
    bool selects_satellite_stream_before_tune() const noexcept override { return false; }
    Result<void> open_receiver(std::uint8_t receiver) noexcept override
    { return adapter_.open(receiver); }
    Result<void> tune_terrestrial(std::uint8_t receiver, std::uint32_t khz,
                                  std::uint32_t timeout) noexcept override
    { return adapt(adapter_.program_hz(receiver, dt295::ReceiverAdapter::ReceiverSystem::terrestrial,
                                       static_cast<std::uint64_t>(khz) * 1000U, timeout)); }
    Result<void> tune_satellite(std::uint8_t receiver, std::uint32_t khz,
                                std::uint32_t timeout) noexcept override
    { return adapt(adapter_.program_hz(receiver, dt295::ReceiverAdapter::ReceiverSystem::satellite,
                                       static_cast<std::uint64_t>(khz) * 1000U, timeout)); }
    Result<TunerHzProgramming> tune_terrestrial_hz(
        std::uint8_t receiver, std::uint64_t hz, std::uint32_t timeout) noexcept override
    { return adapter_.program_hz(receiver, dt295::ReceiverAdapter::ReceiverSystem::terrestrial,
                                 hz, timeout); }
    Result<TunerHzProgramming> tune_satellite_hz(
        std::uint8_t receiver, std::uint64_t hz, std::uint32_t timeout) noexcept override
    { return adapter_.program_hz(receiver, dt295::ReceiverAdapter::ReceiverSystem::satellite,
                                 hz, timeout); }
    TunerStrictProfile strict_profile(std::uint8_t receiver,
                                      ipc::System system) const noexcept override
    {
        if (!receiver_supports(receiver, system)) return TunerStrictProfile{};
        TunerStrictProfile profile{};
        profile.native_hz = true;
        profile.readiness = dtv::userland::detail::TuneReadiness::commands;
        profile.min_hz = system == ipc::System::ISDB_S ? 950000000U : 473142857U;
        profile.max_hz = system == ipc::System::ISDB_S ? 2150000000U : 767142857U;
        profile.slot_count = system == ipc::System::ISDB_S ? 8U : 0U;
        return profile;
    }
    Result<bool> is_locked(std::uint8_t receiver, ipc::System system) noexcept override
    {
        if (!receiver_supports(receiver, system)) return Result<bool>::failure(Error::UNSUPPORTED);
        return adapter_.is_locked(receiver, system == ipc::System::ISDB_T
            ? dt295::ReceiverAdapter::ReceiverSystem::terrestrial
            : dt295::ReceiverAdapter::ReceiverSystem::satellite);
    }
    Result<void> select_satellite_slot(std::uint8_t receiver, std::uint8_t slot,
                                       std::uint32_t timeout) noexcept override
    { return adapter_.select_satellite_slot(receiver, slot, timeout); }
    Result<void> select_satellite_tsid(std::uint8_t receiver, std::uint16_t tsid,
                                       std::uint32_t timeout) noexcept override
    { return adapter_.select_satellite_tsid(receiver, tsid, timeout); }
    Result<void> close_receiver(std::uint8_t receiver) noexcept override
    { return adapter_.close(receiver); }
    Result<void> begin_tune_power(std::uint8_t receiver, ipc::System system,
                                  std::uint8_t voltage) noexcept override
    { return adapter_.begin_tune_power(receiver, system == ipc::System::ISDB_T
        ? dt295::ReceiverAdapter::ReceiverSystem::terrestrial
        : dt295::ReceiverAdapter::ReceiverSystem::satellite, voltage); }
    Result<void> commit_tune_power(std::uint8_t receiver) noexcept override
    { return adapter_.commit_tune_power(receiver); }
    Result<void> rollback_tune_power(std::uint8_t receiver) noexcept override
    { return adapter_.rollback_tune_power(receiver); }
    void mark_receiver_disconnected(std::uint8_t receiver) noexcept override
    { adapter_.mark_disconnected(receiver); }
    Result<void> shutdown() noexcept override { return adapter_.shutdown(); }
    Result<void> start_capture(std::uint8_t receiver, ipc::System system) noexcept override
    { return adapter_.start_capture(receiver, system == ipc::System::ISDB_T
        ? dt295::ReceiverAdapter::ReceiverSystem::terrestrial
        : dt295::ReceiverAdapter::ReceiverSystem::satellite); }
    Result<void> stop_capture(std::uint8_t receiver, ipc::System system) noexcept override
    { return adapter_.stop_capture(receiver, system == ipc::System::ISDB_T
        ? dt295::ReceiverAdapter::ReceiverSystem::terrestrial
        : dt295::ReceiverAdapter::ReceiverSystem::satellite); }
private:
    static Result<void> adapt(Result<TunerHzProgramming> result) noexcept
    { return result ? Result<void>::success() : Result<void>::failure(result.error()); }
    dt295::ReceiverAdapter& adapter_;
};

}  // namespace

struct Family::Impl final {
    Model model = Model::dt181;
    dt181_session session{};
    std::unique_ptr<Assets> assets;
    PixTime clock;
    dt181::Consumer consumer;
    std::unique_ptr<dt181::ReceiverAdapter> adapter;
    std::unique_ptr<PixTunerBackend> tuner;
    EmptyCardBackend card;
    Dt295CardBackend dt295_card;
    EmptyCardSession protocol;
    std::unique_ptr<PixStreamShim> shim;
    std::unique_ptr<dt295::ReceiverAdapter> dt295_adapter;
    std::unique_ptr<Dt295TunerBackend> dt295_tuner;
    std::unique_ptr<Dt295StreamShim> dt295_shim;
    bool initialized = false;
};

namespace {

bool same_serial(const dtv::userland::identity::SerialText& left,
                 const dtv::userland::identity::SerialText& right) noexcept
{
    return left.length == right.length &&
           std::memcmp(left.bytes.data(), right.bytes.data(), left.length) == 0;
}

bool read_serial_text(libusb_device* device,
                      const libusb_device_descriptor& descriptor,
                      dtv::userland::identity::SerialText& output) noexcept
{
    if (descriptor.iSerialNumber == 0U) return false;
    libusb_device_handle* handle = nullptr;
    if (libusb_open(device, &handle) != LIBUSB_SUCCESS || handle == nullptr)
        return false;
    unsigned char characters[256]{};
    const int length = libusb_get_string_descriptor_ascii(
        handle, descriptor.iSerialNumber, characters,
        static_cast<int>(sizeof(characters)));
    libusb_close(handle);
    if (length <= 0 || static_cast<std::size_t>(length) > output.bytes.size())
        return false;
    output.length = static_cast<std::uint16_t>(length);
    std::memcpy(output.bytes.data(), characters, static_cast<std::size_t>(length));
    return true;
}

bool fill_location(libusb_device* device, unsigned bus, Model model,
                   LocatedDevice& output) noexcept
{
    std::uint8_t ports[USB_SELECTOR_MAX_PORTS]{};
    const int port_count = libusb_get_port_numbers(device, ports, sizeof(ports));
    if (port_count <= 0) return false;
    char path[64]{};
    if (usb_selector_format_path(path, sizeof(path), bus, ports,
                                 static_cast<unsigned>(port_count)) != 0)
        return false;
    output.model = model;
    output.bus = static_cast<std::uint8_t>(bus);
    output.address = libusb_get_device_address(device);
    output.port_count = static_cast<std::uint8_t>(port_count);
    std::copy(ports, ports + port_count, output.ports.begin());
    output.path.assign(path);
    output.selector.kind = USB_SELECT_PATH;
    output.selector.bus = output.bus;
    output.selector.address = output.address;
    output.selector.port_count = static_cast<unsigned>(output.port_count);
    std::copy(ports, ports + port_count, output.selector.ports);
    return true;
}

}  // namespace

dtv::userland::Result<LocatedDevice> locate(const std::string& input) noexcept
{
    const bool serial_selector = input.rfind("serial:", 0U) == 0U;
    if (!serial_selector && input.rfind("usb:", 0U) != 0U)
        return dtv::userland::Result<LocatedDevice>::failure(Error::INVALID_ARGUMENT);

    LocatedDevice selected{};
    dtv::userland::identity::SerialText wanted{};
    if (serial_selector) {
        const auto parsed = dtv::userland::identity::parse_device_selector(input);
        if (!parsed ||
            parsed.value().kind != dtv::userland::identity::SelectorKind::serial)
            return dtv::userland::Result<LocatedDevice>::failure(
                Error::INVALID_ARGUMENT);
        wanted = parsed.value().serial;
    } else if (usb_selector_parse(input.c_str() + 4, &selected.selector) != 0 ||
               selected.selector.kind != USB_SELECT_PATH) {
        return dtv::userland::Result<LocatedDevice>::failure(Error::INVALID_ARGUMENT);
    }

    libusb_context* context = nullptr;
    if (libusb_init(&context) != LIBUSB_SUCCESS)
        return dtv::userland::Result<LocatedDevice>::failure(Error::USB_IO);
    libusb_device** devices = nullptr;
    const ssize_t count = libusb_get_device_list(context, &devices);
    if (count < 0) {
        libusb_exit(context);
        return dtv::userland::Result<LocatedDevice>::failure(Error::USB_IO);
    }
    bool found = false;
    bool ambiguous = false;
    ssize_t selected_index = -1;
    Error failure = Error::NOT_FOUND;
    for (ssize_t index = 0; index < count; ++index) {
        libusb_device_descriptor descriptor{};
        if (libusb_get_device_descriptor(devices[index], &descriptor) != LIBUSB_SUCCESS)
            continue;
        if (descriptor.idVendor != 0x06b8U) continue;
        Model model;
        if (descriptor.idProduct == 0x102bU) model = Model::dt181;
        else if (descriptor.idProduct == 0x103bU) model = Model::dt295;
        else continue;

        const unsigned bus = libusb_get_bus_number(devices[index]);
        if (serial_selector) {
            dtv::userland::identity::SerialText observed{};
            if (!read_serial_text(devices[index], descriptor, observed) ||
                !same_serial(observed, wanted)) continue;
            if (found) {
                ambiguous = true;
                break;
            }
            if (!fill_location(devices[index], bus, model, selected)) {
                failure = Error::INTERNAL;
                break;
            }
            selected.serial = observed;
            selected.serial_present = true;
            selected.serial_unique = true;
            selected.serial_uniqueness_known = true;
            found = true;
            continue;
        }

        if (found) continue;

        LocatedDevice candidate{};
        std::uint8_t ports[USB_SELECTOR_MAX_PORTS]{};
        const int port_count = libusb_get_port_numbers(devices[index], ports,
                                                       sizeof(ports));
        if (port_count <= 0 || !usb_selector_matches(&selected.selector, 0U, bus,
                libusb_get_device_address(devices[index]), ports,
                static_cast<unsigned>(port_count))) continue;
        if (!fill_location(devices[index], bus, model, candidate)) {
            failure = Error::INTERNAL;
            break;
        }
        dtv::userland::identity::SerialText observed{};
        if (read_serial_text(devices[index], descriptor, observed)) {
            candidate.serial = observed;
            candidate.serial_present = true;
            candidate.serial_unique = true;
        } else if (descriptor.iSerialNumber == 0U) {
            candidate.serial_absent = true;
        }
        selected = std::move(candidate);
        selected_index = index;
        found = true;
        continue;
    }
    if (!serial_selector && found && selected.serial_present) {
        selected.serial_uniqueness_known = true;
        for (ssize_t index = 0; index < count; ++index) {
            if (index == selected_index) continue;
            libusb_device_descriptor descriptor{};
            if (libusb_get_device_descriptor(devices[index], &descriptor) !=
                LIBUSB_SUCCESS || descriptor.idVendor != 0x06b8U)
                continue;
            dtv::userland::identity::SerialText observed{};
            if (read_serial_text(devices[index], descriptor, observed) &&
                same_serial(observed, selected.serial)) {
                selected.serial_unique = false;
                break;
            }
        }
    }
    libusb_free_device_list(devices, 1);
    libusb_exit(context);
    if (ambiguous)
        return dtv::userland::Result<LocatedDevice>::failure(Error::INVALID_ARGUMENT);
    if (!found) return dtv::userland::Result<LocatedDevice>::failure(failure);
    return dtv::userland::Result<LocatedDevice>::success(std::move(selected));
}

dtv::userland::Result<LocatedDevice> locate_fd(
    int fd, const std::string* input) noexcept
{
#ifdef _WIN32
    (void)fd;
    (void)input;
    return dtv::userland::Result<LocatedDevice>::failure(Error::UNSUPPORTED);
#else
    if (fd < 3 || ::fcntl(fd, F_GETFD) < 0)
        return dtv::userland::Result<LocatedDevice>::failure(Error::INVALID_ARGUMENT);

    dtv::userland::identity::Selector wanted{};
    usb_selector wanted_usb{};
    bool has_selector = input != nullptr;
    bool serial_selector = false;
    if (has_selector) {
        const auto parsed = dtv::userland::identity::parse_device_selector(*input);
        if (!parsed)
            return dtv::userland::Result<LocatedDevice>::failure(Error::INVALID_ARGUMENT);
        wanted = parsed.value();
        serial_selector = wanted.kind ==
            dtv::userland::identity::SelectorKind::serial;
        if (!serial_selector &&
            (wanted.kind != dtv::userland::identity::SelectorKind::usb_topology ||
             wanted.topology.count != 1U ||
             usb_selector_parse(input->c_str() + 4U, &wanted_usb) != 0 ||
             wanted_usb.kind != USB_SELECT_PATH))
            return dtv::userland::Result<LocatedDevice>::failure(Error::INVALID_ARGUMENT);
    }

    const int owned_fd = ::dup(fd);
    if (owned_fd < 0 || ::fcntl(owned_fd, F_SETFD, FD_CLOEXEC) < 0) {
        if (owned_fd >= 0) (void)::close(owned_fd);
        return dtv::userland::Result<LocatedDevice>::failure(Error::USB_IO);
    }
    (void)libusb_set_option(nullptr, LIBUSB_OPTION_NO_DEVICE_DISCOVERY);
    libusb_context* context = nullptr;
    const int initialized = libusb_init(&context);
    if (initialized != LIBUSB_SUCCESS) {
        (void)::close(owned_fd);
        return dtv::userland::Result<LocatedDevice>::failure(Error::USB_IO);
    }
    libusb_device_handle* handle = nullptr;
    const int wrapped = libusb_wrap_sys_device(context,
        static_cast<intptr_t>(owned_fd), &handle);
    if (wrapped != LIBUSB_SUCCESS || handle == nullptr) {
        libusb_exit(context);
        (void)::close(owned_fd);
        return dtv::userland::Result<LocatedDevice>::failure(
            wrapped == LIBUSB_ERROR_INVALID_PARAM || wrapped == LIBUSB_ERROR_NO_DEVICE
                ? Error::INVALID_ARGUMENT : Error::USB_IO);
    }

    LocatedDevice selected{};
    selected.fd = fd;
    auto* device = libusb_get_device(handle);
    libusb_device_descriptor descriptor{};
    Error failure = Error::NOT_FOUND;
    if (device == nullptr ||
        libusb_get_device_descriptor(device, &descriptor) != LIBUSB_SUCCESS) {
        failure = Error::INVALID_ARGUMENT;
    }
    bool matches = failure != Error::INVALID_ARGUMENT &&
        descriptor.idVendor == 0x06b8U &&
        (descriptor.idProduct == 0x102bU || descriptor.idProduct == 0x103bU);
    if (matches) {
        selected.model = descriptor.idProduct == 0x102bU ? Model::dt181 : Model::dt295;
        const unsigned bus = libusb_get_bus_number(device);
        std::uint8_t ports[USB_SELECTOR_MAX_PORTS]{};
        const int port_count = libusb_get_port_numbers(device, ports, sizeof(ports));
        selected.bus = static_cast<std::uint8_t>(bus);
        selected.address = libusb_get_device_address(device);
        if (port_count > 0 && static_cast<std::size_t>(port_count) <= selected.ports.size()) {
            selected.port_count = static_cast<std::uint8_t>(port_count);
            std::copy(ports, ports + port_count, selected.ports.begin());
            char path[64]{};
            if (usb_selector_format_path(path, sizeof(path), bus, ports,
                                         static_cast<unsigned>(port_count)) == 0)
                selected.path.assign(path);
        }
        if (descriptor.iSerialNumber != 0U) {
            unsigned char serial[256]{};
            const int length = libusb_get_string_descriptor_ascii(
                handle, descriptor.iSerialNumber, serial, sizeof(serial));
            if (length > 0 && static_cast<std::size_t>(length) <= selected.serial.bytes.size()) {
                selected.serial.length = static_cast<std::uint16_t>(length);
                std::memcpy(selected.serial.bytes.data(), serial,
                            static_cast<std::size_t>(length));
                selected.serial_present = true;
                selected.serial_unique = false;
                selected.serial_uniqueness_known = false;
            }
        } else {
            selected.serial_absent = true;
        }
        if (has_selector && serial_selector) {
            matches = selected.serial_present && same_serial(selected.serial, wanted.serial);
        } else if (has_selector) {
            matches = selected.bus == wanted_usb.bus && selected.port_count != 0U &&
                usb_selector_matches(&wanted_usb, 0U, selected.bus, selected.address,
                                     selected.ports.data(), selected.port_count);
        }
        if (!matches) failure = Error::NOT_FOUND;
        selected.selector.kind = USB_SELECT_INDEX;
    } else if (failure != Error::INVALID_ARGUMENT) {
        failure = Error::NOT_FOUND;
    }

    libusb_close(handle);
    libusb_exit(context);
    (void)::close(owned_fd);
    if (!matches)
        return dtv::userland::Result<LocatedDevice>::failure(failure);
    return dtv::userland::Result<LocatedDevice>::success(std::move(selected));
#endif
}

dtv::userland::Result<LocatedDevice> locate_dt181(const std::string& input) noexcept
{
    const auto located = locate(input);
    if (!located)
        return dtv::userland::Result<LocatedDevice>::failure(located.error());
    if (located.value().model != Model::dt181)
        return dtv::userland::Result<LocatedDevice>::failure(Error::UNSUPPORTED);
    return located;
}

Assets::~Assets() noexcept
{
    if (firmware != nullptr) {
        secure_clear(firmware, 699673U);
        std::free(firmware);
    }
    secure_clear(bootstrap.data(), bootstrap.size());
    secure_clear(validation.data(), validation.size());
}

dtv::userland::Result<std::unique_ptr<Assets>> load_assets_bytes(
    const std::uint8_t* firmware, std::size_t firmware_size,
    const std::uint8_t* bootstrap, std::size_t bootstrap_size,
    const std::uint8_t* validation, std::size_t validation_size,
    RuntimeFailureDiagnostic* diagnostic) noexcept
{
    if (diagnostic != nullptr) *diagnostic = RuntimeFailureDiagnostic{};
    if (firmware == nullptr || firmware_size != 699673U) {
        set_asset_diagnostic(diagnostic, "fpga.bin",
                             firmware == nullptr ? "embedded asset is missing" :
                                 "embedded asset size does not match",
                             699673U, firmware_size, firmware != nullptr);
        return dtv::userland::Result<std::unique_ptr<Assets>>::failure(Error::INVALID_ARGUMENT);
    }
    if (bootstrap == nullptr || bootstrap_size != 16U) {
        set_asset_diagnostic(diagnostic, "bootstrap.bin",
                             bootstrap == nullptr ? "embedded asset is missing" :
                                 "embedded asset size does not match", 16U,
                             bootstrap_size, bootstrap != nullptr);
        return dtv::userland::Result<std::unique_ptr<Assets>>::failure(Error::INVALID_ARGUMENT);
    }
    if (validation == nullptr || validation_size != 64U) {
        set_asset_diagnostic(diagnostic, "validation.bin",
                             validation == nullptr ? "embedded asset is missing" :
                                 "embedded asset size does not match", 64U,
                             validation_size, validation != nullptr);
        return dtv::userland::Result<std::unique_ptr<Assets>>::failure(Error::INVALID_ARGUMENT);
    }
    std::unique_ptr<Assets> assets(new (std::nothrow) Assets());
    if (!assets) return dtv::userland::Result<std::unique_ptr<Assets>>::failure(Error::INTERNAL);
    assets->firmware = static_cast<std::uint8_t*>(std::malloc(firmware_size));
    if (assets->firmware == nullptr)
        return dtv::userland::Result<std::unique_ptr<Assets>>::failure(Error::INTERNAL);
    std::memcpy(assets->firmware, firmware, firmware_size);
    std::memcpy(assets->bootstrap.data(), bootstrap, bootstrap_size);
    std::memcpy(assets->validation.data(), validation, validation_size);
    return dtv::userland::Result<std::unique_ptr<Assets>>::success(std::move(assets));
}

namespace {

struct FirmwareBuffer final {
    std::vector<std::uint8_t> bytes;
    ~FirmwareBuffer() noexcept
    {
        if (!bytes.empty()) secure_clear(bytes.data(), bytes.size());
    }
};

bool verify_bundle_bytes(Model model, const std::uint8_t* bytes, std::size_t size,
                         FirmwareBundleView& view,
                         RuntimeFailureDiagnostic* diagnostic) noexcept
{
    char error[192]{};
    const FirmwareModel firmware_model = model == Model::dt295
        ? FirmwareModel::dt295 : FirmwareModel::dt181;
    if (verify_firmware_bundle(firmware_model, bytes, size, view,
                               error, sizeof(error))) return true;
    set_asset_diagnostic(diagnostic, "firmware", error);
    return false;
}

bool read_firmware_file(const char* path, Model model,
                        std::vector<std::uint8_t>& bytes,
                        FirmwareBundleView& view,
                        RuntimeFailureDiagnostic* diagnostic) noexcept
{
    if (path == nullptr || path[0] == '\0') {
        set_asset_diagnostic(diagnostic, "firmware", "--firmware FILE is required");
        return false;
    }
    const std::size_t expected = model == Model::dt295
        ? PIX_DT295_BUNDLE_SIZE : PIX_DT181_BUNDLE_SIZE;
    bytes.resize(expected);
    if (!load_exact(path, "firmware", bytes.data(), bytes.size(), diagnostic)) return false;
    return verify_bundle_bytes(model, bytes.data(), bytes.size(), view, diagnostic);
}

}  // namespace

dtv::userland::Result<std::unique_ptr<Family>> Family::create(
    const LocatedDevice& device, const char* firmware_path,
    RuntimeFailureDiagnostic* diagnostic) noexcept
{
    if (diagnostic != nullptr) *diagnostic = RuntimeFailureDiagnostic{};
#if defined(PIX_EMBEDDED_ASSETS)
    const char* asset_manifest = pix_embedded_assets_manifest();
    if (asset_manifest == nullptr || asset_manifest[0] == '\0') {
        set_asset_diagnostic(diagnostic, "embedded assets",
                             "asset manifest is missing or empty");
        return dtv::userland::Result<std::unique_ptr<Family>>::failure(Error::INTERNAL);
    }
#endif
    std::unique_ptr<Family> family(new (std::nothrow) Family());
    if (!family) return dtv::userland::Result<std::unique_ptr<Family>>::failure(Error::INTERNAL);
    family->impl_.reset(new (std::nothrow) Impl());
    if (!family->impl_)
        return dtv::userland::Result<std::unique_ptr<Family>>::failure(Error::INTERNAL);
    family->impl_->model = device.model;
    if (device.model == Model::dt295) {
        FirmwareBuffer external_bundle;
        FirmwareBundleView bundle_view{};
        const std::uint8_t* bundle_data = nullptr;
        std::size_t bundle_size = 0U;
        if (firmware_path != nullptr && firmware_path[0] != '\0') {
            if (!read_firmware_file(firmware_path, Model::dt295, external_bundle.bytes,
                                    bundle_view, diagnostic))
                return dtv::userland::Result<std::unique_ptr<Family>>::failure(
                    Error::INVALID_ARGUMENT);
            bundle_data = external_bundle.bytes.data();
            bundle_size = external_bundle.bytes.size();
        }
#if defined(PIX_EMBEDDED_ASSETS)
        if (bundle_data == nullptr) {
            bundle_data = pix_embedded_dt295_bundle(&bundle_size);
            if (!verify_bundle_bytes(Model::dt295, bundle_data, bundle_size,
                                     bundle_view, diagnostic))
                return dtv::userland::Result<std::unique_ptr<Family>>::failure(
                    Error::INVALID_ARGUMENT);
        }
#else
        if (bundle_data == nullptr) {
            set_asset_diagnostic(diagnostic, "firmware", "--firmware FILE is required");
            return dtv::userland::Result<std::unique_ptr<Family>>::failure(
                Error::INVALID_ARGUMENT);
        }
#endif
        std::unique_ptr<dt295::ReceiverAdapter> adapter(
            new (std::nothrow) dt295::ReceiverAdapter(
                bundle_data, bundle_size, device.selector, family->impl_->clock, 0U,
                kTsQueuePackets, device.fd));
        if (!adapter)
            return dtv::userland::Result<std::unique_ptr<Family>>::failure(Error::INTERNAL);
        if (!adapter->ready())
        {
            if (diagnostic != nullptr && adapter->init_detail()[0] != '\0')
                set_asset_diagnostic(diagnostic, "firmware",
                                     adapter->init_detail());
            return dtv::userland::Result<std::unique_ptr<Family>>::failure(adapter->init_error());
        }
        family->impl_->dt295_adapter = std::move(adapter);
        family->impl_->dt295_tuner.reset(new (std::nothrow) Dt295TunerBackend(
            *family->impl_->dt295_adapter));
        if (!family->impl_->dt295_tuner)
            return dtv::userland::Result<std::unique_ptr<Family>>::failure(Error::INTERNAL);
        family->impl_->dt295_shim.reset(new (std::nothrow) Dt295StreamShim(
            *family->impl_->dt295_adapter));
        if (!family->impl_->dt295_shim)
            return dtv::userland::Result<std::unique_ptr<Family>>::failure(Error::INTERNAL);
        return dtv::userland::Result<std::unique_ptr<Family>>::success(std::move(family));
    }
    FirmwareBuffer external_bundle;
    FirmwareBundleView bundle_view{};
    const std::uint8_t* bundle_data = nullptr;
    if (firmware_path != nullptr && firmware_path[0] != '\0') {
        if (!read_firmware_file(firmware_path, Model::dt181, external_bundle.bytes,
                                bundle_view, diagnostic))
            return dtv::userland::Result<std::unique_ptr<Family>>::failure(
                Error::INVALID_ARGUMENT);
        bundle_data = external_bundle.bytes.data();
    }
#if defined(PIX_EMBEDDED_ASSETS)
    if (bundle_data == nullptr) {
        std::size_t embedded_bundle_size = 0U;
        bundle_data = pix_embedded_dt181_bundle(&embedded_bundle_size);
        if (!verify_bundle_bytes(Model::dt181, bundle_data, embedded_bundle_size,
                                 bundle_view, diagnostic))
            return dtv::userland::Result<std::unique_ptr<Family>>::failure(
                Error::INVALID_ARGUMENT);
    }
#else
    if (bundle_data == nullptr) {
        set_asset_diagnostic(diagnostic, "firmware", "--firmware FILE is required");
        return dtv::userland::Result<std::unique_ptr<Family>>::failure(
            Error::INVALID_ARGUMENT);
    }
#endif
    auto assets = load_assets_bytes(bundle_view.fpga, bundle_view.fpga_size,
                                    bundle_view.bootstrap, bundle_view.bootstrap_size,
                                    bundle_view.validation, bundle_view.validation_size,
                                    diagnostic);
    if (!assets) return dtv::userland::Result<std::unique_ptr<Family>>::failure(assets.error());
    family->impl_->assets = std::move(assets.value());
    const int opened = dt181_session_open(&family->impl_->session, &device.selector,
                                          device.fd, 0);
    if (opened != 0)
        return dtv::userland::Result<std::unique_ptr<Family>>::failure(
            opened == 3 ? Error::NOT_FOUND : opened == 4 ? Error::BUSY : Error::USB_IO);
    if (dt181_session_initialize(&family->impl_->session, family->impl_->assets->firmware,
            family->impl_->assets->bootstrap.data(), family->impl_->assets->validation.data()) != 0)
        return dtv::userland::Result<std::unique_ptr<Family>>::failure(Error::USB_IO);
    family->impl_->initialized = true;
    family->impl_->adapter.reset(new (std::nothrow) dt181::ReceiverAdapter(
        family->impl_->session, family->impl_->consumer, family->impl_->clock, 0U,
        kTsQueuePackets));
    if (!family->impl_->adapter)
        return dtv::userland::Result<std::unique_ptr<Family>>::failure(Error::INTERNAL);
    family->impl_->tuner.reset(new (std::nothrow) PixTunerBackend(
        *family->impl_->adapter, family->impl_->session));
    if (!family->impl_->tuner)
        return dtv::userland::Result<std::unique_ptr<Family>>::failure(Error::INTERNAL);
    family->impl_->shim.reset(new (std::nothrow) PixStreamShim(*family->impl_->adapter));
    if (!family->impl_->shim)
        return dtv::userland::Result<std::unique_ptr<Family>>::failure(Error::INTERNAL);
    return dtv::userland::Result<std::unique_ptr<Family>>::success(std::move(family));
}

Family::~Family() noexcept
{
    (void)shutdown();
    if (impl_) {
        if (impl_->model == Model::dt295) {
            impl_->dt295_shim.reset();
            impl_->dt295_tuner.reset();
            impl_->dt295_adapter.reset();
            return;
        }
        impl_->shim.reset();
        impl_->tuner.reset();
        impl_->adapter.reset();
        if (impl_->initialized) {
            (void)dt181_session_shutdown(&impl_->session);
            impl_->initialized = false;
        }
        if (impl_->session.opened) dt181_session_close(&impl_->session);
        impl_->assets.reset();
    }
}

Result<void> Family::shutdown() noexcept
{
    if (!impl_) return Result<void>::success();
    if (impl_->model == Model::dt295)
        return impl_->dt295_adapter ? impl_->dt295_adapter->shutdown()
                                    : Result<void>::success();
    if (impl_->adapter) {
        const auto stopped = impl_->adapter->shutdown();
        if (!stopped) return stopped;
    }
    if (impl_->initialized) {
        if (dt181_session_shutdown(&impl_->session) != 0)
            return Result<void>::failure(Error::USB_IO);
        impl_->initialized = false;
    }
    if (impl_->session.opened) dt181_session_close(&impl_->session);
    impl_->assets.reset();
    return Result<void>::success();
}

TunerServiceBackend& Family::tuner_backend() noexcept
{
    return impl_->model == Model::dt295
        ? static_cast<TunerServiceBackend&>(*impl_->dt295_tuner)
        : static_cast<TunerServiceBackend&>(*impl_->tuner);
}
CardServiceBackend& Family::card_backend() noexcept
{
    return impl_->model == Model::dt295
        ? static_cast<CardServiceBackend&>(impl_->dt295_card)
        : static_cast<CardServiceBackend&>(impl_->card);
}
CardProtocolSession& Family::card_protocol() noexcept { return impl_->protocol; }
TunerServiceTime& Family::time() noexcept { return impl_->clock; }
TunerStreamControl* Family::stream_control() noexcept
{
    return impl_->model == Model::dt295
        ? static_cast<TunerStreamControl*>(impl_->dt295_shim.get())
        : static_cast<TunerStreamControl*>(impl_->shim.get());
}

bool Family::supports_dt295_scan() const noexcept
{
    return impl_ && impl_->model == Model::dt295 && impl_->dt295_adapter;
}

dtv::userland::Result<Dt295Acquisition> Family::dt295_acquisition() const noexcept
{
    if (!supports_dt295_scan())
        return dtv::userland::Result<Dt295Acquisition>::failure(Error::UNSUPPORTED);
    const auto value = impl_->dt295_adapter->acquisition();
    Dt295Acquisition out{};
    out.phase = static_cast<Dt295AcquisitionPhase>(value.phase);
    out.failure = static_cast<Dt295AcquisitionFailure>(value.failure);
    out.tune_generation = value.tune_generation;
    out.acquisition_generation = value.acquisition_generation;
    out.plan_generation = value.plan_generation;
    return dtv::userland::Result<Dt295Acquisition>::success(out);
}

dtv::userland::Result<Dt295ScanLease> Family::begin_dt295_si_scan() noexcept
{
    if (!supports_dt295_scan())
        return dtv::userland::Result<Dt295ScanLease>::failure(Error::UNSUPPORTED);
    dt295::ReceiverAdapter::ScanLease value{};
    const auto started = impl_->dt295_adapter->begin_scan(&value);
    if (!started) return dtv::userland::Result<Dt295ScanLease>::failure(started.error());
    Dt295ScanLease out{};
    out.tune_generation = value.tune_generation;
    out.scan_id = value.scan_id;
    return dtv::userland::Result<Dt295ScanLease>::success(out);
}

dtv::userland::Result<Dt295SiRead> Family::read_dt295_si_scan(
    const Dt295ScanLease& lease, dtv::userland::MutableByteView output,
    std::chrono::milliseconds timeout) noexcept
{
    if (!supports_dt295_scan())
        return dtv::userland::Result<Dt295SiRead>::failure(Error::UNSUPPORTED);
    dt295::ReceiverAdapter::ScanLease value{};
    value.tune_generation = lease.tune_generation;
    value.scan_id = lease.scan_id;
    dt295::ReceiverAdapter::SiReadResult read{};
    const auto result = impl_->dt295_adapter->read_scan(value, output, timeout, &read);
    if (!result) return dtv::userland::Result<Dt295SiRead>::failure(result.error());
    Dt295SiRead out{};
    out.bytes = read.bytes;
    out.timed_out = read.timed_out;
    out.terminal = read.terminal;
    out.stale_generation = read.stale_generation;
    out.dropped_packets = read.dropped_packets;
    out.continuity_epoch = read.continuity_epoch;
    out.reason = static_cast<Dt295SiEndReason>(read.reason);
    return dtv::userland::Result<Dt295SiRead>::success(out);
}

dtv::userland::Result<void> Family::end_dt295_si_scan(
    const Dt295ScanLease& lease) noexcept
{
    if (!supports_dt295_scan())
        return dtv::userland::Result<void>::failure(Error::UNSUPPORTED);
    dt295::ReceiverAdapter::ScanLease value{};
    value.tune_generation = lease.tune_generation;
    value.scan_id = lease.scan_id;
    return impl_->dt295_adapter->end_scan(value);
}

Result<void> build_list(const LocatedDevice& device, const std::string& version,
                        const std::array<std::uint8_t, 16U>& epoch_bytes,
                        dtv::userland::wire::ListResponse& list) noexcept
{
    using namespace dtv::userland::wire;
    list = ListResponse{};
    list.epoch = epoch_bytes;
    const bool dt295 = device.model == Model::dt295;
    const char* model_id = dt295 ? "PIX-DT295" : "PIX-DT181-PU0";
    if (!set_string(list.producer.product, "pix-userland") ||
        !set_string(list.producer.version, version) ||
        !set_string(list.producer.core_version.value, DTV_USERLAND_CORE_VERSION))
        return Result<void>::failure(Error::INTERNAL);
    list.producer.core_version.present = true;
    list.topology.state = TopologyState::complete;
    list.topology.expected_function_count.present = true;
    list.topology.expected_function_count.value = 1U;
    list.enclosure.present = true;
    list.enclosure.value.id = 0U;
    list.enclosure.value.model.present = true;
    if (!set_string(list.enclosure.value.model.value.name, model_id))
        return Result<void>::failure(Error::INTERNAL);
    if (device.serial_present) {
        list.enclosure.value.identity.serial_state = IdentitySerialState::present;
        list.enclosure.value.identity.serial.present = true;
        if (!set_string(list.enclosure.value.identity.serial.value,
                        std::string_view(device.serial.bytes.data(), device.serial.length)))
            return Result<void>::failure(Error::INTERNAL);
        list.enclosure.value.identity.uniqueness = !device.serial_uniqueness_known
            ? Uniqueness::unknown : device.serial_unique
                ? Uniqueness::unique_in_scan : Uniqueness::duplicate;
    } else if (device.serial_absent) {
        list.enclosure.value.identity.serial_state = IdentitySerialState::absent;
        list.enclosure.value.identity.uniqueness = Uniqueness::not_applicable;
    } else {
        list.enclosure.value.identity.serial_state = IdentitySerialState::unknown;
        list.enclosure.value.identity.uniqueness = Uniqueness::unknown;
    }
    list.enclosure.value.limits.max_concurrent_streams.present = true;
    list.enclosure.value.limits.max_concurrent_streams.value = 1U;
    list.functions.count = 1U;
    UsbFunction& function = list.functions.items[0];
    function.id = 0U;
    function.vid = 0x06b8U;
    function.pid = dt295 ? 0x103bU : 0x102bU;
    function.mode = Mode::runtime;
    if (!set_string(function.model.value.name, model_id))
        return Result<void>::failure(Error::INTERNAL);
    if (!device.path.empty()) {
        function.location.bus.present = true;
        function.location.bus.value = device.bus;
        function.location.address.present = true;
        function.location.address.value = device.address;
        function.location.usb_path.present = true;
        if (!set_string(function.location.usb_path.value, device.path))
            return Result<void>::failure(Error::INTERNAL);
    }
    function.model.present = true;
    if (device.serial_present) {
        function.serial_state = SerialState::present;
        function.serial.present = true;
        if (!set_string(function.serial.value,
                        std::string_view(device.serial.bytes.data(), device.serial.length)))
            return Result<void>::failure(Error::INTERNAL);
    } else if (device.serial_absent) {
        function.serial_state = SerialState::absent;
    } else {
        function.serial_state = SerialState::not_queried;
    }
    function.identification_reason = IdentificationReason::none;
    function.present = true;
    list.receivers.present = true;
    list.receivers.value.count = 1U;
    ReceiverDescriptor& receiver = list.receivers.value.items[0];
    receiver.id = 0U;
    receiver.systems.mask = 0x03U;
    receiver.binding.present = true;
    receiver.binding.value = Binding{0U, 0U};
    receiver.capabilities.ts_scope = TsScope::multiplex;
    receiver.capabilities.controls = dt295 ? 0U : kControlBitLna;
    if (!dt295) {
        receiver.capabilities.lna.entries.count = 2U;
        receiver.capabilities.lna.entries.items[0].system = System::isdb_t;
        receiver.capabilities.lna.entries.items[0].default_value.present = true;
        receiver.capabilities.lna.entries.items[0].default_value.value = false;
        receiver.capabilities.lna.entries.items[1].system = System::isdb_s;
        receiver.capabilities.lna.entries.items[1].default_value.present = true;
        receiver.capabilities.lna.entries.items[1].default_value.value = true;
    }
    list.readers.present = true;
    list.readers.value.count = 0U;
    if (validate_list_response(list) != Error::OK)
        return Result<void>::failure(Error::INVALID_ARGUMENT);
    return Result<void>::success();
}

}  // namespace pix::runtime
