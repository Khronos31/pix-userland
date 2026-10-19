// SPDX-License-Identifier: GPL-2.0-or-later
#include "pix_runtime.h"

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
#include <thread>
#include <unistd.h>

namespace pix::runtime {
namespace dt181 = ::pix::capture::dt181;
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

Result<void> load_exact(const std::string& path, std::uint8_t* output,
                        std::size_t expected) noexcept
{
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return Result<void>::failure(Error::NOT_FOUND);
    std::size_t offset = 0U;
    while (offset < expected) {
        const ssize_t count = ::read(fd, output + offset, expected - offset);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) {
            (void)::close(fd);
            return Result<void>::failure(Error::INVALID_ARGUMENT);
        }
        offset += static_cast<std::size_t>(count);
    }
    std::uint8_t extra = 0U;
    ssize_t trailing = 0;
    do { trailing = ::read(fd, &extra, 1U); } while (trailing < 0 && errno == EINTR);
    const int close_result = ::close(fd);
    if (trailing != 0 || close_result != 0)
        return Result<void>::failure(Error::INVALID_ARGUMENT);
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

class EmptyCardSession final : public CardProtocolSession {
public:
    Result<void> initialize() noexcept override
    { return Result<void>::failure(Error::NO_CARD); }
    Result<std::size_t> transmit(ByteView, MutableByteView) noexcept override
    { return Result<std::size_t>::failure(Error::NO_CARD); }
    bool initialized() const noexcept override { return false; }
    const CardAtr& atr() const noexcept override { return atr_; }
    void invalidate() noexcept override {}
private:
    CardAtr atr_{};
};

class PixStreamShim final : public px4::userland::TunerStreamControl {
public:
    explicit PixStreamShim(dt181::ReceiverAdapter& adapter) noexcept : adapter_(adapter) {}
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
    static dt181::ReceiverAdapter::Attachment convert(const TunerAttachment& value) noexcept
    {
        dt181::ReceiverAdapter::Attachment out{};
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
            value.usb_errors, value.empty_intervals};
    }
    dt181::ReceiverAdapter& adapter_;
};

class PixTunerBackend final : public TunerServiceBackend {
public:
    explicit PixTunerBackend(dt181::ReceiverAdapter& adapter) noexcept : adapter_(adapter) {}
    std::uint8_t receiver_count() const noexcept override { return 1U; }
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
};

}  // namespace

struct Family::Impl final {
    dt181_session session{};
    std::unique_ptr<Assets> assets;
    PixTime clock;
    dt181::Consumer consumer;
    std::unique_ptr<dt181::ReceiverAdapter> adapter;
    std::unique_ptr<PixTunerBackend> tuner;
    EmptyCardBackend card;
    EmptyCardSession protocol;
    std::unique_ptr<PixStreamShim> shim;
    bool initialized = false;
};

dtv::userland::Result<LocatedDevice> locate_dt181(const std::string& input) noexcept
{
    if (input.rfind("serial:", 0U) == 0U)
        return dtv::userland::Result<LocatedDevice>::failure(Error::NOT_FOUND);
    if (input.rfind("usb:", 0U) != 0U)
        return dtv::userland::Result<LocatedDevice>::failure(Error::INVALID_ARGUMENT);
    LocatedDevice selected{};
    if (usb_selector_parse(input.c_str() + 4, &selected.selector) != 0 ||
        selected.selector.kind != USB_SELECT_PATH)
        return dtv::userland::Result<LocatedDevice>::failure(Error::INVALID_ARGUMENT);

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
    Error failure = Error::NOT_FOUND;
    for (ssize_t index = 0; index < count; ++index) {
        libusb_device_descriptor descriptor{};
        if (libusb_get_device_descriptor(devices[index], &descriptor) != LIBUSB_SUCCESS)
            continue;
        const unsigned bus = libusb_get_bus_number(devices[index]);
        std::uint8_t ports[USB_SELECTOR_MAX_PORTS]{};
        const int port_count = libusb_get_port_numbers(devices[index], ports,
                                                       sizeof(ports));
        if (port_count <= 0 || !usb_selector_matches(&selected.selector, 0U, bus,
                libusb_get_device_address(devices[index]), ports,
                static_cast<unsigned>(port_count))) continue;
        if (descriptor.idVendor != 0x06b8U || descriptor.idProduct != 0x102bU) {
            failure = descriptor.idVendor == 0x06b8U && descriptor.idProduct == 0x103bU
                ? Error::UNSUPPORTED : Error::NOT_FOUND;
            break;
        }
        selected.bus = static_cast<std::uint8_t>(bus);
        selected.address = libusb_get_device_address(devices[index]);
        selected.port_count = static_cast<std::uint8_t>(port_count);
        std::copy(ports, ports + port_count, selected.ports.begin());
        char path[64]{};
        if (usb_selector_format_path(path, sizeof(path), bus, ports,
                                     static_cast<unsigned>(port_count)) != 0) {
            failure = Error::INTERNAL;
            break;
        }
        selected.path.assign(path);
        found = true;
        break;
    }
    libusb_free_device_list(devices, 1);
    libusb_exit(context);
    if (!found) return dtv::userland::Result<LocatedDevice>::failure(failure);
    return dtv::userland::Result<LocatedDevice>::success(std::move(selected));
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

dtv::userland::Result<std::unique_ptr<Assets>> load_assets(const char* directory) noexcept
{
    if (directory == nullptr || directory[0] == '\0')
        return dtv::userland::Result<std::unique_ptr<Assets>>::failure(Error::INVALID_ARGUMENT);
    std::unique_ptr<Assets> assets(new (std::nothrow) Assets());
    if (!assets) return dtv::userland::Result<std::unique_ptr<Assets>>::failure(Error::INTERNAL);
    std::string root(directory);
    if (!root.empty() && root.back() != '/') root.push_back('/');
    assets->firmware = static_cast<std::uint8_t*>(std::malloc(699673U));
    if (assets->firmware == nullptr)
        return dtv::userland::Result<std::unique_ptr<Assets>>::failure(Error::INTERNAL);
    if (!load_exact(root + "fpga.bin", assets->firmware, 699673U) ||
        !load_exact(root + "bootstrap.bin", assets->bootstrap.data(), assets->bootstrap.size()) ||
        !load_exact(root + "validation.bin", assets->validation.data(), assets->validation.size()))
        return dtv::userland::Result<std::unique_ptr<Assets>>::failure(Error::INVALID_ARGUMENT);
    return dtv::userland::Result<std::unique_ptr<Assets>>::success(std::move(assets));
}

dtv::userland::Result<std::unique_ptr<Family>> Family::create(
    const LocatedDevice& device, const char* assets_directory) noexcept
{
    std::unique_ptr<Family> family(new (std::nothrow) Family());
    if (!family) return dtv::userland::Result<std::unique_ptr<Family>>::failure(Error::INTERNAL);
    family->impl_.reset(new (std::nothrow) Impl());
    if (!family->impl_)
        return dtv::userland::Result<std::unique_ptr<Family>>::failure(Error::INTERNAL);
    auto assets = load_assets(assets_directory);
    if (!assets) return dtv::userland::Result<std::unique_ptr<Family>>::failure(assets.error());
    family->impl_->assets = std::move(assets.value());
    const int opened = dt181_session_open(&family->impl_->session, &device.selector, -1, 0);
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
    family->impl_->tuner.reset(new (std::nothrow) PixTunerBackend(*family->impl_->adapter));
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

TunerServiceBackend& Family::tuner_backend() noexcept { return *impl_->tuner; }
CardServiceBackend& Family::card_backend() noexcept { return impl_->card; }
CardProtocolSession& Family::card_protocol() noexcept { return impl_->protocol; }
TunerServiceTime& Family::time() noexcept { return impl_->clock; }
TunerStreamControl* Family::stream_control() noexcept { return impl_->shim.get(); }

Result<void> build_list(const LocatedDevice& device, const std::string& instance,
                        const std::string& version,
                        const std::array<std::uint8_t, ipc::kNonceLength>& epoch_bytes,
                        ControlContext& context) noexcept
{
    using namespace dtv::userland::wire;
    context = ControlContext{};
    context.epoch = epoch_bytes;
    ListResponse& list = context.list;
    list.epoch = context.epoch;
    if (!set_string(list.producer.product, "pix-userland") ||
        !set_string(list.producer.version, version))
        return Result<void>::failure(Error::INTERNAL);
    list.topology.state = TopologyState::complete;
    list.topology.expected_function_count.present = true;
    list.topology.expected_function_count.value = 1U;
    list.enclosure.present = true;
    list.enclosure.value.id = 0U;
    list.enclosure.value.model.present = true;
    if (!set_string(list.enclosure.value.model.value.id, "PIX-DT181-PU0") ||
        !set_string(list.enclosure.value.model.value.name, "PIX-DT181-PU0"))
        return Result<void>::failure(Error::INTERNAL);
    list.enclosure.value.identity.serial_state = IdentitySerialState::unknown;
    list.enclosure.value.identity.uniqueness = Uniqueness::unknown;
    list.enclosure.value.limits.max_concurrent_streams.present = true;
    list.enclosure.value.limits.max_concurrent_streams.value = 1U;
    list.functions.count = 1U;
    UsbFunction& function = list.functions.items[0];
    function.id = 0U;
    function.vid = 0x06b8U;
    function.pid = 0x102bU;
    function.mode = Mode::runtime;
    function.location.bus.present = true;
    function.location.bus.value = device.bus;
    function.location.address.present = true;
    function.location.address.value = device.address;
    function.location.usb_path.present = true;
    if (!set_string(function.location.usb_path.value, device.path) ||
        !set_string(function.model.value.id, "PIX-DT181-PU0") ||
        !set_string(function.model.value.name, "PIX-DT181-PU0"))
        return Result<void>::failure(Error::INTERNAL);
    function.model.present = true;
    function.serial_state = SerialState::not_queried;
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
    receiver.capabilities.controls = kControlBitLna;
    receiver.capabilities.lna.entries.count = 2U;
    receiver.capabilities.lna.entries.items[0].system = System::isdb_t;
    receiver.capabilities.lna.entries.items[0].default_value.present = true;
    receiver.capabilities.lna.entries.items[0].default_value.value = false;
    receiver.capabilities.lna.entries.items[1].system = System::isdb_s;
    receiver.capabilities.lna.entries.items[1].default_value.present = true;
    receiver.capabilities.lna.entries.items[1].default_value.value = true;
    list.readers.present = true;
    list.readers.value.count = 0U;
    context.instance.present = !instance.empty();
    if (context.instance.present && !set_string(context.instance.value, instance))
        return Result<void>::failure(Error::INVALID_ARGUMENT);
    if (validate_list_response(list) != Error::OK)
        return Result<void>::failure(Error::INVALID_ARGUMENT);
    return Result<void>::success();
}

}  // namespace pix::runtime
