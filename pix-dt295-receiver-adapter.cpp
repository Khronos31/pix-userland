/* SPDX-License-Identifier: MIT */
#include "pix-dt295-receiver-adapter.h"

#include <libusb.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>
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
constexpr std::uint64_t kAcquisitionDeadlineMs = 30000U;

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

struct RetiredStream final {
    dt295_stream* stream;
    usb_backend* usb;
    ::dt295* device;
    dt295_routes* routes;
    dt295_assets* assets;
};

/* The stream still owns its event thread and transfers after the adapter is
 * gone. This worker keeps only the retired resources; device IO is not called
 * once the adapter has been destroyed, so no io_/clock_ reference remains. */
void* retire_stream_entry(void* opaque) noexcept
{
    auto* retired = static_cast<RetiredStream*>(opaque);
    for (;;) {
        const int rc = dt295_stream_stop(retired->stream, 3000U);
        if (rc == 0) break;
        const struct timespec pause = {0, 100000000L};
        nanosleep(&pause, nullptr);
    }
    (void)dt295_stream_destroy(retired->stream);
    dt295_routes_destroy(retired->routes);
    dt295_destroy(retired->device);
    usb_close(retired->usb);
    dt295_assets_free(retired->assets);
    delete retired;
    return nullptr;
}

}  // namespace

ReceiverAdapter::ReceiverAdapter(const std::uint8_t* assets, std::size_t assets_size,
                                 const usb_selector& selector, Clock& clock,
                                 ReceiverId receiver, std::size_t queue_packets,
                                 int device_fd) noexcept
    : clock_(clock), receiver_(receiver), queue_packets_(queue_packets)
{
    io_.owner = this;
    initialize(assets, assets_size, selector, device_fd);
}

ReceiverAdapter::~ReceiverAdapter() noexcept
{
    const auto outcome = shutdown();
    if (outcome || stream_ == nullptr) return;
    auto* retired = new (std::nothrow)
        RetiredStream{stream_, usb_, device_, routes_, assets_};
    if (retired == nullptr) {
        std::fprintf(stderr, "DT295 retirement allocation failed; resources retained\n");
        return;
    }
    stream_ = nullptr;
    usb_ = nullptr;
    device_ = nullptr;
    routes_ = nullptr;
    assets_ = nullptr;
    pthread_attr_t attributes;
    if (pthread_attr_init(&attributes) != 0) {
        std::fprintf(stderr, "DT295 retirement attributes failed; resources retained\n");
        return;
    }
    (void)pthread_attr_setdetachstate(&attributes, PTHREAD_CREATE_DETACHED);
    pthread_t thread{};
    const int created = pthread_create(&thread, &attributes, retire_stream_entry, retired);
    (void)pthread_attr_destroy(&attributes);
    if (created != 0)
        std::fprintf(stderr, "DT295 retirement thread failed; resources retained\n");
}

void ReceiverAdapter::initialize(const std::uint8_t* assets, std::size_t assets_size,
                                 const usb_selector& selector, int device_fd) noexcept
{
    init_error_ = Error::INVALID_ARGUMENT;
    char error[256]{};
    if (dt295_assets_load_bytes(assets, assets_size, &assets_, error, sizeof(error)) != 0) {
        std::snprintf(init_detail_.data(), init_detail_.size(), "%s", error);
        return;
    }
    initialize_device(selector, device_fd);
}

void ReceiverAdapter::initialize_device(const usb_selector& selector, int device_fd) noexcept
{
    char error[256]{};
    if (usb_open_model(&usb_, &selector, device_fd, 0, USB_MODEL_DT295) != 0) return;
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
    return matches(receiver) && (system == ReceiverSystem::terrestrial ||
                                 system == ReceiverSystem::satellite);
}

ReceiverAdapter::AcquisitionSnapshot ReceiverAdapter::acquisition() const noexcept
{
    AcquisitionSnapshot snapshot{};
    snapshot.phase = acquisition_phase_.load(std::memory_order_acquire);
    snapshot.failure = acquisition_failure_.load(std::memory_order_acquire);
    snapshot.tune_generation = tune_generation_.load(std::memory_order_acquire);
    snapshot.acquisition_generation = acquisition_generation_.load(std::memory_order_acquire);
    snapshot.plan_generation = plan_generation_.load(std::memory_order_acquire);
    return snapshot;
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
    const ReceiverCapability capability{receiver, true, true, true,
                                        TuneReadiness::commands, false};
    auto strict = engine.configure_strict_capabilities(&capability, 1U);
    if (!strict) return strict;
    return engine.configure_stream_contract(StreamContract::v1, epoch);
}

ReceiverAdapter::Result ReceiverAdapter::open(ReceiverId receiver) noexcept
{
    std::lock_guard<std::mutex> operation_lock(operation_mutex_);
    if (!matches(receiver)) return Result::failure(Error::INVALID_ARGUMENT);
    if (init_error_ != Error::OK) return Result::failure(init_error_);
    if (recovery_failed_) return Result::failure(Error::USB_IO);
    if (opened_) return Result::failure(Error::BUSY);
    std::lock_guard<std::mutex> protocol_lock(protocol_mutex_);
    dt295_card_invalidate_observation(device_);
    char error[256]{};
    if (!dt295_has_session(device_)) {
        if (dt295_boot(device_, error, sizeof(error)) != 0) {
            dt295_card_invalidate_observation(device_);
            return Result::failure(Error::USB_IO);
        }
        if (dt295_open_session(device_, error, sizeof(error)) != 0) {
            dt295_card_invalidate_observation(device_);
            return Result::failure(Error::USB_IO);
        }
    }
    if (dt295_card_initialize(device_, error, sizeof(error)) != 0) {
        dt295_card_invalidate_observation(device_);
        return Result::failure(Error::NO_CARD);
    }
    if (dt295_card_activate(device_, 2000U, error, sizeof(error)) != 0) {
        dt295_card_invalidate_observation(device_);
        return Result::failure(Error::NO_CARD);
    }
    opened_ = true;
    acquisition_phase_.store(AcquisitionPhase::idle, std::memory_order_release);
    acquisition_failure_.store(AcquisitionFailure::none, std::memory_order_release);
    plan_generation_.store(0U, std::memory_order_release);
    session_ended_ = false;
    return Result::success();
}

dtv::userland::Result<TuneProgramming> ReceiverAdapter::program_hz(
    ReceiverId receiver, ReceiverSystem system, std::uint64_t requested_hz,
    std::uint32_t) noexcept
{
    using TuneResult = dtv::userland::Result<TuneProgramming>;
    std::lock_guard<std::mutex> operation_lock(operation_mutex_);
    if (!supports(receiver, system))
        return TuneResult::failure(Error::UNSUPPORTED);
    if (!opened_ || init_error_ != Error::OK)
        return TuneResult::failure(Error::NOT_FOUND);
    if (recovery_failed_)
        return TuneResult::failure(Error::USB_IO);
    unsigned channel = 0U;
    if (system == ReceiverSystem::terrestrial) {
        channel = channel_for_hz(requested_hz);
        if (channel == 0U) return TuneResult::failure(Error::INVALID_ARGUMENT);
    } else if (requested_hz < 950000000U || requested_hz > 2150000000U) {
        return TuneResult::failure(Error::INVALID_ARGUMENT);
    }

    tune_generation_.fetch_add(1U, std::memory_order_acq_rel);
    acquisition_phase_.store(AcquisitionPhase::idle, std::memory_order_release);
    acquisition_failure_.store(AcquisitionFailure::none, std::memory_order_release);
    plan_generation_.store(0U, std::memory_order_release);

    if (stream_attached_) {
        const auto stopped = stop_attached(attachment_);
        if (!stopped) return TuneResult::failure(stopped.error());
    } else if (streaming_ || stream_ != nullptr) {
        const auto stopped = stop_receiver_unlocked();
        if (!stopped) return TuneResult::failure(stopped.error());
    }
    tuned_system_.store(system, std::memory_order_release);
    std::lock_guard<std::mutex> protocol_lock(protocol_mutex_);
    plan_complete_ = false;
    std::memset(&live_, 0, sizeof(live_));
    dt295_routes_reset(routes_);
    dt295_routes_set_satellite(routes_, system == ReceiverSystem::satellite ? 1 : 0);
    char error[256]{};
    std::uint64_t programmed_hz = 0U;
    if (system == ReceiverSystem::terrestrial) {
        if (satellite_tuned_ && dt295_sleep_satellite(device_, error, sizeof(error)) != 0)
            return TuneResult::failure(Error::USB_IO);
        satellite_tuned_ = false;
        if (dt295_tune_terrestrial(device_, channel, error, sizeof(error)) != 0)
            return TuneResult::failure(Error::USB_IO);
        programmed_hz = center_for_channel(channel);
    } else {
        const int tsid = satellite_tsid_valid_.load(std::memory_order_acquire)
                             ? static_cast<int>(satellite_tsid_.load(std::memory_order_acquire))
                             : -1;
        if (dt295_tune_satellite(device_, static_cast<std::uint32_t>(requested_hz), tsid,
                                 error, sizeof(error)) != 0)
            return TuneResult::failure(Error::USB_IO);
        satellite_tuned_ = true;
        const std::uint64_t rounded = ((requested_hz / 1000U + 500U) / 1000U) * 1000U;
        programmed_hz = rounded * 1000U;
    }

    TuneProgramming programming{};
    programming.programmed_hz = programmed_hz;
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

dtv::userland::Result<bool> ReceiverAdapter::is_locked(ReceiverId receiver,
                                             ReceiverSystem system) noexcept
{
    std::lock_guard<std::mutex> operation_lock(operation_mutex_);
    if (!supports(receiver, system) || system != ReceiverSystem::satellite)
        return dtv::userland::Result<bool>::failure(Error::UNSUPPORTED);
    if (!opened_ || init_error_ != Error::OK)
        return dtv::userland::Result<bool>::failure(Error::NOT_FOUND);
    if (recovery_failed_)
        return dtv::userland::Result<bool>::failure(Error::USB_IO);
    std::lock_guard<std::mutex> protocol_lock(protocol_mutex_);
    dt295_satellite_state state{};
    char error[256]{};
    if (dt295_satellite_status(device_, &state, error, sizeof(error)) != 0)
        return dtv::userland::Result<bool>::failure(Error::USB_IO);
    const bool target = !satellite_tsid_valid_.load(std::memory_order_acquire) ||
                        state.selected_tsid == satellite_tsid_.load(std::memory_order_acquire);
    return dtv::userland::Result<bool>::success(state.locked != 0U && target);
}

std::optional<bool> ReceiverAdapter::sample_card_inserted() noexcept
{
    const std::uint64_t started = clock_.monotonic_ms();
    const std::uint64_t observation_deadline = started + 1000U;
    const std::uint64_t recovery_deadline = started + 3000U;
    std::unique_lock<std::mutex> operation_lock(operation_mutex_, std::try_to_lock);
    if (!operation_lock.owns_lock())
        return std::nullopt;
    std::unique_lock<std::mutex> protocol_lock(protocol_mutex_, std::try_to_lock);
    if (!protocol_lock.owns_lock() || !opened_ || init_error_ != Error::OK)
        return std::nullopt;
    dt295_card_snapshot snapshot{};
    if (dt295_card_observe_until(device_, &snapshot, observation_deadline) != 0) {
        dt295_card_invalidate_observation(device_);
        return std::nullopt;
    }
    if (snapshot.raw_state == 3U)
        return true;
    if (snapshot.raw_state == 0U && snapshot.activation_valid &&
        snapshot.notification_generation > snapshot.activation_generation)
        return false;
    if (snapshot.raw_state == 1U && snapshot.fresh_notification && snapshot.activation_valid &&
        snapshot.notification_generation > snapshot.activation_generation) {
        if (stream_attached_ || streaming_ || stream_ != nullptr)
            return std::nullopt;
        dt295_card_snapshot recovered{};
        char error[256]{};
        const dt295_card_recovery_result result = dt295_card_recover(
            device_, &snapshot, recovery_deadline, &recovered, error, sizeof(error));
        if (result == DT295_CARD_RECOVERY_ACTIVATED)
            return true;
        if (result == DT295_CARD_RECOVERY_FATAL) {
            recovery_failed_ = true;
            std::fprintf(stderr, "DT295 card recovery fatal: %s\n", error);
            dt295_card_invalidate_observation(device_);
        }
    }
    return std::nullopt;
}

ReceiverAdapter::Result ReceiverAdapter::select_satellite_slot(ReceiverId receiver, std::uint8_t slot,
                                               std::uint32_t timeout_ms) noexcept
{
    std::lock_guard<std::mutex> operation_lock(operation_mutex_);
    if (!matches(receiver)) return Result::failure(Error::INVALID_ARGUMENT);
    if (!opened_ || init_error_ != Error::OK) return Result::failure(Error::NOT_FOUND);
    if (recovery_failed_) return Result::failure(Error::USB_IO);
    if (!satellite_tuned_) return Result::failure(Error::NOT_READY);
    if (slot > 7U) return Result::failure(Error::INVALID_ARGUMENT);
    std::lock_guard<std::mutex> protocol_lock(protocol_mutex_);
    const std::uint32_t total_budget = timeout_ms == 0U ? 3000U : std::min(timeout_ms, 5000U);
    const std::uint64_t deadline = clock_.monotonic_ms() + total_budget;
    char error[256]{};
    if (dt295_satellite_wait_lock(device_, total_budget, error, sizeof(error)) != 0)
        return Result::failure(Error::TIMEOUT);
    uint16_t table[8]{};
    if (dt295_satellite_tsid_table(device_, table, error, sizeof(error)) != 0)
        return Result::failure(Error::USB_IO);
    const std::uint16_t tsid = table[slot];
    if (tsid == 0U || tsid == 0xffffU) return Result::failure(Error::INVALID_ARGUMENT);
    satellite_tsid_.store(tsid, std::memory_order_release);
    satellite_tsid_valid_.store(true, std::memory_order_release);
    if (dt295_select_satellite_tsid(device_, tsid, error, sizeof(error)) != 0)
        return Result::failure(Error::USB_IO);
    for (;;) {
        dt295_satellite_state state{};
        if (dt295_satellite_status(device_, &state, error, sizeof(error)) != 0)
            return Result::failure(Error::USB_IO);
        if (state.selected_tsid == tsid && state.locked) return Result::success();
        if (clock_.monotonic_ms() >= deadline) return Result::failure(Error::TIMEOUT);
        clock_.sleep_ms(100U);
    }
}

ReceiverAdapter::Result ReceiverAdapter::select_satellite_tsid(ReceiverId receiver, std::uint16_t tsid,
                                               std::uint32_t timeout_ms) noexcept
{
    std::lock_guard<std::mutex> operation_lock(operation_mutex_);
    if (!matches(receiver)) return Result::failure(Error::INVALID_ARGUMENT);
    if (!opened_ || init_error_ != Error::OK) return Result::failure(Error::NOT_FOUND);
    if (recovery_failed_) return Result::failure(Error::USB_IO);
    satellite_tsid_.store(tsid, std::memory_order_release);
    satellite_tsid_valid_.store(true, std::memory_order_release);
    if (!satellite_tuned_) return Result::success();
    std::lock_guard<std::mutex> protocol_lock(protocol_mutex_);
    char error[256]{};
    if (dt295_select_satellite_tsid(device_, tsid, error, sizeof(error)) != 0)
        return Result::failure(Error::USB_IO);
    const std::uint32_t budget = timeout_ms == 0U ? 3000U : std::min(timeout_ms, 5000U);
    const std::uint64_t deadline = clock_.monotonic_ms() + budget;
    for (;;) {
        dt295_satellite_state state{};
        if (dt295_satellite_status(device_, &state, error, sizeof(error)) != 0)
            return Result::failure(Error::USB_IO);
        if (state.selected_tsid == tsid && state.locked) return Result::success();
        if (clock_.monotonic_ms() >= deadline) return Result::failure(Error::TIMEOUT);
        clock_.sleep_ms(100U);
    }
}

ReceiverAdapter::Result ReceiverAdapter::close(ReceiverId receiver) noexcept
{
    std::lock_guard<std::mutex> operation_lock(operation_mutex_);
    if (!matches(receiver)) return Result::failure(Error::INVALID_ARGUMENT);
    if (stream_attached_) return Result::failure(Error::BUSY);
    if (streaming_ || stream_ != nullptr) {
        const auto stopped = stop_receiver_unlocked();
        if (!stopped) return stopped;
    }
    {
        std::lock_guard<std::mutex> protocol_lock(protocol_mutex_);
        dt295_card_invalidate_observation(device_);
        opened_ = false;
    }
    satellite_tuned_ = false;
    satellite_tsid_valid_.store(false, std::memory_order_release);
    acquisition_phase_.store(AcquisitionPhase::idle, std::memory_order_release);
    acquisition_failure_.store(AcquisitionFailure::none, std::memory_order_release);
    plan_generation_.store(0U, std::memory_order_release);
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

void ReceiverAdapter::mark_disconnected(ReceiverId) noexcept
{
    std::lock_guard<std::mutex> operation_lock(operation_mutex_);
    std::lock_guard<std::mutex> protocol_lock(protocol_mutex_);
    dt295_card_invalidate_observation(device_);
    opened_ = false;
    fail_acquisition(AcquisitionFailure::disconnected,
                     dtv::userland::detail::StreamTerminal::disconnected,
                     Error::DISCONNECTED);
}

ReceiverAdapter::Result ReceiverAdapter::start_capture(ReceiverId receiver, ReceiverSystem system) noexcept
{
    std::lock_guard<std::mutex> operation_lock(operation_mutex_);
    if (!supports(receiver, system)) return Result::failure(Error::UNSUPPORTED);
    if (!opened_) return Result::failure(Error::NOT_FOUND);
    Result streaming = Result::success();
    {
        std::lock_guard<std::mutex> protocol_lock(protocol_mutex_);
        streaming = ensure_streaming();
    }
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
    std::lock_guard<std::mutex> operation_lock(operation_mutex_);
    if (!supports(receiver, system)) return Result::failure(Error::INVALID_ARGUMENT);
    pump_stop_.store(true, std::memory_order_release);
    if (pump_joinable_) {
        if (pthread_join(pump_thread_, nullptr) != 0)
            return Result::failure(Error::INTERNAL);
        pump_joinable_ = false;
    }
    const auto stopped = stop_receiver_unlocked();
    end_session_for_acquisition(dtv::userland::detail::StreamTerminal::stopped);
    return stopped;
}

ReceiverAdapter::Result ReceiverAdapter::stop_receiver_unlocked() noexcept
{
    Result outcome = Result::success();
    {
        std::lock_guard<std::mutex> protocol_lock(protocol_mutex_);
        if (device_ != nullptr && dt295_has_session(device_)) {
            char error[256]{};
            size_t drained = 0U;
            if (dt295_stop(device_, &drained, error, sizeof(error)) != 0)
                outcome = Result::failure(Error::USB_IO);
        }
    }
    const auto halted = halt_unlocked();
    if (!halted && outcome) outcome = halted;
    streaming_ = false;
    plan_generation_.store(0U, std::memory_order_release);
    acquisition_phase_.store(AcquisitionPhase::idle, std::memory_order_release);
    acquisition_failure_.store(AcquisitionFailure::none, std::memory_order_release);
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
    if (stream_ != nullptr) return Result::failure(Error::BUSY);
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
    if (recovery_failed_) return Result::failure(Error::USB_IO);
    if (streaming_) return Result::success();
    if (stream_ != nullptr) return Result::failure(Error::BUSY);
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
    pump_error_.store(0, std::memory_order_release);
    acquisition_failure_.store(AcquisitionFailure::none, std::memory_order_release);
    acquisition_started_ms_ = clock_.monotonic_ms();
    plan_generation_.store(0U, std::memory_order_release);
    acquisition_generation_.fetch_add(1U, std::memory_order_acq_rel);
    acquisition_phase_.store(AcquisitionPhase::preparing, std::memory_order_release);
    return Result::success();
}

void ReceiverAdapter::end_session_for_acquisition(
    dtv::userland::detail::StreamTerminal terminal) noexcept
{
    std::lock_guard<std::mutex> lock(session_.mutex);
    if (!stream_attached_ || session_.incarnation != attachment_incarnation_ ||
        !same_identity(session_.identity, attachment_))
        return;
    const auto detached = session_.detach_locked(attachment_);
    if (detached) {
        if (session_.final_snapshot.terminal == static_cast<std::uint8_t>(
                dtv::userland::detail::StreamTerminal::stopped))
            session_.amend_final_terminal_locked(attachment_, terminal);
        session_ended_ = true;
    }
}

void ReceiverAdapter::fail_acquisition(AcquisitionFailure failure,
                                       dtv::userland::detail::StreamTerminal terminal,
                                       Error error) noexcept
{
    acquisition_failure_.store(failure, std::memory_order_release);
    acquisition_phase_.store(AcquisitionPhase::terminal, std::memory_order_release);
    pump_error_.store(static_cast<int>(error), std::memory_order_release);
    end_session_for_acquisition(terminal);
}

void ReceiverAdapter::pump() noexcept
{
    std::vector<std::uint8_t> residue;
    residue.reserve(65536U + 188U);
    std::uint8_t chunk[65536U];
    std::uint64_t last_renew = clock_.monotonic_ms();
    const std::uint64_t generation = acquisition_generation_.load(std::memory_order_acquire);
    const auto acquisition_valid = [&]() noexcept {
        return !pump_stop_.load(std::memory_order_acquire) &&
               acquisition_phase_.load(std::memory_order_acquire) !=
                   AcquisitionPhase::terminal &&
               acquisition_generation_.load(std::memory_order_acquire) == generation;
    };
    while (!pump_stop_.load(std::memory_order_acquire)) {
        if (stream_ == nullptr) break;
        if (acquisition_phase_.load(std::memory_order_acquire) == AcquisitionPhase::terminal)
            break;
        if (acquisition_phase_.load(std::memory_order_acquire) ==
                AcquisitionPhase::preparing &&
            clock_.monotonic_ms() - acquisition_started_ms_ >= kAcquisitionDeadlineMs) {
            fail_acquisition(AcquisitionFailure::deadline,
                             dtv::userland::detail::StreamTerminal::sync_error,
                             Error::TIMEOUT);
            break;
        }
        std::uint64_t sequence = 0U;
        std::uint64_t completed_ns = 0U;
        const int count = dt295_stream_read(stream_, chunk, sizeof(chunk), &sequence,
                                            &completed_ns, 50U);
        if (count < 0) {
            if (count == DT295_STREAM_ABANDONED || count == DT295_STREAM_CLOSED) break;
            fail_acquisition(AcquisitionFailure::usb,
                             dtv::userland::detail::StreamTerminal::usb_error,
                             Error::USB_IO);
            break;
        }
        if (count > 0) residue.insert(residue.end(), chunk, chunk + count);

        std::size_t offset = 0U;
        bool terminal = false;
        bool decode_failed = false;
        const bool ready =
            acquisition_phase_.load(std::memory_order_acquire) == AcquisitionPhase::ready;
        while (residue.size() - offset >= 188U) {
            std::uint8_t packet[188U];
            std::memcpy(packet, residue.data() + offset, 188U);
            offset += 188U;
            {
                std::lock_guard<std::mutex> protocol_lock(protocol_mutex_);
                if (dt295_decode_packet(device_, packet) < 0) {
                    decode_failed = true;
                    break;
                }
            }
            dt295_routes_feed(routes_, packet);
            if (!ready) continue;
            {
                std::lock_guard<std::mutex> lock(session_.mutex);
                if (!stream_attached_ || !session_.attached ||
                    session_.terminal != dtv::userland::detail::StreamTerminal::none ||
                    session_.incarnation != attachment_incarnation_ ||
                    !same_identity(session_.identity, attachment_))
                    continue;
                session_.append_locked(dtv::userland::ByteView{packet, 188U});
                if (session_.terminal != dtv::userland::detail::StreamTerminal::none) {
                    terminal = true;
                    break;
                }
            }
        }
        if (decode_failed) {
            fail_acquisition(AcquisitionFailure::packet,
                             dtv::userland::detail::StreamTerminal::sync_error,
                             Error::INTERNAL);
            break;
        }
        if (offset != 0U)
            residue.erase(residue.begin(), residue.begin() + static_cast<std::ptrdiff_t>(offset));
        if (terminal) break;
        if (!acquisition_valid()) break;

        if (!plan_complete_ && count > 0) {
            dt295_routes_snapshot snapshot{};
            dt295_routes_snapshot_get(routes_, &snapshot);
            if (tuned_system_.load(std::memory_order_acquire) == ReceiverSystem::satellite &&
                satellite_tsid_valid_.load(std::memory_order_acquire) &&
                snapshot.have_pat &&
                snapshot.tsid != satellite_tsid_.load(std::memory_order_acquire)) {
                fail_acquisition(AcquisitionFailure::satellite_tsid,
                                 dtv::userland::detail::StreamTerminal::bridge_fatal,
                                 Error::UNSUPPORTED);
                break;
            }
            if (snapshot.unsupported_tables != 0U) {
                fail_acquisition(AcquisitionFailure::unsupported,
                                 dtv::userland::detail::StreamTerminal::bridge_fatal,
                                 Error::UNSUPPORTED);
                break;
            }
            if (!snapshot.ready && snapshot.have_pat) {
                dt295_route_plan bootstrap{};
                char route_error[DT295_ROUTE_ERROR]{};
                if (dt295_routes_bootstrap_plan(routes_, &live_, &bootstrap, route_error) != 0) {
                    fail_acquisition(AcquisitionFailure::plan,
                                     dtv::userland::detail::StreamTerminal::bridge_fatal,
                                     Error::INTERNAL);
                    break;
                }
                if (!acquisition_valid()) break;
                Result applied = Result::success();
                {
                    std::lock_guard<std::mutex> protocol_lock(protocol_mutex_);
                    applied = apply_plan_unlocked(&live_, &bootstrap);
                }
                if (!applied) {
                    fail_acquisition(AcquisitionFailure::usb,
                                     dtv::userland::detail::StreamTerminal::usb_error,
                                     applied.error());
                    break;
                }
            }
            if (snapshot.ready) {
                dt295_route_plan plan{};
                char route_error[DT295_ROUTE_ERROR]{};
                if (dt295_routes_build(routes_, 0U, nullptr, &plan, route_error) != 0) {
                    fail_acquisition(AcquisitionFailure::plan,
                                     dtv::userland::detail::StreamTerminal::bridge_fatal,
                                     Error::INTERNAL);
                    break;
                }
                unsigned ecms = 0U;
                for (unsigned i = 0U; i < DT295_SLOT_COUNT; ++i)
                    if (plan.routes[i].enabled && plan.routes[i].is_ecm) ++ecms;
                if (ecms > 1U) {
                    fail_acquisition(AcquisitionFailure::multiple_ecm,
                                     dtv::userland::detail::StreamTerminal::bridge_fatal,
                                     Error::UNSUPPORTED);
                    break;
                }
                if (!acquisition_valid()) break;
                char error[256]{};
                size_t drained = 0U;
                bool stopped = true;
                {
                    std::lock_guard<std::mutex> protocol_lock(protocol_mutex_);
                    stopped = dt295_stop(device_, &drained, error, sizeof(error)) == 0;
                }
                if (!stopped) {
                    fail_acquisition(AcquisitionFailure::usb,
                                     dtv::userland::detail::StreamTerminal::usb_error,
                                     Error::USB_IO);
                    break;
                }
                if (!acquisition_valid()) break;
                const auto halted = halt_unlocked();
                if (!halted) {
                    fail_acquisition(AcquisitionFailure::usb,
                                     dtv::userland::detail::StreamTerminal::usb_error,
                                     halted.error());
                    break;
                }
                if (!acquisition_valid()) break;
                bool prepared = false;
                {
                    std::lock_guard<std::mutex> protocol_lock(protocol_mutex_);
                    prepared = dt295_reprepare_plan(device_, &plan, error, sizeof(error)) == 0;
                }
                if (!prepared) {
                    fail_acquisition(AcquisitionFailure::usb,
                                     dtv::userland::detail::StreamTerminal::usb_error,
                                     Error::USB_IO);
                    break;
                }
                if (!acquisition_valid()) break;
                bool started_device = false;
                {
                    std::lock_guard<std::mutex> protocol_lock(protocol_mutex_);
                    started_device = dt295_start(device_, error, sizeof(error)) == 0;
                }
                if (!started_device) {
                    fail_acquisition(AcquisitionFailure::usb,
                                     dtv::userland::detail::StreamTerminal::usb_error,
                                     Error::USB_IO);
                    break;
                }
                if (!acquisition_valid()) break;
                const auto restarted = start_stream_unlocked();
                if (!restarted) {
                    fail_acquisition(AcquisitionFailure::usb,
                                     dtv::userland::detail::StreamTerminal::usb_error,
                                     restarted.error());
                    break;
                }
                if (!acquisition_valid()) break;
                AcquisitionPhase expected = AcquisitionPhase::preparing;
                if (!acquisition_phase_.compare_exchange_strong(
                        expected, AcquisitionPhase::ready, std::memory_order_acq_rel,
                        std::memory_order_acquire))
                    break;
                live_ = plan;
                plan_complete_ = true;
                residue.clear();
                plan_generation_.fetch_add(1U, std::memory_order_acq_rel);
            }
        }

        const std::uint64_t now = clock_.monotonic_ms();
        if (now >= last_renew && now - last_renew >= 1000U) {
            int renewed = 0;
            char error[256]{};
            {
                std::lock_guard<std::mutex> protocol_lock(protocol_mutex_);
                (void)dt295_renew_if_due(device_, &renewed, error, sizeof(error));
            }
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
    std::lock_guard<std::mutex> operation_lock(operation_mutex_);
    if (!opened_) return Result::failure(Error::NOT_FOUND);
    if (stream_attached_) return Result::failure(Error::BUSY);
    Result streaming = Result::success();
    {
        std::lock_guard<std::mutex> protocol_lock(protocol_mutex_);
        streaming = ensure_streaming();
    }
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
        session_ended_ = false;
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
        if (session_ended_) {
            session_ended_ = false;
        } else {
            const auto detached = session_.detach_locked(attachment);
            if (!detached && stopped) return detached;
        }
    }
    if (!stopped) return stopped;
    const int pump_error = pump_error_.load(std::memory_order_acquire);
    if (pump_error != 0 && pump_error != static_cast<int>(Error::OK))
        return Result::failure(static_cast<Error>(pump_error));
    return Result::success();
}

ReceiverAdapter::Result ReceiverAdapter::detach(const Attachment& attachment) noexcept
{
    std::lock_guard<std::mutex> operation_lock(operation_mutex_);
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
    auto result = session_.finish_read_locked(attachment, ticket.value(), output);
    if (result && result.value().eof &&
        result.value().terminal == dtv::userland::detail::StreamTerminal::stopped &&
        session_.final_snapshot_valid &&
        same_identity(session_.final_identity, attachment)) {
        const auto reason = static_cast<dtv::userland::detail::StreamTerminal>(
            session_.final_snapshot.terminal);
        if (reason != dtv::userland::detail::StreamTerminal::none &&
            reason != dtv::userland::detail::StreamTerminal::stopped)
            result.value().terminal = reason;
    }
    return result;
}

ReceiverAdapter::Result ReceiverAdapter::release_final(const Attachment& attachment) noexcept
{
    return session_.release_final(attachment);
}

ReceiverAdapter::Result ReceiverAdapter::shutdown() noexcept
{
    std::lock_guard<std::mutex> operation_lock(operation_mutex_);
    pump_stop_.store(true, std::memory_order_release);
    if (pump_joinable_) {
        (void)pthread_join(pump_thread_, nullptr);
        pump_joinable_ = false;
    }
    end_session_for_acquisition(dtv::userland::detail::StreamTerminal::stopped);
    acquisition_phase_.store(AcquisitionPhase::idle, std::memory_order_release);
    acquisition_failure_.store(AcquisitionFailure::none, std::memory_order_release);
    plan_generation_.store(0U, std::memory_order_release);
    Result outcome = Result::success();
    if (stream_ != nullptr) {
        const auto halted = halt_unlocked();
        if (!halted) {
            // The event thread still owns the stream, handle and context.
            // Keep every resource reachable for a later stop; releasing the
            // USB handle here would let callbacks touch freed memory.
            streaming_ = false;
            return halted;
        }
    }
    {
        std::lock_guard<std::mutex> protocol_lock(protocol_mutex_);
        dt295_card_invalidate_observation(device_);
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
        if (routes_ != nullptr) {
            dt295_routes_destroy(routes_);
            routes_ = nullptr;
        }
        if (device_ != nullptr) {
            dt295_destroy(device_);
            device_ = nullptr;
        }
        opened_ = false;
    }
    stream_attached_ = false;
    streaming_ = false;
    key_retained_ = false;
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
