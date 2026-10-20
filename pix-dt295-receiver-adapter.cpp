/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "pix-dt295-receiver-adapter.h"

#include <libusb.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <limits>
#include <new>
#include <string>

namespace pix::capture::dt295 {
namespace {

using dtv::userland::Error;
using dtv::userland::detail::TuneProgramming;
using dtv::userland::detail::TuneReadiness;

constexpr std::uint64_t kFirstCenterHz = 473142857U;
constexpr std::uint64_t kChannelSpacingHz = 6000000U;
constexpr unsigned kFirstChannel = 13U;
constexpr unsigned kLastChannel = 62U;

unsigned channel_for_hz(std::uint64_t hz) noexcept
{
    unsigned best = 0U;
    std::uint64_t best_distance = std::numeric_limits<std::uint64_t>::max();
    for (unsigned channel = kFirstChannel; channel <= kLastChannel; ++channel) {
        const std::uint64_t center =
            kFirstCenterHz + (channel - kFirstChannel) * kChannelSpacingHz;
        const std::uint64_t distance = hz > center ? hz - center : center - hz;
        if (distance < best_distance) {
            best_distance = distance;
            best = channel;
        }
    }
    return best_distance <= kChannelSpacingHz / 2U ? best : 0U;
}

std::uint64_t center_for_channel(unsigned channel) noexcept
{
    return kFirstCenterHz + (channel - kFirstChannel) * kChannelSpacingHz;
}

bool same_identity(const dtv::userland::detail::Attachment& left,
                   const dtv::userland::detail::Attachment& right) noexcept
{
    return dtv::userland::detail::PacketStreamSession::same_identity(left, right);
}

}  // namespace

ReceiverAdapter::ReceiverAdapter(const char* assets_directory, const usb_selector& selector,
                                 Clock& clock, ReceiverId receiver,
                                 std::size_t queue_packets) noexcept
    : clock_(clock), receiver_(receiver), queue_packets_(queue_packets)
{
    io_.owner = this;
    initialize(assets_directory, selector);
}

ReceiverAdapter::~ReceiverAdapter() noexcept
{
    (void)shutdown();
}

void ReceiverAdapter::initialize(const char* assets_directory,
                                 const usb_selector& selector) noexcept
{
    init_error_ = Error::INVALID_ARGUMENT;
    if (assets_directory == nullptr || assets_directory[0] == '\0') return;
    std::string path(assets_directory);
    if (!path.empty() && path.back() != '/') path.push_back('/');
    path += "dt295.assets";
    char error[256]{};
    if (dt295_assets_load(path.c_str(), &assets_, error, sizeof(error)) != 0) return;
    if (usb_open_model(&usb_, &selector, -1, 0, USB_MODEL_DT295) != 0) return;
    const dt295_io io{&io_,
                      &ReceiverAdapter::control,
                      &ReceiverAdapter::bulk,
                      &ReceiverAdapter::interrupt,
                      &ReceiverAdapter::now_ms,
                      &ReceiverAdapter::sleep_ms};
    if (dt295_create(&device_, &io, assets_, error, sizeof(error)) != 0) return;
    routes_ = dt295_routes_create();
    if (routes_ == nullptr) return;
    init_error_ = Error::OK;
}

bool ReceiverAdapter::matches(ReceiverId receiver) const noexcept
{
    return receiver == receiver_;
}

bool ReceiverAdapter::supports(ReceiverId receiver, ReceiverSystem system) const noexcept
{
    return matches(receiver) && system == ReceiverSystem::terrestrial;
}

bool ReceiverAdapter::selects_satellite_stream_before_tune() const noexcept { return false; }

std::uint32_t ReceiverAdapter::lock_settle_ms(ReceiverId, ReceiverSystem) const noexcept
{
    return 0U;
}

ReceiverAdapter::Result ReceiverAdapter::configure(Engine& engine, ReceiverId receiver,
                                  std::uint64_t epoch) noexcept
{
    using namespace dtv::userland::detail;
    const ReceiverCapability capability{receiver, true, false, true,
                                        TuneReadiness::commands, false};
    auto strict = engine.configure_strict_capabilities(&capability, 1U);
    if (!strict) return strict;
    return engine.configure_stream_contract(StreamContract::v1, epoch);
}

ReceiverAdapter::Result ReceiverAdapter::open(ReceiverId receiver) noexcept
{
    if (!matches(receiver)) return Result::failure(Error::INVALID_ARGUMENT);
    if (init_error_ != Error::OK) return Result::failure(init_error_);
    if (opened_) return Result::failure(Error::BUSY);
    char error[256]{};
    if (!dt295_has_session(device_)) {
        if (dt295_boot(device_, error, sizeof(error)) != 0)
            return Result::failure(Error::USB_IO);
        if (dt295_open_session(device_, error, sizeof(error)) != 0)
            return Result::failure(Error::USB_IO);
    }
    if (dt295_card_initialize(device_, error, sizeof(error)) != 0)
        return Result::failure(Error::NO_CARD);
    if (dt295_card_activate(device_, 2000U, error, sizeof(error)) != 0)
        return Result::failure(Error::NO_CARD);
    opened_ = true;
    return Result::success();
}

dtv::userland::Result<TuneProgramming> ReceiverAdapter::program_hz(
    ReceiverId receiver, ReceiverSystem system, std::uint64_t requested_hz,
    std::uint32_t) noexcept
{
    using TuneResult = dtv::userland::Result<TuneProgramming>;
    if (!supports(receiver, system))
        return TuneResult::failure(Error::UNSUPPORTED);
    if (!opened_ || init_error_ != Error::OK)
        return TuneResult::failure(Error::NOT_FOUND);
    const unsigned channel = channel_for_hz(requested_hz);
    if (channel == 0U) return TuneResult::failure(Error::INVALID_ARGUMENT);

    if (stream_attached_) {
        const auto stopped = stop_attached(attachment_);
        if (!stopped) return TuneResult::failure(stopped.error());
    } else if (streaming_ || stream_ != nullptr) {
        const auto stopped = stop_receiver_unlocked();
        if (!stopped) return TuneResult::failure(stopped.error());
    }
    plan_complete_ = false;
    std::memset(&live_, 0, sizeof(live_));
    char error[256]{};
    if (dt295_tune_terrestrial(device_, channel, error, sizeof(error)) != 0) {
        return TuneResult::failure(Error::USB_IO);
    }

    TuneProgramming programming{};
    programming.programmed_hz.present = true;
    programming.programmed_hz.value = center_for_channel(channel);
    return TuneResult::success(programming);
}

ReceiverAdapter::Result ReceiverAdapter::tune(ReceiverId receiver, ReceiverSystem system,
                             std::uint32_t frequency_khz,
                             std::uint32_t timeout_ms) noexcept
{
    const auto programmed = program_hz(receiver, system,
        static_cast<std::uint64_t>(frequency_khz) * 1000U, timeout_ms);
    return programmed ? Result::success() : Result::failure(programmed.error());
}

dtv::userland::Result<bool> ReceiverAdapter::is_locked(ReceiverId, ReceiverSystem) noexcept
{
    return dtv::userland::Result<bool>::failure(Error::UNSUPPORTED);
}

ReceiverAdapter::Result ReceiverAdapter::select_satellite_slot(ReceiverId, std::uint8_t,
                                               std::uint32_t) noexcept
{
    return Result::failure(Error::UNSUPPORTED);
}

ReceiverAdapter::Result ReceiverAdapter::select_satellite_tsid(ReceiverId, std::uint16_t,
                                               std::uint32_t) noexcept
{
    return Result::failure(Error::UNSUPPORTED);
}

ReceiverAdapter::Result ReceiverAdapter::close(ReceiverId receiver) noexcept
{
    if (!matches(receiver)) return Result::failure(Error::INVALID_ARGUMENT);
    if (stream_attached_) return Result::failure(Error::BUSY);
    if (streaming_ || stream_ != nullptr) {
        const auto stopped = stop_receiver_unlocked();
        if (!stopped) return stopped;
    }
    opened_ = false;
    return Result::success();
}

ReceiverAdapter::Result ReceiverAdapter::begin_tune_power(ReceiverId receiver, ReceiverSystem system,
                                         std::uint8_t voltage) noexcept
{
    if (!supports(receiver, system)) return Result::failure(Error::UNSUPPORTED);
    return voltage == 0U ? Result::success() : Result::failure(Error::UNSUPPORTED);
}

ReceiverAdapter::Result ReceiverAdapter::commit_tune_power(ReceiverId receiver) noexcept
{
    return matches(receiver) ? Result::success() : Result::failure(Error::INVALID_ARGUMENT);
}

ReceiverAdapter::Result ReceiverAdapter::rollback_tune_power(ReceiverId receiver) noexcept
{
    return matches(receiver) ? Result::success() : Result::failure(Error::INVALID_ARGUMENT);
}

void ReceiverAdapter::mark_disconnected(ReceiverId) noexcept {}

ReceiverAdapter::Result ReceiverAdapter::start_capture(ReceiverId receiver, ReceiverSystem system) noexcept
{
    if (!supports(receiver, system)) return Result::failure(Error::UNSUPPORTED);
    if (!opened_) return Result::failure(Error::NOT_FOUND);
    const auto streaming = ensure_streaming();
    if (!streaming) return streaming;
    if (stream_attached_ && !pump_joinable_) {
        pump_stop_.store(false, std::memory_order_release);
        pump_error_.store(0, std::memory_order_release);
        if (pthread_create(&pump_thread_, nullptr, pump_entry, this) != 0)
            return Result::failure(Error::INTERNAL);
        pump_joinable_ = true;
    }
    return Result::success();
}

ReceiverAdapter::Result ReceiverAdapter::stop_capture(ReceiverId receiver, ReceiverSystem system) noexcept
{
    if (!supports(receiver, system)) return Result::failure(Error::INVALID_ARGUMENT);
    pump_stop_.store(true, std::memory_order_release);
    if (pump_joinable_) {
        if (pthread_join(pump_thread_, nullptr) != 0)
            return Result::failure(Error::INTERNAL);
        pump_joinable_ = false;
    }
    return stop_receiver_unlocked();
}

ReceiverAdapter::Result ReceiverAdapter::stop_receiver_unlocked() noexcept
{
    Result outcome = Result::success();
    if (device_ != nullptr && dt295_has_session(device_)) {
        char error[256]{};
        size_t drained = 0U;
        if (dt295_stop(device_, &drained, error, sizeof(error)) != 0)
            outcome = Result::failure(Error::USB_IO);
    }
    const auto halted = halt_unlocked();
    if (!halted && outcome) outcome = halted;
    streaming_ = false;
    return outcome;
}

ReceiverAdapter::Result ReceiverAdapter::halt_unlocked() noexcept
{
    if (stream_ == nullptr) return Result::success();
    int rc = dt295_stream_stop(stream_, 3000U);
    if (rc == DT295_STREAM_ABANDONED) rc = dt295_stream_stop(stream_, 3000U);
    if (rc != 0) return Result::failure(Error::USB_IO);
    if (dt295_stream_destroy(stream_) != 0) return Result::failure(Error::USB_IO);
    stream_ = nullptr;
    return Result::success();
}

ReceiverAdapter::Result ReceiverAdapter::start_stream_unlocked() noexcept
{
    if (usb_ == nullptr) return Result::failure(Error::DISCONNECTED);
    dt295_stream* stream = nullptr;
    const int rc = dt295_stream_start(
        static_cast<libusb_context*>(usb_native_context(usb_)),
        static_cast<libusb_device_handle*>(usb_native_handle(usb_)), &stream);
    if (rc != 0 || stream == nullptr) return Result::failure(Error::USB_IO);
    stream_ = stream;
    return Result::success();
}

ReceiverAdapter::Result ReceiverAdapter::apply_plan_unlocked(
    dt295_route_plan* current, const dt295_route_plan* desired) noexcept
{
    if (current == nullptr || desired == nullptr || device_ == nullptr)
        return Result::failure(Error::DISCONNECTED);
    dt295_route_plan effective = *desired;
    dt295_route_op ops[DT295_ROUTE_OP_LIMIT];
    size_t count = 0U;
    char route_error[DT295_ROUTE_ERROR]{};
    if (dt295_routes_plan_ops(current, &effective, ops, &count, route_error) != 0)
        return Result::failure(Error::INTERNAL);
    if (count == 0U) return Result::success();
    dt295_route_plan candidate{};
    if (dt295_routes_guard(current, ops, count, &candidate, route_error) != 0)
        return Result::failure(Error::INTERNAL);
    char error[256]{};
    for (size_t i = 0U; i < count; ++i) {
        if (dt295_route_write(device_, &ops[i], error, sizeof(error)) != 0)
            return Result::failure(Error::USB_IO);
    }
    candidate.ready = effective.ready;
    candidate.cat_received = effective.cat_received;
    candidate.missing_pmts = effective.missing_pmts;
    candidate.primary_program = effective.primary_program;
    candidate.has_primary = effective.has_primary;
    candidate.video_stream_type = effective.video_stream_type;
    candidate.audio_stream_type = effective.audio_stream_type;
    std::memcpy(candidate.dedicated_pid, effective.dedicated_pid,
                sizeof(candidate.dedicated_pid));
    dt295_routes_commit(current, &candidate);
    return Result::success();
}

ReceiverAdapter::Result ReceiverAdapter::ensure_streaming() noexcept
{
    if (streaming_) return Result::success();
    if (device_ == nullptr || routes_ == nullptr)
        return Result::failure(Error::DISCONNECTED);
    char error[256]{};
    char route_error[DT295_ROUTE_ERROR]{};
    if (dt295_routes_initial_plan(routes_, &live_, route_error) != 0)
        return Result::failure(Error::INTERNAL);
    const int prepared = key_retained_
        ? dt295_reprepare_plan(device_, &live_, error, sizeof(error))
        : dt295_prepare_plan(device_, &live_, error, sizeof(error));
    if (prepared != 0) return Result::failure(Error::USB_IO);
    key_retained_ = true;
    if (dt295_start(device_, error, sizeof(error)) != 0)
        return Result::failure(Error::USB_IO);
    const auto started = start_stream_unlocked();
    if (!started) {
        size_t drained = 0U;
        (void)dt295_stop(device_, &drained, error, sizeof(error));
        return started;
    }
    streaming_ = true;
    plan_complete_ = false;
    return Result::success();
}

void ReceiverAdapter::pump() noexcept
{
    std::vector<std::uint8_t> residue;
    residue.reserve(65536U + 188U);
    std::uint8_t chunk[65536U];
    std::uint64_t last_renew = clock_.monotonic_ms();
    while (!pump_stop_.load(std::memory_order_acquire)) {
        if (stream_ == nullptr) break;
        std::uint64_t sequence = 0U;
        std::uint64_t completed_ns = 0U;
        const int count = dt295_stream_read(stream_, chunk, sizeof(chunk), &sequence,
                                            &completed_ns, 50U);
        if (count < 0) {
            if (count == DT295_STREAM_ABANDONED || count == DT295_STREAM_CLOSED) break;
            pump_error_.store(static_cast<int>(Error::USB_IO), std::memory_order_release);
            break;
        }
        if (count > 0) residue.insert(residue.end(), chunk, chunk + count);

        std::size_t offset = 0U;
        bool terminal = false;
        while (residue.size() - offset >= 188U) {
            std::uint8_t packet[188U];
            std::memcpy(packet, residue.data() + offset, 188U);
            offset += 188U;
            if (dt295_decode_packet(device_, packet) < 0) continue;
            dt295_routes_feed(routes_, packet);
            {
                std::lock_guard<std::mutex> lock(session_.mutex);
                if (!stream_attached_ || session_.incarnation != attachment_incarnation_ ||
                    !same_identity(session_.identity, attachment_))
                    continue;
                session_.append_locked(dtv::userland::ByteView{packet, 188U});
                if (session_.terminal != dtv::userland::detail::StreamTerminal::none) {
                    terminal = true;
                    break;
                }
            }
        }
        if (offset != 0U)
            residue.erase(residue.begin(), residue.begin() + static_cast<std::ptrdiff_t>(offset));
        if (terminal) break;

        if (!plan_complete_ && count > 0) {
            dt295_routes_snapshot snapshot{};
            dt295_routes_snapshot_get(routes_, &snapshot);
            if (!snapshot.ready && snapshot.have_pat) {
                dt295_route_plan bootstrap{};
                char route_error[DT295_ROUTE_ERROR]{};
                if (dt295_routes_bootstrap_plan(routes_, &live_, &bootstrap, route_error) != 0) {
                    pump_error_.store(static_cast<int>(Error::INTERNAL),
                                      std::memory_order_release);
                    break;
                }
                const auto applied = apply_plan_unlocked(&live_, &bootstrap);
                if (!applied) {
                    pump_error_.store(static_cast<int>(applied.error()),
                                      std::memory_order_release);
                    break;
                }
            }
            if (snapshot.ready) {
                dt295_route_plan plan{};
                char route_error[DT295_ROUTE_ERROR]{};
                if (dt295_routes_build(routes_, 0U, nullptr, &plan, route_error) == 0) {
                    char error[256]{};
                    size_t drained = 0U;
                    (void)dt295_stop(device_, &drained, error, sizeof(error));
                    const auto halted = halt_unlocked();
                    if (!halted) {
                        pump_error_.store(static_cast<int>(halted.error()),
                                          std::memory_order_release);
                        break;
                    }
                    if (dt295_reprepare_plan(device_, &plan, error, sizeof(error)) != 0 ||
                        dt295_start(device_, error, sizeof(error)) != 0) {
                        pump_error_.store(static_cast<int>(Error::USB_IO),
                                          std::memory_order_release);
                        break;
                    }
                    const auto restarted = start_stream_unlocked();
                    if (!restarted) {
                        pump_error_.store(static_cast<int>(restarted.error()),
                                          std::memory_order_release);
                        break;
                    }
                    live_ = plan;
                    plan_complete_ = true;
                    residue.clear();
                }
            }
        }

        const std::uint64_t now = clock_.monotonic_ms();
        if (now >= last_renew && now - last_renew >= 1000U) {
            int renewed = 0;
            char error[256]{};
            (void)dt295_renew_if_due(device_, &renewed, error, sizeof(error));
            last_renew = now;
        }
    }
}

void* ReceiverAdapter::pump_entry(void* opaque) noexcept
{
    static_cast<ReceiverAdapter*>(opaque)->pump();
    return nullptr;
}

ReceiverAdapter::Result ReceiverAdapter::attach(const Attachment& attachment) noexcept
{
    if (!opened_) return Result::failure(Error::NOT_FOUND);
    if (stream_attached_) return Result::failure(Error::BUSY);
    const auto streaming = ensure_streaming();
    if (!streaming) return streaming;
    {
        std::lock_guard<std::mutex> lock(session_.mutex);
        const auto published = session_.begin_attach_locked(
            attachment, queue_packets_,
            attachment.evict_oldest ? dtv::userland::detail::StreamQueuePolicy::evict_oldest
                                    : dtv::userland::detail::StreamQueuePolicy::legacy);
        if (!published) return published;
        attachment_ = attachment;
        attachment_incarnation_ = session_.incarnation;
    }
    pump_stop_.store(false, std::memory_order_release);
    pump_error_.store(0, std::memory_order_release);
    stream_attached_ = true;
    if (pthread_create(&pump_thread_, nullptr, pump_entry, this) != 0) {
        pump_stop_.store(true, std::memory_order_release);
        stream_attached_ = false;
        std::lock_guard<std::mutex> lock(session_.mutex);
        session_.abandon_attach_locked();
        (void)stop_receiver_unlocked();
        return Result::failure(Error::INTERNAL);
    }
    pump_joinable_ = true;
    return Result::success();
}

ReceiverAdapter::Result ReceiverAdapter::stop_attached(const Attachment& attachment) noexcept
{
    if (!stream_attached_) return Result::success();
    if (!same_identity(attachment, attachment_))
        return Result::failure(Error::NOT_FOUND);
    pump_stop_.store(true, std::memory_order_release);
    if (pump_joinable_) {
        if (pthread_join(pump_thread_, nullptr) != 0)
            return Result::failure(Error::INTERNAL);
        pump_joinable_ = false;
    }
    const auto stopped = stop_receiver_unlocked();
    stream_attached_ = false;
    {
        std::lock_guard<std::mutex> lock(session_.mutex);
        const auto detached = session_.detach_locked(attachment);
        if (!detached && stopped) return detached;
    }
    if (!stopped) return stopped;
    const int pump_error = pump_error_.load(std::memory_order_acquire);
    if (pump_error != 0 && pump_error != static_cast<int>(Error::OK))
        return Result::failure(static_cast<Error>(pump_error));
    return Result::success();
}

ReceiverAdapter::Result ReceiverAdapter::detach(const Attachment& attachment) noexcept
{
    return stop_attached(attachment);
}

dtv::userland::Result<dtv::userland::detail::StreamCounters> ReceiverAdapter::stats(
    const Attachment& attachment) const noexcept
{
    return session_.stats(attachment);
}

dtv::userland::Result<dtv::userland::detail::StreamFinalSnapshot>
ReceiverAdapter::final_snapshot(const Attachment& attachment) const noexcept
{
    return session_.final_snapshot_query(attachment);
}

dtv::userland::Result<ReceiverAdapter::ReadResult> ReceiverAdapter::read(
    const Attachment& attachment, dtv::userland::MutableByteView output,
    std::chrono::milliseconds timeout) noexcept
{
    std::unique_lock<std::mutex> lock(session_.mutex);
    const auto ticket = session_.begin_read_locked(attachment);
    if (!ticket) return dtv::userland::Result<ReadResult>::failure(ticket.error());
    if (!session_.read_wake_locked(ticket.value()))
        session_.changed.wait_for(lock, timeout, [&] {
            return session_.read_wake_locked(ticket.value());
        });
    return session_.finish_read_locked(attachment, ticket.value(), output);
}

ReceiverAdapter::Result ReceiverAdapter::release_final(const Attachment& attachment) noexcept
{
    return session_.release_final(attachment);
}

ReceiverAdapter::Result ReceiverAdapter::shutdown() noexcept
{
    pump_stop_.store(true, std::memory_order_release);
    if (pump_joinable_) {
        (void)pthread_join(pump_thread_, nullptr);
        pump_joinable_ = false;
    }
    Result outcome = Result::success();
    if (stream_ != nullptr) {
        const auto halted = halt_unlocked();
        if (!halted) outcome = halted;
    }
    if (device_ != nullptr && dt295_has_session(device_)) {
        char error[256]{};
        size_t drained = 0U;
        if (dt295_stop(device_, &drained, error, sizeof(error)) != 0)
            outcome = Result::failure(Error::USB_IO);
    }
    if (device_ != nullptr && dt295_has_session(device_)) {
        char error[256]{};
        if (dt295_shutdown(device_, error, sizeof(error)) != 0)
            outcome = Result::failure(Error::USB_IO);
    }
    stream_attached_ = false;
    opened_ = false;
    streaming_ = false;
    key_retained_ = false;
    if (routes_ != nullptr) {
        dt295_routes_destroy(routes_);
        routes_ = nullptr;
    }
    if (device_ != nullptr) {
        dt295_destroy(device_);
        device_ = nullptr;
    }
    if (usb_ != nullptr) {
        usb_close(usb_);
        usb_ = nullptr;
    }
    if (assets_ != nullptr) {
        dt295_assets_free(assets_);
        assets_ = nullptr;
    }
    return outcome;
}

int ReceiverAdapter::control(void* opaque, std::uint8_t request_type, std::uint8_t request,
                             std::uint16_t value, std::uint16_t index, std::uint8_t* data,
                             std::size_t length, unsigned timeout_ms,
                             std::size_t* actual) noexcept
{
    auto* io = static_cast<Io*>(opaque);
    if (io == nullptr || io->owner == nullptr || actual == nullptr ||
        length > std::numeric_limits<std::uint16_t>::max())
        return LIBUSB_ERROR_INVALID_PARAM;
    const int result = libusb_control_transfer(
        static_cast<libusb_device_handle*>(usb_native_handle(io->owner->usb_)),
        request_type, request, value, index, data, static_cast<std::uint16_t>(length),
        timeout_ms);
    *actual = result < 0 ? 0U : static_cast<std::size_t>(result);
    return result < 0 ? result : 0;
}

int ReceiverAdapter::bulk(void* opaque, std::uint8_t endpoint, std::uint8_t* data,
                          std::size_t length, unsigned timeout_ms,
                          std::size_t* actual) noexcept
{
    auto* io = static_cast<Io*>(opaque);
    if (io == nullptr || io->owner == nullptr || actual == nullptr)
        return LIBUSB_ERROR_INVALID_PARAM;
    *actual = 0U;
    if (endpoint == 0x81U && io->owner->stream_ != nullptr) {
        const int result = dt295_stream_read(io->owner->stream_, data, length,
                                             &io->last_sequence, &io->last_completed_ns,
                                             timeout_ms);
        if (result > 0) {
            *actual = static_cast<std::size_t>(result);
            return 0;
        }
        return result == 0 ? LIBUSB_ERROR_TIMEOUT : result;
    }
    if (length > static_cast<std::size_t>(std::numeric_limits<int>::max()))
        return LIBUSB_ERROR_INVALID_PARAM;
    int transferred = 0;
    const int result = libusb_bulk_transfer(
        static_cast<libusb_device_handle*>(usb_native_handle(io->owner->usb_)),
        endpoint, data, static_cast<int>(length), &transferred, timeout_ms);
    if (transferred > 0) *actual = static_cast<std::size_t>(transferred);
    return result;
}

int ReceiverAdapter::interrupt(void* opaque, std::uint8_t endpoint, std::uint8_t* data,
                               std::size_t length, unsigned timeout_ms,
                               std::size_t* actual) noexcept
{
    auto* io = static_cast<Io*>(opaque);
    if (io == nullptr || io->owner == nullptr || actual == nullptr ||
        length > static_cast<std::size_t>(std::numeric_limits<int>::max()))
        return LIBUSB_ERROR_INVALID_PARAM;
    int transferred = 0;
    const int result = libusb_interrupt_transfer(
        static_cast<libusb_device_handle*>(usb_native_handle(io->owner->usb_)),
        endpoint, data, static_cast<int>(length), &transferred, timeout_ms);
    *actual = transferred > 0 ? static_cast<std::size_t>(transferred) : 0U;
    return result;
}

std::uint64_t ReceiverAdapter::now_ms(void* opaque) noexcept
{
    auto* io = static_cast<Io*>(opaque);
    return io != nullptr && io->owner != nullptr ? io->owner->clock_.monotonic_ms() : 0U;
}

void ReceiverAdapter::sleep_ms(void* opaque, unsigned milliseconds) noexcept
{
    auto* io = static_cast<Io*>(opaque);
    if (io != nullptr && io->owner != nullptr)
        io->owner->clock_.sleep_ms(milliseconds);
}

}  // namespace pix::capture::dt295
