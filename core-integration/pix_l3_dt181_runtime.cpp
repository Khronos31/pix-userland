// SPDX-License-Identifier: GPL-2.0-or-later
#include "pix_l3_dt181_runtime.h"

#include "dt181_capture_host.h"
#include "usb_backend.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <new>
#include <thread>
#include <utility>

namespace pix::runtime {
namespace {

using px4::userland::Error;
using px4::userland::Result;
using px4::userland::TunerAttachment;
using px4::userland::TunerHzProgramming;
using px4::userland::TunerStrictProfile;
using px4::userland::TunerStreamCounters;
using px4::userland::TunerStreamFinalSnapshot;
using px4::userland::TunerStreamReadResult;
using px4::userland::TunerStreamTerminal;
using px4::userland::ipc::System;

struct NativeAdapterOwner final {
    L3Dt181Runtime *runtime;
    dtv_l3_adapter_release_fn release;
    void *adapter_owner;
};

dtv_l3_result product_configure(void *, std::uint32_t, std::uint64_t,
                                const std::uint8_t *, std::size_t) noexcept
{
    return DTV_L3_OK;
}

dtv_l3_claim_result product_claim(void *, std::uint32_t) noexcept
{
    return {DTV_L3_OK, DTV_L3_CLAIM_RETAINED};
}

dtv_l3_release_result product_release_claim(void *, std::uint32_t) noexcept
{
    return {DTV_L3_RELEASE_QUIESCED, DTV_L3_CLAIM_NONE,
            DTV_L3_RESTORE_NOT_ATTEMPTED, DTV_L3_OK};
}

dtv_l3_result product_transfer(void *, std::uint64_t,
                               const pix_l3_transfer *, std::uint8_t *,
                               std::size_t, pix_l3_transfer_result *) noexcept
{
    return DTV_L3_UNSUPPORTED;
}

dtv_l3_result product_cancel(void *, std::uint64_t) noexcept
{
    return DTV_L3_UNSUPPORTED;
}

dtv_l3_result product_bind(void *context, dtv_l3_context *owner) noexcept
{
    L3Dt181Runtime *runtime = static_cast<L3Dt181Runtime *>(context);
    if (runtime == nullptr || owner == nullptr) return DTV_L3_INVALID;
    runtime->bind_owner(owner);
    return DTV_L3_OK;
}

dtv_l3_result product_initialize(void *context, dtv_l3_context *) noexcept
{
    L3Dt181Runtime *runtime = static_cast<L3Dt181Runtime *>(context);
    return runtime != nullptr ? runtime->initialize_product() : DTV_L3_INVALID;
}

dtv_l3_result product_tune(void *context, dtv_l3_provider_binding *,
                           std::uint32_t receiver_id, std::uint64_t requested_hz,
                           std::uint64_t *programmed_hz) noexcept
{
    L3Dt181Runtime *runtime = static_cast<L3Dt181Runtime *>(context);
    return runtime != nullptr
               ? runtime->program_tune(receiver_id, requested_hz, programmed_hz)
               : DTV_L3_INVALID;
}

dtv_l3_result product_stream_attach(void *context, dtv_l3_context *owner,
                                    std::uint64_t, dtv_l3_ts_deliver_fn deliver,
                                    void *callback_context) noexcept
{
    L3Dt181Runtime *runtime = static_cast<L3Dt181Runtime *>(context);
    return runtime != nullptr
               ? runtime->start_producer(owner, deliver, callback_context)
               : DTV_L3_INVALID;
}

dtv_l3_result product_stream_stop(void *context, dtv_l3_context *,
                                  std::uint64_t) noexcept
{
    L3Dt181Runtime *runtime = static_cast<L3Dt181Runtime *>(context);
    return runtime != nullptr ? runtime->stop_producer() : DTV_L3_INVALID;
}

}  // namespace

L3Dt181Runtime::L3Dt181Runtime(dt181_session &session,
                               px4::userland::TunerServiceBackend &frontend,
                               std::string location,
                               const std::uint8_t *bundle,
                               std::size_t bundle_size,
                               const std::uint8_t *firmware,
                               const std::uint8_t *bootstrap,
                               const std::uint8_t *validation) noexcept
    : session_(session), frontend_(frontend), firmware_(bundle),
      asset_firmware_(firmware), firmware_size_(bundle_size), bootstrap_(bootstrap),
      validation_(validation), location_(std::move(location))
{
    native_product_.vendor_id = 0x06b8U;
    native_product_.product_id = 0x102bU;
    native_product_.product_context = this;
    native_product_.create = native_create;
    native_product_.release = native_release;
}

L3Dt181Runtime::~L3Dt181Runtime() noexcept
{
    (void)shutdown();
}

Result<void> L3Dt181Runtime::start() noexcept
{
    if (owner_ != nullptr) {
        return Result<void>::success();
    }
    if (firmware_ == nullptr || firmware_size_ == 0U || location_.empty() ||
        asset_firmware_ == nullptr || bootstrap_ == nullptr || validation_ == nullptr) {
        return Result<void>::failure(Error::INVALID_ARGUMENT);
    }
    pix_l3_factory_config config{};
    config.model = PIX_L3_MODEL_DT181;
    config.usb.cancel_scope = DTV_L3_CANCEL_SCOPE_TRANSFER;
    config.usb.cancel_supported = 1U;
    config.usb.claim = product_claim;
    config.usb.release_claim = product_release_claim;
    config.usb.transfer = product_transfer;
    config.usb.cancel = product_cancel;
    config.product.context = this;
    config.product.configure = product_configure;
    config.product.bind = product_bind;
    config.product.initialize = product_initialize;
    config.product.tune = product_tune;
    config.product.stream_attach = product_stream_attach;
    config.product.stream_stop = product_stream_stop;
    if (pix_l3_factory_create(&config, &factory_) != DTV_L3_OK ||
        factory_ == nullptr) {
        return Result<void>::failure(Error::INTERNAL);
    }
    const dtv_l3_public_open_args args = {
        sizeof(dtv_l3_public_open_args), &native_product_, location_.c_str(),
        {firmware_, firmware_size_, nullptr, 0U}, {}, 0U};
    if (dtv_l3_public_open(&args, &public_context_) !=
            DTV_L3_PUBLIC_ACCEPTED || public_context_ == nullptr) {
        return Result<void>::failure(Error::USB_IO);
    }
    if (dtv_l3_public_wait_ready(public_context_, 10000U) !=
        DTV_L3_PUBLIC_OK) {
        if (initialization_attempted_ &&
            dt181_session_shutdown(&session_) == 0) {
            initialized_ = false;
            initialization_attempted_ = false;
        }
        (void)close_owner();
        return Result<void>::failure(Error::USB_IO);
    }
    return Result<void>::success();
}

dtv_l3_result L3Dt181Runtime::native_create(
    void *context, libusb_device_handle *handle,
    const dtv_l3_firmware *firmware,
    const dtv_l3_product_adapter **adapter_out,
    dtv_l3_native_claim *claims, std::uint32_t claim_capacity,
    std::uint32_t *claim_count_out, void **adapter_owner_out)
{
    L3Dt181Runtime *runtime = static_cast<L3Dt181Runtime *>(context);
    if (runtime == nullptr || handle == nullptr || firmware == nullptr ||
        adapter_out == nullptr || claims == nullptr || claim_capacity < 1U ||
        claim_count_out == nullptr || adapter_owner_out == nullptr ||
        runtime->factory_ == nullptr) {
        return DTV_L3_INVALID;
    }
    *adapter_out = nullptr;
    *claim_count_out = 0U;
    *adapter_owner_out = nullptr;
    usb_backend *backend = nullptr;
    if (usb_wrap_libusb_handle(&backend, handle) != 0 || backend == nullptr ||
        dt181_session_bind(&runtime->session_, backend) != 0) {
        usb_close(backend);
        return DTV_L3_FAILED;
    }
    dtv_l3_adapter_factory factory{};
    if (pix_l3_factory_adapter_factory(runtime->factory_, &factory) != DTV_L3_OK) {
        dt181_session_close(&runtime->session_);
        return DTV_L3_FAILED;
    }
    const dtv_l3_open_config config = {
        PIX_L3_MODEL_DT181, 0U, firmware};
    void *adapter_owner = nullptr;
    const dtv_l3_result created = factory.create(
        factory.factory_context, &config, adapter_out, &adapter_owner);
    if (created != DTV_L3_OK || *adapter_out == nullptr ||
        adapter_owner == nullptr) {
        if (adapter_owner != nullptr && factory.release != nullptr)
            factory.release(adapter_owner);
        dt181_session_close(&runtime->session_);
        return created == DTV_L3_OK ? DTV_L3_FAILED : created;
    }
    NativeAdapterOwner *owner = new (std::nothrow) NativeAdapterOwner{
        runtime, factory.release, adapter_owner};
    if (owner == nullptr) {
        if (factory.release != nullptr) factory.release(adapter_owner);
        dt181_session_close(&runtime->session_);
        return DTV_L3_FAILED;
    }
    claims[0] = {0, -1};
    *claim_count_out = 1U;
    *adapter_owner_out = owner;
    return DTV_L3_OK;
}

void L3Dt181Runtime::native_release(void *adapter_owner) noexcept
{
    NativeAdapterOwner *owner = static_cast<NativeAdapterOwner *>(adapter_owner);
    if (owner == nullptr) return;
    if (owner->release != nullptr) owner->release(owner->adapter_owner);
    if (owner->runtime != nullptr && owner->runtime->session_.opened)
        dt181_session_close(&owner->runtime->session_);
    delete owner;
}

dtv_l3_result L3Dt181Runtime::initialize_product() noexcept
{
    initialization_attempted_ = true;
    if (dt181_session_initialize(&session_, asset_firmware_, bootstrap_, validation_) != 0)
        return DTV_L3_FAILED;
    initialized_ = true;
    return DTV_L3_OK;
}

void L3Dt181Runtime::bind_owner(dtv_l3_context *owner) noexcept
{
    owner_ = owner;
}

Result<void> L3Dt181Runtime::shutdown() noexcept
{
    reset_stream();
    if (initialization_attempted_) {
        if (dt181_session_shutdown(&session_) != 0)
            return Result<void>::failure(Error::USB_IO);
        initialized_ = false;
        initialization_attempted_ = false;
    }
    if (close_owner() != DTV_L3_PUBLIC_OK)
        return Result<void>::failure(Error::USB_IO);
    return Result<void>::success();
}

void L3Dt181Runtime::request_stop() noexcept
{
    producer_stop_.store(true);
}

std::uint8_t L3Dt181Runtime::receiver_count() const noexcept
{
    return frontend_.receiver_count();
}

bool L3Dt181Runtime::receiver_supports(std::uint8_t receiver,
                                       System system) const noexcept
{
    return frontend_.receiver_supports(receiver, system);
}

bool L3Dt181Runtime::selects_satellite_stream_before_tune() const noexcept
{
    return frontend_.selects_satellite_stream_before_tune();
}

bool L3Dt181Runtime::supports_internal_card_observation() const noexcept
{
    return frontend_.supports_internal_card_observation();
}

dtv::userland::wire::Optional<bool>
L3Dt181Runtime::sample_internal_card_inserted() noexcept
{
    return frontend_.sample_internal_card_inserted();
}

Result<void> L3Dt181Runtime::open_receiver(std::uint8_t receiver) noexcept
{
    const auto opened = frontend_.open_receiver(receiver);
    if (!opened) return opened;
    return ensure_lease();
}

Result<void> L3Dt181Runtime::tune_lease(std::uint64_t hz, std::uint64_t *programmed_hz) noexcept
{
    if (lease_ == nullptr) return Result<void>::failure(Error::NOT_READY);
    std::uint64_t programmed = 0U;
    std::uint64_t revision = 0U;
    const dtv_l3_result tuned = dtv_l3_lease_tune(lease_, hz, &programmed, &revision);
    if (tuned == DTV_L3_OK && programmed_hz != nullptr) {
        *programmed_hz = programmed;
    }
    return tuned == DTV_L3_OK
               ? Result<void>::success()
               : Result<void>::failure(Error::USB_IO);
}

dtv_l3_result L3Dt181Runtime::program_tune(
    std::uint32_t receiver, std::uint64_t requested_hz,
    std::uint64_t *programmed_hz) noexcept
{
    if (receiver != 0U || programmed_hz == nullptr) {
        return DTV_L3_INVALID;
    }
    const auto tuned = tune_system_ == System::ISDB_T
        ? frontend_.tune_terrestrial_hz(static_cast<std::uint8_t>(receiver),
                                        requested_hz, tune_timeout_ms_)
        : frontend_.tune_satellite_hz(static_cast<std::uint8_t>(receiver),
                                      requested_hz, tune_timeout_ms_);
    if (!tuned) {
        return DTV_L3_FAILED;
    }
    *programmed_hz = tuned.value().programmed_hz.value_or(requested_hz);
    return DTV_L3_OK;
}

Result<void> L3Dt181Runtime::tune_terrestrial(
    std::uint8_t receiver, std::uint32_t frequency_khz,
    std::uint32_t timeout_ms) noexcept
{
    if (receiver != 0U) return Result<void>::failure(Error::UNSUPPORTED);
    tune_system_ = System::ISDB_T;
    tune_timeout_ms_ = timeout_ms;
    return tune_lease(static_cast<std::uint64_t>(frequency_khz) * 1000U);
}

Result<void> L3Dt181Runtime::tune_satellite(std::uint8_t receiver,
                                            std::uint32_t frequency_khz,
                                            std::uint32_t timeout_ms) noexcept
{
    if (receiver != 0U) return Result<void>::failure(Error::UNSUPPORTED);
    tune_system_ = System::ISDB_S;
    tune_timeout_ms_ = timeout_ms;
    return tune_lease(static_cast<std::uint64_t>(frequency_khz) * 1000U);
}

Result<TunerHzProgramming> L3Dt181Runtime::tune_terrestrial_hz(
    std::uint8_t receiver, std::uint64_t frequency_hz,
    std::uint32_t timeout_ms) noexcept
{
    if (receiver != 0U) return Result<TunerHzProgramming>::failure(Error::UNSUPPORTED);
    tune_system_ = System::ISDB_T;
    tune_timeout_ms_ = timeout_ms;
    std::uint64_t programmed_hz = frequency_hz;
    const auto leased = tune_lease(frequency_hz, &programmed_hz);
    if (!leased) return Result<TunerHzProgramming>::failure(leased.error());
    TunerHzProgramming tuned{};
    tuned.programmed_hz = programmed_hz;
    return Result<TunerHzProgramming>::success(tuned);
}

Result<TunerHzProgramming> L3Dt181Runtime::tune_satellite_hz(
    std::uint8_t receiver, std::uint64_t frequency_hz,
    std::uint32_t timeout_ms) noexcept
{
    if (receiver != 0U) return Result<TunerHzProgramming>::failure(Error::UNSUPPORTED);
    tune_system_ = System::ISDB_S;
    tune_timeout_ms_ = timeout_ms;
    std::uint64_t programmed_hz = frequency_hz;
    const auto leased = tune_lease(frequency_hz, &programmed_hz);
    if (!leased) return Result<TunerHzProgramming>::failure(leased.error());
    TunerHzProgramming tuned{};
    tuned.programmed_hz = programmed_hz;
    return Result<TunerHzProgramming>::success(tuned);
}

TunerStrictProfile L3Dt181Runtime::strict_profile(std::uint8_t receiver,
                                                  System system) const noexcept
{
    return frontend_.strict_profile(receiver, system);
}

Result<bool> L3Dt181Runtime::is_locked(std::uint8_t receiver,
                                       System system) noexcept
{
    return frontend_.is_locked(receiver, system);
}

Result<void> L3Dt181Runtime::select_satellite_slot(
    std::uint8_t receiver, std::uint8_t slot, std::uint32_t timeout_ms) noexcept
{
    return frontend_.select_satellite_slot(receiver, slot, timeout_ms);
}

Result<void> L3Dt181Runtime::select_satellite_tsid(
    std::uint8_t receiver, std::uint16_t tsid, std::uint32_t timeout_ms) noexcept
{
    return frontend_.select_satellite_tsid(receiver, tsid, timeout_ms);
}

Result<void> L3Dt181Runtime::close_receiver(std::uint8_t receiver) noexcept
{
    /* The frontend pin is separate from the L3 lease lifetime. The lease is
     * released by stream detach/stop or runtime shutdown. */
    return frontend_.close_receiver(receiver);
}

Result<void> L3Dt181Runtime::begin_tune_power(std::uint8_t receiver,
                                              System system,
                                              std::uint8_t lnb_voltage) noexcept
{
    return frontend_.begin_tune_power(receiver, system, lnb_voltage);
}

Result<void> L3Dt181Runtime::commit_tune_power(std::uint8_t receiver) noexcept
{
    return frontend_.commit_tune_power(receiver);
}

Result<void> L3Dt181Runtime::rollback_tune_power(std::uint8_t receiver) noexcept
{
    return frontend_.rollback_tune_power(receiver);
}

Result<void> L3Dt181Runtime::set_independent_lnb_power(bool enabled) noexcept
{
    return frontend_.set_independent_lnb_power(enabled);
}

dtv::userland::wire::Optional<bool>
L3Dt181Runtime::independent_lnb_power_state() noexcept
{
    return frontend_.independent_lnb_power_state();
}

void L3Dt181Runtime::mark_receiver_disconnected(std::uint8_t receiver) noexcept
{
    frontend_.mark_receiver_disconnected(receiver);
}

Result<void> L3Dt181Runtime::start_capture(std::uint8_t receiver,
                                           System system) noexcept
{
    return frontend_.start_capture(receiver, system);
}

Result<void> L3Dt181Runtime::stop_capture(std::uint8_t receiver,
                                          System system) noexcept
{
    return frontend_.stop_capture(receiver, system);
}

Result<void> L3Dt181Runtime::attach(const TunerAttachment &attachment) noexcept
{
    if (attachment.receiver != 0U) {
        return Result<void>::failure(Error::INVALID_ARGUMENT);
    }
    if (stream_ != nullptr) {
        reset_stream();
    }
    const auto leased = ensure_lease();
    if (!leased) return leased;
    const dtv_l3_result started = dtv_l3_lease_start(lease_);
    if (started != DTV_L3_OK) {
        return Result<void>::failure(Error::USB_IO);
    }
    dtv_l3_stream *stream = nullptr;
    const dtv_l3_result attached = dtv_l3_lease_attach(lease_, &stream);
    if (attached != DTV_L3_OK || stream == nullptr) {
        return Result<void>::failure(Error::USB_IO);
    }
    stream_ = stream;
    return Result<void>::success();
}

Result<void> L3Dt181Runtime::detach(const TunerAttachment &) noexcept
{
    reset_stream();
    return Result<void>::success();
}

Result<TunerStreamCounters> L3Dt181Runtime::stats(
    const TunerAttachment &) const noexcept
{
    TunerStreamCounters counters{};
    counters.packets = ts_packets_;
    counters.bytes = ts_bytes_;
    if (stream_ != nullptr) {
        dtv_l3_stream_status status{};
        if (dtv_l3_stream_status_get(stream_, &status) == DTV_L3_OK) {
            counters.queue_drops = status.dropped_overflow_packets;
        }
    }
    for (auto &state : counters.availability) {
        state = dtv::userland::wire::MeasureState::valid;
    }
    return Result<TunerStreamCounters>::success(counters);
}

Result<TunerStreamFinalSnapshot> L3Dt181Runtime::final_snapshot(
    const TunerAttachment &) const noexcept
{
    TunerStreamFinalSnapshot snapshot{};
    const bool live = stream_ != nullptr || final_packets_ == 0U;
    snapshot.counters.packets = live ? ts_packets_ : final_packets_;
    snapshot.counters.bytes = live ? ts_bytes_ : final_bytes_;
    snapshot.counters.queue_drops = live ? 0U : final_queue_drops_;
    for (auto &state : snapshot.counters.availability) {
        state = dtv::userland::wire::MeasureState::valid;
    }
    snapshot.terminal = final_terminal_
                            ? static_cast<std::uint8_t>(TunerStreamTerminal::stopped)
                            : static_cast<std::uint8_t>(TunerStreamTerminal::none);
    return Result<TunerStreamFinalSnapshot>::success(snapshot);
}

Result<void> L3Dt181Runtime::release_final(const TunerAttachment &) noexcept
{
    return Result<void>::success();
}

Result<TunerStreamReadResult> L3Dt181Runtime::read(
    const TunerAttachment &, dtv::userland::MutableByteView output,
    dtv::userland::Timeout timeout) noexcept
{
    (void)timeout;
    TunerStreamReadResult result{};
    if (stream_ == nullptr) {
        result.eof = true;
        result.terminal = TunerStreamTerminal::stopped;
        return Result<TunerStreamReadResult>::success(result);
    }
    std::size_t bytes = 0U;
    dtv_l3_read_state state = DTV_L3_READ_WAIT;
    const dtv_l3_result read_result = dtv_l3_stream_read_ts_wait(
        stream_, output.data, output.size, &bytes, &state, 1000U);
    if (read_result == DTV_L3_WAIT_TIMEOUT || state == DTV_L3_READ_WAIT) {
        result.timed_out = true;
        return Result<TunerStreamReadResult>::success(result);
    }
    if (read_result == DTV_L3_CLOSING || state == DTV_L3_READ_EOF) {
        result.eof = true;
        result.terminal = TunerStreamTerminal::stopped;
        return Result<TunerStreamReadResult>::success(result);
    }
    if (read_result != DTV_L3_OK) {
        return Result<TunerStreamReadResult>::failure(Error::USB_IO);
    }
    result.bytes = bytes;
    return Result<TunerStreamReadResult>::success(result);
}

Result<TunerStreamTerminal> L3Dt181Runtime::terminal(
    const TunerAttachment &) const noexcept
{
    return Result<TunerStreamTerminal>::success(
        final_terminal_ ? TunerStreamTerminal::stopped
                        : TunerStreamTerminal::none);
}

dtv_l3_result L3Dt181Runtime::start_producer(dtv_l3_context *owner,
                                             dtv_l3_ts_deliver_fn deliver,
                                             void *callback_context) noexcept
{
    if (owner == nullptr || deliver == nullptr || owner_ == nullptr) {
        return DTV_L3_INVALID;
    }
    if (producer_started_) {
        return DTV_L3_OK;
    }
    std::array<std::uint8_t, 32U> context{};
    if (pixy_fifo_clear(&session_.device) != 0 ||
        pixy_start(&session_.device, context.data()) != 0) {
        return DTV_L3_FAILED;
    }
    deliver_ = deliver;
    deliver_context_ = callback_context;
    if (dt181_capture_host_start_feed(&feed_, context.data(),
                                      [](void *opaque, const std::uint8_t *data,
                                         std::size_t size) -> int {
                                          return static_cast<L3Dt181Runtime *>(opaque)
                                              ->bridge_deliver(data, size);
                                      },
                                      this) != 0) {
        feed_ = nullptr;
        (void)pixy_stop(&session_.device);
        return DTV_L3_FAILED;
    }
    producer_stop_.store(false);
    producer_ = std::thread(&L3Dt181Runtime::producer_loop, this, deliver,
                            callback_context);
    producer_started_ = true;
    return DTV_L3_OK;
}

dtv_l3_result L3Dt181Runtime::stop_producer() noexcept
{
    producer_stop_.store(true);
    if (producer_.joinable()) {
        producer_.join();
    }
    producer_started_ = false;
    if (feed_ != nullptr) {
        (void)dt181_capture_host_stop_feed(feed_);
        dt181_capture_host_destroy(feed_);
        feed_ = nullptr;
    }
    (void)pixy_stop(&session_.device);
    deliver_ = nullptr;
    deliver_context_ = nullptr;
    return DTV_L3_OK;
}

int L3Dt181Runtime::bridge_deliver(const std::uint8_t *data,
                                   std::size_t size) noexcept
{
    if (data == nullptr || size == 0U || deliver_ == nullptr) {
        return -1;
    }
    ts_packets_ += size / 188U;
    ts_bytes_ += size;
    deliver_(deliver_context_, data, size);
    return 0;
}

void L3Dt181Runtime::producer_loop(dtv_l3_ts_deliver_fn deliver,
                                   void *callback_context) noexcept
{
    (void)deliver;
    (void)callback_context;
    while (!producer_stop_.load()) {
        dtv_l3_transfer_request request{};
        request.kind = DTV_L3_TRANSFER_BULK;
        request.endpoint = 0x82U;
        request.timeout_ms = 100U;
        request.in_capacity = 32768U;
        request.claim_index = 0U;
        dtv_l3_op *op = nullptr;
        const dtv_l3_result started =
            dtv_l3_transfer_start(owner_, &request, &op);
        if (started != DTV_L3_ACCEPTED || op == nullptr) {
            break;
        }
        dtv_l3_op_result result{};
        while (!producer_stop_.load()) {
            (void)dtv_l3_op_wait(op, 100U);
            if (dtv_l3_op_poll(op, &result) != DTV_L3_OK) {
                break;
            }
            if (result.state == DTV_L3_OP_DONE ||
                result.state == DTV_L3_OP_FAILED) {
                break;
            }
        }
        if (producer_stop_.load()) {
            (void)dtv_l3_op_cancel(op);
            for (unsigned attempt = 0U; attempt < 20U; ++attempt) {
                (void)dtv_l3_op_wait(op, 100U);
                if (dtv_l3_op_poll(op, &result) != DTV_L3_OK ||
                    result.state == DTV_L3_OP_DONE ||
                    result.state == DTV_L3_OP_FAILED) {
                    break;
                }
            }
        }
        if (result.result == DTV_L3_OK && result.data != nullptr &&
            result.data_size != 0U && feed_ != nullptr) {
            (void)dt181_capture_host_feed(feed_, result.data, result.data_size);
        }
        (void)dtv_l3_op_release(op);
        if (producer_stop_.load()) {
            break;
        }
    }
}

Result<void> L3Dt181Runtime::ensure_lease() noexcept
{
    if (owner_ == nullptr) {
        return Result<void>::failure(Error::NOT_READY);
    }
    if (lease_ != nullptr) {
        return Result<void>::success();
    }
    const dtv_l3_result acquired = dtv_l3_lease_acquire(owner_, 0U, &lease_);
    return acquired == DTV_L3_OK
               ? Result<void>::success()
               : Result<void>::failure(Error::USB_IO);
}

void L3Dt181Runtime::reset_stream() noexcept
{
    if (stream_ == nullptr) {
        if (lease_ != nullptr) {
            (void)dtv_l3_lease_release(lease_);
            lease_ = nullptr;
        }
        return;
    }
    final_packets_ = ts_packets_;
    final_bytes_ = ts_bytes_;
    dtv_l3_stream_status status{};
    if (dtv_l3_stream_status_get(stream_, &status) == DTV_L3_OK) {
        final_queue_drops_ = status.dropped_overflow_packets;
    }
    (void)dtv_l3_stream_stop(stream_, 1000U);
    (void)dtv_l3_stream_release(stream_, DTV_L3_STREAM_RELEASE_DISCARD_UNREAD);
    for (unsigned attempt = 0U; attempt < 5000U; ++attempt) {
        const dtv_l3_result destroyed = dtv_l3_stream_destroy(stream_);
        if (destroyed == DTV_L3_OK || destroyed == DTV_L3_STALE) {
            stream_ = nullptr;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    final_terminal_ = true;
    if (lease_ != nullptr) {
        for (unsigned attempt = 0U; attempt < 5000U; ++attempt) {
            const dtv_l3_result released_lease = dtv_l3_lease_release(lease_);
            if (released_lease != DTV_L3_BUSY) {
                lease_ = nullptr;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
}

dtv_l3_public_result L3Dt181Runtime::close_owner() noexcept
{
    if (producer_started_ || feed_ != nullptr) {
        (void)stop_producer();
    }
    if (producer_.joinable()) {
        producer_stop_.store(true);
        producer_.join();
    }
    dtv_l3_public_result closed = DTV_L3_PUBLIC_OK;
    if (public_context_ != nullptr)
        closed = dtv_l3_public_close(public_context_, 5000U);
    if (closed != DTV_L3_PUBLIC_OK) return closed;
    public_context_ = nullptr;
    owner_ = nullptr;
    if (factory_ != nullptr) {
        pix_l3_factory_destroy(factory_);
        factory_ = nullptr;
    }
    return DTV_L3_PUBLIC_OK;
}

}  // namespace pix::runtime
