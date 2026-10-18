/* SPDX-License-Identifier: MIT */
#ifndef PIX_CAPTURE_CONSUMER_H
#define PIX_CAPTURE_CONSUMER_H

#include "dtv/userland/detail/packet_stream_session.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>

struct dt295_capture_host;

namespace pix::capture {

class Consumer;
struct ConsumerOwner;

class RetainedOwner final {
public:
    RetainedOwner() noexcept = default;
    RetainedOwner(const RetainedOwner&) = delete;
    RetainedOwner& operator=(const RetainedOwner&) = delete;
    RetainedOwner(RetainedOwner&&) noexcept = default;
    RetainedOwner& operator=(RetainedOwner&&) noexcept = default;

    // Call stop until quiesced() is true, then call release(). Keep this handle
    // alive while native callbacks or the worker may still reference its owner.
    // The enclosing daemon decides whether a reported stop failure requires
    // process-level shutdown; this library does not make that decision.
    dtv::userland::Result<void> stop() noexcept;
    bool quiesced() const noexcept;
    dtv::userland::Result<dtv::userland::detail::StreamFinalSnapshot> final_snapshot(
        const dtv::userland::detail::Attachment&) const noexcept;
    dtv::userland::Result<void> release() noexcept;

private:
    friend class Consumer;
    explicit RetainedOwner(std::shared_ptr<ConsumerOwner>) noexcept;
    std::shared_ptr<ConsumerOwner> owner_;
};

class Consumer final {
public:
    using Attachment = dtv::userland::detail::Attachment;
    using ReadResult = dtv::userland::detail::StreamReadResult;

    Consumer() noexcept = default;
    ~Consumer() noexcept;
    Consumer(const Consumer&) = delete;
    Consumer& operator=(const Consumer&) = delete;

    // Serialize calls on one Consumer or RetainedOwner at the caller. The
    // callback/queue synchronization is internal; this does not make the
    // public object safe for concurrent lifecycle calls. The caller must
    // serialize lifecycle operations and coordinate daemon shutdown.
    dtv::userland::Result<void> attach(const Attachment&, std::size_t queue_packets) noexcept;
    dtv::userland::Result<void> start(const void*, const char*, const Attachment&,
                                      std::size_t queue_packets) noexcept;
    dtv::userland::Result<void> stop() noexcept;
    // Transfer an unquiesced owner to the caller before destroying this wrapper
    // when stop cannot complete. The caller retries stop and releases only
    // after quiesced() is true. If the wrapper is destroyed without retaining
    // the handle, the owner self-retains safely but cannot be recovered here.
    dtv::userland::Result<RetainedOwner> take_retained_owner() noexcept;
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
    std::shared_ptr<ConsumerOwner> owner_;
    bool retired_identity_valid_ = false;
    Attachment retired_identity_{};
};

}  // namespace pix::capture

#endif
