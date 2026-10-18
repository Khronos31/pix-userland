/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "siano-capture-consumer.h"
extern "C" {
}

#include <cerrno>
#include <cstring>
#include <new>
#include <exception>

namespace siano::capture {

struct Consumer::Host final {
    siano_capture_host* handle = nullptr;
};

Consumer::Consumer() noexcept = default;

Consumer::~Consumer() noexcept
{
    if (host_ && !stop()) std::terminate();
}

dtv::userland::Result<void> Consumer::start(
    void* device, const Attachment& identity,
    std::size_t queue_packets) noexcept
{
    if (!device || host_)
        return dtv::userland::Result<void>::failure(dtv::userland::Error::INVALID_ARGUMENT);
    host_.reset(new (std::nothrow) Host{});
    if (!host_)
        return dtv::userland::Result<void>::failure(dtv::userland::Error::INTERNAL);
    std::uint64_t generation = 0U;
    int host_rc = 0;
    {
        std::lock_guard<std::mutex> lock(session_.mutex);
        auto attached = session_.begin_attach_locked(identity, queue_packets);
        if (!attached) { host_.reset(); return attached; }
        run_.reset(new (std::nothrow) Run{this, identity, session_.incarnation, generation});
        if (!run_) {
            session_.abandon_attach_locked();
            host_.reset();
            return dtv::userland::Result<void>::failure(dtv::userland::Error::INTERNAL);
        }
        host_rc = siano_capture_host_start(&host_->handle, device, packet_sink, run_.get(), &generation);
        if (host_rc != 0) {
            run_.reset();
            session_.abandon_attach_locked();
            host_.reset();
            const auto error = host_rc == -EOVERFLOW ? dtv::userland::Error::RESOURCE_LIMIT
                              : host_rc == -EALREADY ? dtv::userland::Error::BUSY
                              : host_rc == -EINVAL ? dtv::userland::Error::INVALID_ARGUMENT
                                                     : dtv::userland::Error::INTERNAL;
            return dtv::userland::Result<void>::failure(error);
        }
        run_->generation = generation;
    }
    return dtv::userland::Result<void>::success();
}

dtv::userland::Result<void> Consumer::stop() noexcept
{
    if (!host_ || !run_)
        return dtv::userland::Result<void>::failure(dtv::userland::Error::NOT_FOUND);
    const int stop_rc = siano_capture_host_stop(host_->handle);
    if (!siano_capture_host_quiesced(host_->handle))
        return dtv::userland::Result<void>::failure(dtv::userland::Error::INTERNAL);
    siano_capture_host_destroy(host_->handle);
    host_.reset();
    const Attachment identity = run_->identity;
    run_.reset();
    std::lock_guard<std::mutex> lock(session_.mutex);
    auto detached = session_.detach_locked(identity);
    if (!detached) return detached;
    if (stop_rc != 0 && session_.final_snapshot_valid &&
        session_.final_snapshot.terminal == static_cast<std::uint8_t>(
            dtv::userland::detail::StreamTerminal::stopped))
        session_.amend_final_terminal_locked(
            identity, dtv::userland::detail::StreamTerminal::bridge_fatal);
    return stop_rc == 0 ? dtv::userland::Result<void>::success()
                        : dtv::userland::Result<void>::failure(dtv::userland::Error::INTERNAL);
}

int Consumer::packet_sink(void* opaque,
                          const std::uint8_t* packet) noexcept
{
    auto* run = static_cast<Run*>(opaque);
    if (!run || !run->owner || !packet) return -EINVAL;
#ifdef SIANO_CAPTURE_TEST_SEAMS
    if (run->test_sink_gate) run->test_sink_gate(run->test_sink_context);
#endif
    auto& session = run->owner->session_;
    std::lock_guard<std::mutex> lock(session.mutex);
    if (!session.attached || session.incarnation != run->incarnation ||
        !dtv::userland::detail::PacketStreamSession::same_identity(
            session.identity, run->identity))
        return -ESTALE;
    session.append_locked(dtv::userland::ByteView{
        packet, dtv::userland::detail::PacketStreamSession::packet_size});
    return session.terminal == dtv::userland::detail::StreamTerminal::none ? 0 : -ENOBUFS;
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

}  // namespace siano::capture
