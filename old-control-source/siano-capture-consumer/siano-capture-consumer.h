/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef SIANO_CAPTURE_CONSUMER_H
#define SIANO_CAPTURE_CONSUMER_H

#include "dtv/userland/detail/packet_stream_session.h"
#include "siano-capture-host.h"

#include <chrono>
#include <memory>

namespace siano::capture {

class Consumer final {
public:
    using Attachment = dtv::userland::detail::Attachment;
    using ReadResult = dtv::userland::detail::StreamReadResult;

    Consumer() noexcept;
    ~Consumer() noexcept;
    Consumer(const Consumer&) = delete;
    Consumer& operator=(const Consumer&) = delete;

    dtv::userland::Result<void> start(void*, const Attachment&,
                                      std::size_t queue_packets) noexcept;
    dtv::userland::Result<void> stop() noexcept;
    dtv::userland::Result<ReadResult> read(
        const Attachment&, dtv::userland::MutableByteView,
        std::chrono::milliseconds timeout) noexcept;
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
        std::uint64_t generation = 0U;
#ifdef SIANO_CAPTURE_TEST_SEAMS
        void (*test_sink_gate)(void*) noexcept = nullptr;
        void* test_sink_context = nullptr;
#endif
    };

    struct Host;

    static int packet_sink(void*, const std::uint8_t*) noexcept;

    dtv::userland::detail::PacketStreamSession session_;
    std::unique_ptr<Run> run_;
    struct Host;
    std::unique_ptr<Host> host_;
};

}  // namespace siano::capture

#endif
