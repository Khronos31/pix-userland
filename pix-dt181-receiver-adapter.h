/* SPDX-License-Identifier: MIT */
// Product-local glue for the existing PIX DT181 C session and capture host.
#ifndef PIX_DT181_RECEIVER_ADAPTER_H
#define PIX_DT181_RECEIVER_ADAPTER_H

#include "dtv/userland/detail/receiver_engine.h"
#include "pix-dt181-consumer.h"
extern "C" {
#include "dt181_session.h"
}

#include <atomic>
#include <chrono>
#include <cstdint>
#include <pthread.h>

namespace pix::capture::dt181 {

class ReceiverAdapter final : public dtv::userland::detail::ReceiverBackend,
                              public dtv::userland::detail::ReceiverStreamControl {
public:
    using Engine = dtv::userland::detail::ReceiverEngine;
    using Clock = dtv::userland::detail::ReceiverClock;
    using Result = dtv::userland::Result<void>;
    using ReceiverId = dtv::userland::detail::ReceiverId;
    using ReceiverSystem = dtv::userland::detail::ReceiverSystem;
    using Attachment = dtv::userland::detail::Attachment;

    ReceiverAdapter(dt181_session&, Consumer&, Clock&, ReceiverId,
                    std::size_t queue_packets) noexcept;
    ~ReceiverAdapter() noexcept override;
    ReceiverAdapter(const ReceiverAdapter&) = delete;
    ReceiverAdapter& operator=(const ReceiverAdapter&) = delete;

    static Result configure(Engine&, ReceiverId, std::uint64_t epoch) noexcept;
    bool supports(ReceiverId, ReceiverSystem) const noexcept override;
    bool selects_satellite_stream_before_tune() const noexcept override;
    std::uint32_t lock_settle_ms(ReceiverId, ReceiverSystem) const noexcept override;
    Result open(ReceiverId) noexcept override;
    Result tune(ReceiverId, ReceiverSystem, std::uint32_t, std::uint32_t) noexcept override;
    dtv::userland::Result<dtv::userland::detail::TuneProgramming> program_hz(
        ReceiverId, ReceiverSystem, std::uint64_t, std::uint32_t) noexcept override;
    dtv::userland::Result<bool> is_locked(ReceiverId, ReceiverSystem) noexcept override;
    Result select_satellite_slot(ReceiverId, std::uint8_t, std::uint32_t) noexcept override;
    Result select_satellite_tsid(ReceiverId, std::uint16_t, std::uint32_t) noexcept override;
    Result close(ReceiverId) noexcept override;
    Result begin_tune_power(ReceiverId, ReceiverSystem, std::uint8_t) noexcept override;
    Result commit_tune_power(ReceiverId) noexcept override;
    Result rollback_tune_power(ReceiverId) noexcept override;
    void mark_disconnected(ReceiverId) noexcept override;
    Result shutdown() noexcept override;
    Result start_capture(ReceiverId, ReceiverSystem) noexcept override;
    Result stop_capture(ReceiverId, ReceiverSystem) noexcept override;
    Result attach(const Attachment&) noexcept override;
    Result detach(const Attachment&) noexcept override;
    dtv::userland::Result<dtv::userland::detail::StreamCounters> stats(
        const Attachment&) const noexcept override;
    dtv::userland::Result<dtv::userland::detail::StreamFinalSnapshot> final_snapshot(
        const Attachment&) const noexcept override;
    dtv::userland::Result<Consumer::ReadResult> read(
        const Attachment&, dtv::userland::MutableByteView,
        std::chrono::milliseconds) noexcept;
    Result release_final(const Attachment&) noexcept override;

private:
    bool matches(ReceiverId) const noexcept;
    bool has_session() const noexcept;
    void pump() noexcept;
    static void* pump_entry(void*) noexcept;
    Result stop_attached(const Attachment&) noexcept;

    dt181_session& session_;
    Consumer& consumer_;
    Clock& clock_;
    ReceiverId receiver_;
    std::size_t queue_packets_;
    bool opened_ = false;
    bool stream_attached_ = false;
    bool satellite_selection_valid_ = false;
    int satellite_tsid_ = -1;
    int satellite_slot_ = -1;
    Attachment attachment_{};
    std::atomic<bool> pump_stop_{false};
    std::atomic<int> pump_error_{0};
    pthread_t pump_thread_{};
    bool pump_joinable_ = false;
};

}  // namespace pix::capture::dt181

#endif
