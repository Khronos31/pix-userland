/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "pix-capture-consumer.h"

#include "pix_capture_consumer_internal.h"

extern "C" {
#include "dt295_capture_host.h"
}

#include <cerrno>
#include <new>

namespace pix::capture {

bool ConsumerOwner::quiesced() const noexcept
{
    return !host || dt295_capture_host_quiesced(host);
}

dtv::userland::Result<void> ConsumerOwner::stop() noexcept
{
    if (!host || !run)
        return dtv::userland::Result<void>::failure(dtv::userland::Error::NOT_FOUND);
    const dtv::userland::detail::Attachment identity = run->identity;
    const int rc = dt295_capture_host_stop(host);
    if (!dt295_capture_host_quiesced(host))
        return dtv::userland::Result<void>::failure(dtv::userland::Error::INTERNAL);
    dt295_capture_host_destroy(host);
    host = nullptr;
    run.reset();
    {
        std::lock_guard<std::mutex> lock(session.mutex);
        auto detached = session.detach_locked(identity);
        if (!detached)
            return detached;
        if (rc != 0 && session.final_snapshot_valid &&
            session.final_snapshot.terminal == static_cast<std::uint8_t>(
                dtv::userland::detail::StreamTerminal::stopped))
            session.amend_final_terminal_locked(
                identity, dtv::userland::detail::StreamTerminal::bridge_fatal);
    }
    keep_alive.reset();
    return rc == 0 ? dtv::userland::Result<void>::success()
                   : dtv::userland::Result<void>::failure(dtv::userland::Error::INTERNAL);
}

RetainedOwner::RetainedOwner(std::shared_ptr<ConsumerOwner> owner) noexcept
    : owner_(std::move(owner))
{
}

dtv::userland::Result<void> RetainedOwner::stop() noexcept
{
    return owner_ ? owner_->stop()
                  : dtv::userland::Result<void>::failure(dtv::userland::Error::NOT_FOUND);
}

bool RetainedOwner::quiesced() const noexcept
{
    return owner_ && owner_->quiesced();
}

dtv::userland::Result<dtv::userland::detail::StreamFinalSnapshot>
RetainedOwner::final_snapshot(
    const dtv::userland::detail::Attachment& identity) const noexcept
{
    return owner_ ? owner_->session.final_snapshot_query(identity)
                  : dtv::userland::Result<dtv::userland::detail::StreamFinalSnapshot>::failure(
                        dtv::userland::Error::NOT_FOUND);
}

dtv::userland::Result<void> RetainedOwner::release() noexcept
{
    if (!owner_)
        return dtv::userland::Result<void>::failure(dtv::userland::Error::NOT_FOUND);
    if (!owner_->quiesced() || owner_->host || owner_->run)
        return dtv::userland::Result<void>::failure(dtv::userland::Error::INTERNAL);
    owner_->keep_alive.reset();
    owner_.reset();
    return dtv::userland::Result<void>::success();
}

Consumer::~Consumer() noexcept
{
    if (owner_ && owner_->host)
        (void)owner_->stop();
}

dtv::userland::Result<void> Consumer::attach(const Attachment& identity,
                                              std::size_t queue_packets) noexcept
{
    if (retired_identity_valid_ &&
        dtv::userland::detail::PacketStreamSession::same_identity(
            retired_identity_, identity))
        return dtv::userland::Result<void>::failure(dtv::userland::Error::BUSY);
    if (!owner_) {
        try {
            owner_ = std::make_shared<ConsumerOwner>();
        } catch (...) {
            return dtv::userland::Result<void>::failure(dtv::userland::Error::INTERNAL);
        }
    }
    if (owner_->run || owner_->host)
        return dtv::userland::Result<void>::failure(dtv::userland::Error::BUSY);
    std::lock_guard<std::mutex> lock(owner_->session.mutex);
    auto result = owner_->session.begin_attach_locked(identity, queue_packets);
    if (!result)
        return result;
    owner_->run.reset(new (std::nothrow) ConsumerOwner::Run{
        owner_.get(), identity, owner_->session.incarnation});
    if (!owner_->run) {
        owner_->session.abandon_attach_locked();
        return dtv::userland::Result<void>::failure(dtv::userland::Error::INTERNAL);
    }
    return dtv::userland::Result<void>::success();
}

dtv::userland::Result<void> Consumer::start(const void* configuration,
                                             const char* runtime_directory,
                                             const Attachment& identity,
                                             std::size_t queue_packets) noexcept
{
    auto attached = attach(identity, queue_packets);
    if (!attached)
        return attached;
    owner_->keep_alive = owner_;
    if (dt295_capture_host_start(&owner_->host, configuration, runtime_directory,
                                 decoded_sink, owner_->run.get()) != 0) {
        if (owner_->host && !dt295_capture_host_quiesced(owner_->host))
            return dtv::userland::Result<void>::failure(dtv::userland::Error::INTERNAL);
        if (owner_->host) {
            dt295_capture_host_destroy(owner_->host);
            owner_->host = nullptr;
        }
        std::lock_guard<std::mutex> lock(owner_->session.mutex);
        (void)owner_->session.abandon_attach_locked();
        owner_->run.reset();
        owner_->keep_alive.reset();
        return dtv::userland::Result<void>::failure(dtv::userland::Error::INTERNAL);
    }
    return dtv::userland::Result<void>::success();
}

dtv::userland::Result<void> Consumer::stop() noexcept
{
    return owner_ ? owner_->stop()
                  : dtv::userland::Result<void>::failure(dtv::userland::Error::NOT_FOUND);
}

dtv::userland::Result<RetainedOwner> Consumer::take_retained_owner() noexcept
{
    if (!owner_ || !owner_->host || !owner_->run || owner_->quiesced())
        return dtv::userland::Result<RetainedOwner>::failure(dtv::userland::Error::NOT_FOUND);
    retired_identity_ = owner_->run->identity;
    retired_identity_valid_ = true;
    RetainedOwner retained(std::move(owner_));
    return dtv::userland::Result<RetainedOwner>::success(std::move(retained));
}

void* Consumer::sink_context() noexcept
{
    return owner_ && owner_->run ? owner_->run.get() : nullptr;
}

int Consumer::decoded_sink(void* opaque, const std::uint8_t* bytes,
                           std::size_t length) noexcept
{
    auto* run = static_cast<ConsumerOwner::Run*>(opaque);
    if (!run || !run->owner || !bytes || length == 0U ||
        length % dtv::userland::detail::PacketStreamSession::packet_size != 0U)
        return -EINVAL;
    auto& session = run->owner->session;
    std::lock_guard<std::mutex> lock(session.mutex);
    if (!session.attached || session.incarnation != run->incarnation ||
        !dtv::userland::detail::PacketStreamSession::same_identity(
            session.identity, run->identity))
        return -ESTALE;
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
    if (!owner_)
        return dtv::userland::Result<ReadResult>::failure(dtv::userland::Error::NOT_FOUND);
    std::unique_lock<std::mutex> lock(owner_->session.mutex);
    auto ticket = owner_->session.begin_read_locked(identity);
    if (!ticket)
        return dtv::userland::Result<ReadResult>::failure(ticket.error());
    if (!owner_->session.read_wake_locked(ticket.value()))
        owner_->session.changed.wait_for(lock, timeout, [&] {
            return owner_->session.read_wake_locked(ticket.value());
        });
    return owner_->session.finish_read_locked(identity, ticket.value(), output);
}

dtv::userland::Result<void> Consumer::detach_after_worker_join(
    const Attachment& identity) noexcept
{
    if (!owner_)
        return dtv::userland::Result<void>::failure(dtv::userland::Error::NOT_FOUND);
    std::lock_guard<std::mutex> lock(owner_->session.mutex);
    return owner_->session.detach_locked(identity);
}

dtv::userland::Result<dtv::userland::detail::StreamCounters> Consumer::stats(
    const Attachment& identity) const noexcept
{
    return owner_ ? owner_->session.stats(identity)
                  : dtv::userland::Result<dtv::userland::detail::StreamCounters>::failure(
                        dtv::userland::Error::NOT_FOUND);
}

dtv::userland::Result<dtv::userland::detail::StreamFinalSnapshot> Consumer::final_snapshot(
    const Attachment& identity) const noexcept
{
    return owner_ ? owner_->session.final_snapshot_query(identity)
                  : dtv::userland::Result<dtv::userland::detail::StreamFinalSnapshot>::failure(
                        dtv::userland::Error::NOT_FOUND);
}

dtv::userland::Result<void> Consumer::release_final(const Attachment& identity) noexcept
{
    return owner_ ? owner_->session.release_final(identity)
                  : dtv::userland::Result<void>::failure(dtv::userland::Error::NOT_FOUND);
}

}  // namespace pix::capture
