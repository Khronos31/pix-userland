// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef PIX_L3_DT181_RUNTIME_H
#define PIX_L3_DT181_RUNTIME_H

#include "pix_l3_adapter.h"
#include "pix_l3_dt181_backend.h"

extern "C" {
#include "dt181_capture_host.h"
#include "dt181_session.h"
}

#include <dtv/userland/host_daemon/tuner_service.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <thread>

namespace pix::runtime {

/*
 * Build-time DT181 runtime for the standard pixd assembly. The existing
 * TunerServiceBackend keeps the product frontend/setup/tune semantics; the L3
 * owner owns the stream lease, transfer/cancel, terminal completion and holder
 * recovery. This is internal to the product and adds no public CLI/API.
 */
class L3Dt181Runtime final : public px4::userland::TunerServiceBackend,
                             public px4::userland::TunerStreamControl {
public:
    L3Dt181Runtime(dt181_session &session,
                   px4::userland::TunerServiceBackend &frontend,
                   const std::uint8_t *firmware,
                   std::size_t firmware_size) noexcept;
    ~L3Dt181Runtime() noexcept override;

    L3Dt181Runtime(const L3Dt181Runtime &) = delete;
    L3Dt181Runtime &operator=(const L3Dt181Runtime &) = delete;

    px4::userland::Result<void> start() noexcept;
    px4::userland::Result<void> shutdown() noexcept override;
    void request_stop() noexcept override;

    std::uint8_t receiver_count() const noexcept override;
    bool receiver_supports(std::uint8_t receiver,
                           px4::userland::ipc::System system) const noexcept override;
    bool selects_satellite_stream_before_tune() const noexcept override;
    bool supports_internal_card_observation() const noexcept override;
    dtv::userland::wire::Optional<bool> sample_internal_card_inserted() noexcept override;
    px4::userland::Result<void> open_receiver(std::uint8_t receiver) noexcept override;
    px4::userland::Result<void> tune_terrestrial(
        std::uint8_t receiver, std::uint32_t frequency_khz,
        std::uint32_t timeout_ms) noexcept override;
    px4::userland::Result<void> tune_satellite(
        std::uint8_t receiver, std::uint32_t frequency_khz,
        std::uint32_t timeout_ms) noexcept override;
    px4::userland::Result<px4::userland::TunerHzProgramming>
    tune_terrestrial_hz(std::uint8_t receiver, std::uint64_t frequency_hz,
                        std::uint32_t timeout_ms) noexcept override;
    px4::userland::Result<px4::userland::TunerHzProgramming>
    tune_satellite_hz(std::uint8_t receiver, std::uint64_t frequency_hz,
                      std::uint32_t timeout_ms) noexcept override;
    px4::userland::TunerStrictProfile strict_profile(
        std::uint8_t receiver,
        px4::userland::ipc::System system) const noexcept override;
    px4::userland::Result<bool> is_locked(
        std::uint8_t receiver,
        px4::userland::ipc::System system) noexcept override;
    px4::userland::Result<void> select_satellite_slot(
        std::uint8_t receiver, std::uint8_t slot,
        std::uint32_t timeout_ms) noexcept override;
    px4::userland::Result<void> select_satellite_tsid(
        std::uint8_t receiver, std::uint16_t tsid,
        std::uint32_t timeout_ms) noexcept override;
    px4::userland::Result<void> close_receiver(std::uint8_t receiver) noexcept override;
    px4::userland::Result<void> begin_tune_power(
        std::uint8_t receiver, px4::userland::ipc::System system,
        std::uint8_t lnb_voltage) noexcept override;
    px4::userland::Result<void> commit_tune_power(
        std::uint8_t receiver) noexcept override;
    px4::userland::Result<void> rollback_tune_power(
        std::uint8_t receiver) noexcept override;
    px4::userland::Result<void> set_independent_lnb_power(
        bool enabled) noexcept override;
    dtv::userland::wire::Optional<bool> independent_lnb_power_state() noexcept override;
    void mark_receiver_disconnected(std::uint8_t receiver) noexcept override;
    px4::userland::Result<void> start_capture(
        std::uint8_t receiver,
        px4::userland::ipc::System system) noexcept override;
    px4::userland::Result<void> stop_capture(
        std::uint8_t receiver,
        px4::userland::ipc::System system) noexcept override;

    px4::userland::Result<void> attach(
        const px4::userland::TunerAttachment &attachment) noexcept override;
    px4::userland::Result<void> detach(
        const px4::userland::TunerAttachment &attachment) noexcept override;
    px4::userland::Result<px4::userland::TunerStreamCounters> stats(
        const px4::userland::TunerAttachment &attachment) const noexcept override;
    px4::userland::Result<px4::userland::TunerStreamFinalSnapshot> final_snapshot(
        const px4::userland::TunerAttachment &attachment) const noexcept override;
    px4::userland::Result<void> release_final(
        const px4::userland::TunerAttachment &attachment) noexcept override;
    px4::userland::Result<px4::userland::TunerStreamReadResult> read(
        const px4::userland::TunerAttachment &attachment,
        dtv::userland::MutableByteView output,
        dtv::userland::Timeout timeout) noexcept override;
    px4::userland::Result<px4::userland::TunerStreamTerminal> terminal(
        const px4::userland::TunerAttachment &attachment) const noexcept override;

    /* Internal product callbacks used by the L3 product adapter. */
    dtv_l3_result start_producer(dtv_l3_context *owner,
                                 dtv_l3_ts_deliver_fn deliver,
                                 void *callback_context) noexcept;
    dtv_l3_result stop_producer() noexcept;
    int bridge_deliver(const std::uint8_t *data, std::size_t size) noexcept;
    dtv_l3_result program_tune(std::uint32_t receiver,
                               std::uint64_t requested_hz,
                               std::uint64_t *programmed_hz) noexcept;

private:
    void producer_loop(dtv_l3_ts_deliver_fn deliver,
                       void *callback_context) noexcept;
    px4::userland::Result<void> ensure_lease() noexcept;
    px4::userland::Result<void> tune_lease(
        std::uint64_t hz, std::uint64_t *programmed_hz = nullptr) noexcept;
    void reset_stream() noexcept;
    void close_owner() noexcept;

    dt181_session &session_;
    px4::userland::TunerServiceBackend &frontend_;
    const std::uint8_t *firmware_ = nullptr;
    std::size_t firmware_size_ = 0U;

    pix_l3_dt181_backend *adapter_ = nullptr;
    pix_l3_factory *factory_ = nullptr;
    dtv_l3_context *owner_ = nullptr;
    dtv_l3_lease *lease_ = nullptr;
    dtv_l3_stream *stream_ = nullptr;

    std::atomic<bool> producer_stop_{false};
    std::thread producer_;
    bool producer_started_ = false;
    dt181_capture_host *feed_ = nullptr;
    dtv_l3_ts_deliver_fn deliver_ = nullptr;
    void *deliver_context_ = nullptr;

    std::uint64_t ts_packets_ = 0U;
    std::uint64_t ts_bytes_ = 0U;
    std::uint64_t final_packets_ = 0U;
    std::uint64_t final_bytes_ = 0U;
    std::uint64_t final_queue_drops_ = 0U;
    bool final_terminal_ = false;
    px4::userland::ipc::System tune_system_ =
        px4::userland::ipc::System::ISDB_T;
    std::uint32_t tune_timeout_ms_ = 5000U;
};

}  // namespace pix::runtime

#endif
