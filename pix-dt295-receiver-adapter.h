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

    ReceiverAdapter(const char* assets_directory, const usb_selector&, Clock&,
                    ReceiverId, std::size_t queue_packets, int device_fd = -1) noexcept;
    ReceiverAdapter(const std::uint8_t* assets, std::size_t assets_size,
                    const usb_selector&, Clock&, ReceiverId,
                    std::size_t queue_packets, int device_fd = -1) noexcept;
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
    Result release_final(const Attachment&) noexcept;

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
    void initialize(const char* assets_directory, const usb_selector&, int device_fd) noexcept;
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
    void usb_stage(const char* stage) noexcept;
    void usb_failure(const char* operation, unsigned endpoint, int result,
                     dtv::userland::Error mapped) noexcept;

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
    bool satellite_tsid_valid_ = false;
    std::uint16_t satellite_tsid_ = 0U;
    bool stream_attached_ = false;
    Attachment attachment_{};
    std::uint64_t attachment_incarnation_ = 0U;
    dtv::userland::detail::PacketStreamSession session_;
    std::mutex operation_mutex_;
    std::mutex protocol_mutex_;
    std::atomic<bool> pump_stop_{false};
    std::atomic<int> pump_error_{0};
    pthread_t pump_thread_{};
    bool pump_joinable_ = false;
    bool usb_diag_enabled_ = false;
    std::atomic<bool> usb_diag_recorded_{false};
    std::atomic<const char*> usb_diag_stage_{nullptr};
};

}  // namespace pix::capture::dt295

#endif
