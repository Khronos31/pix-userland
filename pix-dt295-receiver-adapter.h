/* SPDX-License-Identifier: MIT */
#ifndef PIX_DT295_RECEIVER_ADAPTER_H
#define PIX_DT295_RECEIVER_ADAPTER_H

#include "dtv/userland/detail/packet_stream_session.h"
#include "dtv/userland/detail/receiver_engine.h"

extern "C" {
#include "dt295.h"
#include "dt295_routes.h"
#include "dt295_stream.h"
#include "usb_backend.h"
}

#include <atomic>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <pthread.h>
#include <mutex>
#include <optional>
#include <vector>

namespace pix::capture::dt295 {

class ReceiverAdapter final : public dtv::userland::detail::ReceiverBackend,
                              public dtv::userland::detail::ReceiverStreamControl {
public:
    using Engine = dtv::userland::detail::ReceiverEngine;
    using Clock = dtv::userland::detail::ReceiverClock;
    using Result = dtv::userland::Result<void>;
    using ReceiverId = dtv::userland::detail::ReceiverId;
    using ReceiverSystem = dtv::userland::detail::ReceiverSystem;
    using Attachment = dtv::userland::detail::Attachment;
    using ReadResult = dtv::userland::detail::StreamReadResult;

    enum class AcquisitionPhase : std::uint8_t { idle = 0U, preparing, ready, terminal };
    enum class AcquisitionFailure : std::uint8_t {
        none = 0U,
        deadline,
        unsupported,
        plan,
        multiple_ecm,
        satellite_tsid,
        usb,
        packet,
        disconnected
    };
    struct AcquisitionSnapshot final {
        AcquisitionPhase phase = AcquisitionPhase::idle;
        AcquisitionFailure failure = AcquisitionFailure::none;
        std::uint64_t tune_generation = 0U;
        std::uint64_t acquisition_generation = 0U;
        std::uint64_t plan_generation = 0U;
    };
    AcquisitionSnapshot acquisition() const noexcept;

    struct ScanLease final {
        std::uint64_t tune_generation = 0U;
        std::uint64_t scan_id = 0U;
    };
    enum class SiEndReason : std::uint8_t {
        none = 0U,
        scan_end,
        retune,
        stop,
        shutdown,
        disconnected,
        usb,
        transport,
        stale
    };
    struct SiReadResult final {
        std::size_t bytes = 0U;
        bool timed_out = false;
        bool terminal = false;
        bool stale_generation = false;
        std::uint64_t dropped_packets = 0U;
        std::uint64_t continuity_epoch = 0U;
        SiEndReason reason = SiEndReason::none;
    };
    Result begin_scan(ScanLease* out) noexcept;
    Result read_scan(const ScanLease&, dtv::userland::MutableByteView,
                     std::chrono::milliseconds, SiReadResult*) noexcept;
    Result end_scan(const ScanLease&) noexcept;

    ReceiverAdapter(const std::uint8_t* assets, std::size_t assets_size,
                    const usb_selector&, Clock&, ReceiverId,
                    std::size_t queue_packets, int device_fd = -1,
                    std::size_t si_queue_packets = 1024U) noexcept;
    ~ReceiverAdapter() noexcept override;
    ReceiverAdapter(const ReceiverAdapter&) = delete;
    ReceiverAdapter& operator=(const ReceiverAdapter&) = delete;

    bool ready() const noexcept { return init_error_ == dtv::userland::Error::OK; }
    dtv::userland::Error init_error() const noexcept { return init_error_; }
    const char* init_detail() const noexcept { return init_detail_.data(); }
    bool opened() const noexcept { return opened_; }

    static Result configure(Engine&, ReceiverId, std::uint64_t epoch) noexcept;
    bool supports(ReceiverId, ReceiverSystem) const noexcept override;
    bool selects_satellite_stream_before_tune() const noexcept override;
    std::uint32_t lock_settle_ms(ReceiverId, ReceiverSystem) const noexcept override;
    Result open(ReceiverId) noexcept override;
    Result tune(ReceiverId, ReceiverSystem, std::uint32_t, std::uint32_t) noexcept override;
    dtv::userland::Result<dtv::userland::detail::TuneProgramming> program_hz(
        ReceiverId, ReceiverSystem, std::uint64_t, std::uint32_t) noexcept override;
    dtv::userland::Result<bool> is_locked(ReceiverId, ReceiverSystem) noexcept override;
    std::optional<bool> sample_card_inserted() noexcept;
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
    dtv::userland::Result<ReadResult> read(
        const Attachment&, dtv::userland::MutableByteView,
        std::chrono::milliseconds) noexcept;
    Result release_final(const Attachment&) noexcept override;

private:
    struct Io final {
        ReceiverAdapter* owner = nullptr;
        std::uint64_t last_sequence = 0U;
        std::uint64_t last_completed_ns = 0U;
    };

    static int control(void*, std::uint8_t, std::uint8_t, std::uint16_t, std::uint16_t,
                       std::uint8_t*, std::size_t, unsigned, std::size_t*) noexcept;
    static int bulk(void*, std::uint8_t, std::uint8_t*, std::size_t, unsigned,
                    std::size_t*) noexcept;
    static int interrupt(void*, std::uint8_t, std::uint8_t*, std::size_t, unsigned,
                         std::size_t*) noexcept;
    static std::uint64_t now_ms(void*) noexcept;
    static void sleep_ms(void*, unsigned) noexcept;

    bool matches(ReceiverId) const noexcept;
    void initialize(const std::uint8_t* assets, std::size_t assets_size,
                    const usb_selector&, int device_fd) noexcept;
    void initialize_device(const usb_selector&, int device_fd) noexcept;
    Result halt_unlocked() noexcept;
    Result start_stream_unlocked() noexcept;
    Result stop_receiver_unlocked() noexcept;
    Result apply_plan_unlocked(dt295_route_plan* current,
                               const dt295_route_plan* desired) noexcept;
    Result ensure_streaming() noexcept;
    void pump() noexcept;
    static void* pump_entry(void*) noexcept;
    Result stop_attached(const Attachment&) noexcept;
    void fail_acquisition(AcquisitionFailure, dtv::userland::detail::StreamTerminal,
                          dtv::userland::Error) noexcept;
    void end_session_for_acquisition(dtv::userland::detail::StreamTerminal) noexcept;
    void scan_append_packet(const std::uint8_t packet[188]) noexcept;
    void scan_terminate(SiEndReason reason) noexcept;
    Result scan_release_owner(SiEndReason reason) noexcept;

    Clock& clock_;
    ReceiverId receiver_;
    std::size_t queue_packets_;
    Io io_{};
    usb_backend* usb_ = nullptr;
    dt295_assets* assets_ = nullptr;
    ::dt295* device_ = nullptr;
    dt295_routes* routes_ = nullptr;
    dt295_stream* stream_ = nullptr;
    dt295_route_plan live_{};
    dtv::userland::Error init_error_ = dtv::userland::Error::INTERNAL;
    std::array<char, 256U> init_detail_{};
    bool opened_ = false;
    bool recovery_failed_ = false;
    bool streaming_ = false;
    bool plan_complete_ = false;
    bool key_retained_ = false;
    bool satellite_tuned_ = false;
    std::atomic<bool> satellite_tsid_valid_{false};
    std::atomic<std::uint16_t> satellite_tsid_{0U};
    std::atomic<ReceiverSystem> tuned_system_{ReceiverSystem::terrestrial};
    bool stream_attached_ = false;
    bool session_ended_ = false;
    Attachment attachment_{};
    std::uint64_t attachment_incarnation_ = 0U;
    std::uint64_t acquisition_started_ms_ = 0U;
    std::atomic<AcquisitionPhase> acquisition_phase_{AcquisitionPhase::idle};
    std::atomic<AcquisitionFailure> acquisition_failure_{AcquisitionFailure::none};
    std::atomic<std::uint64_t> tune_generation_{0U};
    std::atomic<std::uint64_t> acquisition_generation_{0U};
    std::atomic<std::uint64_t> plan_generation_{0U};
    dtv::userland::detail::PacketStreamSession session_;
    bool normal_capture_active_ = false;
    std::size_t unconsumed_normal_refusals_ = 0U;
    bool pump_scan_ = false;
    std::atomic<std::uint64_t> scan_id_next_{0U};
    std::mutex operation_mutex_;
    std::mutex protocol_mutex_;
    mutable std::mutex acquisition_view_mutex_;
    std::mutex si_mutex_;
    std::condition_variable si_changed_;
    std::vector<std::uint8_t> si_queue_;
    std::size_t si_capacity_ = 1U;
    std::size_t si_head_ = 0U;
    std::size_t si_count_ = 0U;
    bool scan_active_ = false;
    std::uint64_t scan_owner_ = 0U;
    bool si_terminal_ = false;
    SiEndReason si_reason_ = SiEndReason::none;
    std::uint64_t scan_id_ = 0U;
    std::uint64_t scan_tune_generation_ = 0U;
    std::uint64_t si_dropped_ = 0U;
    std::uint64_t si_epoch_ = 0U;
    std::atomic<bool> pump_stop_{false};
    std::atomic<int> pump_error_{0};
    pthread_t pump_thread_{};
    bool pump_joinable_ = false;
};

}  // namespace pix::capture::dt295

#endif
