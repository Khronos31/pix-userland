/* SPDX-License-Identifier: MIT */
#ifndef PIX_CAPTURE_CONSUMER_H
#define PIX_CAPTURE_CONSUMER_H

#include "dtv/userland/detail/packet_stream_session.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>

extern "C" {
#include "dt181_capture_host.h"
}

namespace pix::capture {

class Consumer final {
public:
    using Attachment = dtv::userland::detail::Attachment;
    using ReadResult = dtv::userland::detail::StreamReadResult;

    Consumer() noexcept = default;
    ~Consumer() noexcept;
    Consumer(const Consumer&) = delete;
    Consumer& operator=(const Consumer&) = delete;

    dtv::userland::Result<void> attach(const Attachment&, std::size_t queue_packets) noexcept;
    dtv::userland::Result<void> start(void*, const Attachment&,
                                      std::size_t queue_packets) noexcept;
    dtv::userland::Result<void> poll(unsigned milliseconds) noexcept;
    dtv::userland::Result<void> certify() noexcept;
    dtv::userland::Result<void> stop() noexcept;
    void* sink_context() noexcept;
    static int decoded_sink(void*, const std::uint8_t*, std::size_t) noexcept;
    dtv::userland::Result<ReadResult> read(
        const Attachment&, dtv::userland::MutableByteView,
        std::chrono::milliseconds timeout) noexcept;
    dtv::userland::Result<void> detach_after_worker_join(const Attachment&) noexcept;
    dtv::userland::Result<dtv::userland::detail::StreamCounters> stats(
        const Attachment&) const noexcept;
    dtv::userland::Result<dtv::userland::detail::StreamFinalSnapshot> final_snapshot(
        const Attachment&) const noexcept;
    dtv::userland::Result<void> release_final(const Attachment&) noexcept;

private:
    friend struct ConsumerTestAccess;

    struct Run final {
        Consumer* owner = nullptr;
        Attachment identity{};
        std::uint64_t incarnation = 0U;
    };

    dtv::userland::detail::PacketStreamSession session_;
    std::unique_ptr<Run> run_;
    dt181_capture_host* host_ = nullptr;
};

}  // namespace pix::capture

#endif
