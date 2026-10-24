/* SPDX-License-Identifier: MIT */
#include "pix-dt181-consumer.h"

#include <cerrno>
#include <new>
#include <exception>

namespace pix::capture::dt181 {

Consumer::~Consumer() noexcept
{
    if (host_) {
        (void)stop();
        if (host_) std::terminate();
    }
}

dtv::userland::Result<void> Consumer::attach(const Attachment& identity,
                                              std::size_t queue_packets) noexcept
{
    if (run_ || host_) return dtv::userland::Result<void>::failure(dtv::userland::Error::BUSY);
    std::lock_guard<std::mutex> lock(session_.mutex);
    auto result = session_.begin_attach_locked(identity, queue_packets,
        identity.evict_oldest ? dtv::userland::detail::StreamQueuePolicy::evict_oldest
                              : dtv::userland::detail::StreamQueuePolicy::legacy);
    if (!result) return result;
    run_.reset(new (std::nothrow) Run{this, identity, session_.incarnation});
    if (!run_) {
        session_.abandon_attach_locked();
        return dtv::userland::Result<void>::failure(dtv::userland::Error::INTERNAL);
    }
    return dtv::userland::Result<void>::success();
}

dtv::userland::Result<void> Consumer::start(void* session,
                                             const Attachment& identity,
                                             std::size_t queue_packets) noexcept
{
    auto attached = attach(identity, queue_packets);
    if (!attached) return attached;
    if (dt181_capture_host_start(&host_, session, decoded_sink, run_.get()) != 0) {
        std::lock_guard<std::mutex> lock(session_.mutex);
        (void)session_.abandon_attach_locked();
        run_.reset();
        return dtv::userland::Result<void>::failure(dtv::userland::Error::INTERNAL);
    }
    return dtv::userland::Result<void>::success();
}

dtv::userland::Result<void> Consumer::poll(unsigned milliseconds) noexcept
{
    return host_ && dt181_capture_host_poll(host_, milliseconds) == 0
        ? dtv::userland::Result<void>::success()
        : dtv::userland::Result<void>::failure(dtv::userland::Error::DISCONNECTED);
}

dtv::userland::Result<void> Consumer::certify() noexcept
{
    return host_ && dt181_capture_host_certify(host_) == 0
        ? dtv::userland::Result<void>::success()
        : dtv::userland::Result<void>::failure(dtv::userland::Error::DISCONNECTED);
}

dtv::userland::Result<void> Consumer::stop() noexcept
{
    if (!host_ || !run_)
        return dtv::userland::Result<void>::failure(dtv::userland::Error::NOT_FOUND);
    const Attachment identity = run_->identity;
    const int rc = dt181_capture_host_stop(host_);
    if (!dt181_capture_host_quiesced(host_))
        return dtv::userland::Result<void>::failure(dtv::userland::Error::INTERNAL);
    dt181_capture_host_destroy(host_);
    host_ = nullptr;
    run_.reset();
    std::lock_guard<std::mutex> lock(session_.mutex);
    auto detached = session_.detach_locked(identity);
    if (!detached) return detached;
    if (rc != 0 && session_.final_snapshot_valid &&
        session_.final_snapshot.terminal == static_cast<std::uint8_t>(
            dtv::userland::detail::StreamTerminal::stopped))
        session_.amend_final_terminal_locked(
            identity, dtv::userland::detail::StreamTerminal::bridge_fatal);
    return rc == 0 ? dtv::userland::Result<void>::success()
                   : dtv::userland::Result<void>::failure(dtv::userland::Error::INTERNAL);
}

void* Consumer::sink_context() noexcept
{
    return run_.get();
}

int Consumer::decoded_sink(void* opaque, const std::uint8_t* bytes,
                           std::size_t length) noexcept
{
    auto* run = static_cast<Run*>(opaque);
    if (!run || !run->owner || !bytes || length == 0U ||
        length % dtv::userland::detail::PacketStreamSession::packet_size != 0U)
        return -EINVAL;
    auto& session = run->owner->session_;
    std::lock_guard<std::mutex> lock(session.mutex);
    if (!session.attached || session.incarnation != run->incarnation ||
        !dtv::userland::detail::PacketStreamSession::same_identity(
            session.identity, run->identity))
#ifdef _WIN32
        return -ECANCELED;
#else
        return -ESTALE;
#endif
    for (std::size_t offset = 0U; offset < length; offset += 188U) {
        session.append_locked(dtv::userland::ByteView{bytes + offset, 188U});
        if (session.terminal != dtv::userland::detail::StreamTerminal::none)
            return -ENOBUFS;
    }
    return 0;
}

dtv::userland::Result<Consumer::ReadResult> Consumer::read(
    const Attachment& identity, dtv::userland::MutableByteView output,
    std::chrono::milliseconds timeout) noexcept
{
    std::unique_lock<std::mutex> lock(session_.mutex);
    auto ticket = session_.begin_read_locked(identity);
    if (!ticket) return dtv::userland::Result<ReadResult>::failure(ticket.error());
    if (!session_.read_wake_locked(ticket.value()))
        session_.changed.wait_for(lock, timeout, [&] {
            return session_.read_wake_locked(ticket.value());
        });
    return session_.finish_read_locked(identity, ticket.value(), output);
}

dtv::userland::Result<void> Consumer::detach_after_worker_join(
    const Attachment& identity) noexcept
{
    std::lock_guard<std::mutex> lock(session_.mutex);
    return session_.detach_locked(identity);
}

dtv::userland::Result<dtv::userland::detail::StreamCounters> Consumer::stats(
    const Attachment& identity) const noexcept
{
    return session_.stats(identity);
}

dtv::userland::Result<dtv::userland::detail::StreamFinalSnapshot>
Consumer::final_snapshot(const Attachment& identity) const noexcept
{
    return session_.final_snapshot_query(identity);
}

dtv::userland::Result<void> Consumer::release_final(const Attachment& identity) noexcept
{
    return session_.release_final(identity);
}

}  // namespace pix::capture::dt181
