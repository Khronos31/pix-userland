/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "pix-dt181-receiver-adapter.h"

#include <cerrno>
#include <limits>

namespace pix::capture::dt181 {
namespace {

using dtv::userland::Error;

ReceiverAdapter::Result result(Error error) noexcept
{
    return ReceiverAdapter::Result::failure(error);
}

Error device_error(int code) noexcept
{
    switch (code) {
    case 2: return Error::INVALID_ARGUMENT;
    case 5: return Error::TIMEOUT;
    case 7: return Error::USB_IO;
    case 9: return Error::NO_CARD;
    default: return Error::INTERNAL;
    }
}

pixy_system to_pixy_system(ReceiverAdapter::ReceiverSystem system) noexcept
{
    return system == ReceiverAdapter::ReceiverSystem::terrestrial
        ? PIXY_TERRESTRIAL : PIXY_BS;
}

}  // namespace

ReceiverAdapter::ReceiverAdapter(dt181_session& session, Consumer& consumer,
                                 Clock& clock, ReceiverId receiver,
                                 std::size_t queue_packets) noexcept
    : session_(session), consumer_(consumer), clock_(clock), receiver_(receiver),
      queue_packets_(queue_packets)
{
}

ReceiverAdapter::~ReceiverAdapter() noexcept
{
    if (stream_attached_) (void)stop_attached(attachment_);
}

ReceiverAdapter::Result ReceiverAdapter::configure(Engine& engine, ReceiverId receiver,
                                                    std::uint64_t epoch) noexcept
{
    using namespace dtv::userland::detail;
    const ReceiverCapability capability{receiver, true, true, true,
                                        TuneReadiness::terminal_lock, true};
    auto strict = engine.configure_strict_capabilities(&capability, 1U);
    if (!strict) return strict;
    return engine.configure_stream_contract(StreamContract::v1, epoch);
}

bool ReceiverAdapter::matches(ReceiverId receiver) const noexcept
{
    return receiver == receiver_;
}

bool ReceiverAdapter::has_session() const noexcept
{
    return session_.opened != 0 && session_.usb != nullptr;
}

bool ReceiverAdapter::supports(ReceiverId receiver, ReceiverSystem system) const noexcept
{
    return matches(receiver) && (system == ReceiverSystem::terrestrial ||
                                 system == ReceiverSystem::satellite);
}

bool ReceiverAdapter::selects_satellite_stream_before_tune() const noexcept { return true; }
std::uint32_t ReceiverAdapter::lock_settle_ms(ReceiverId, ReceiverSystem) const noexcept { return 0U; }

ReceiverAdapter::Result ReceiverAdapter::open(ReceiverId receiver) noexcept
{
    if (!matches(receiver)) return result(Error::INVALID_ARGUMENT);
    if (!has_session()) return result(Error::DISCONNECTED);
    if (opened_) return result(Error::BUSY);
    opened_ = true;
    return Result::success();
}

ReceiverAdapter::Result ReceiverAdapter::tune(ReceiverId, ReceiverSystem,
                                               std::uint32_t,
                                               std::uint32_t) noexcept
{
    return result(Error::UNSUPPORTED);
}

dtv::userland::Result<dtv::userland::detail::TuneProgramming>
ReceiverAdapter::program_hz(ReceiverId receiver, ReceiverSystem system,
                            std::uint64_t requested_hz,
                            std::uint32_t) noexcept
{
    using namespace dtv::userland;
    using namespace dtv::userland::detail;
    using TuneResult = dtv::userland::Result<TuneProgramming>;
    if (!supports(receiver, system))
        return TuneResult::failure(Error::UNSUPPORTED);
    if (!opened_ || !has_session())
        return TuneResult::failure(Error::NOT_FOUND);
    if (requested_hz == 0U || requested_hz > std::numeric_limits<std::uint32_t>::max())
        return TuneResult::failure(Error::INVALID_ARGUMENT);

    int tsid = system == ReceiverSystem::satellite && satellite_selection_valid_
        ? satellite_tsid_ : -1;
    const int slot = system == ReceiverSystem::satellite && satellite_selection_valid_
        ? satellite_slot_ : -1;
    satellite_selection_valid_ = false;
    satellite_tsid_ = -1;
    satellite_slot_ = -1;

    const int lna = system == ReceiverSystem::terrestrial ? 0 : 1;
    const int rc = dt181_session_tune_slot(&session_, to_pixy_system(system),
        static_cast<std::uint32_t>(requested_hz), lna, &tsid, slot);
    if (rc != 0)
        return TuneResult::failure(device_error(dt181_session_device_exit(&session_)));

    std::uint8_t status[64]{};
    std::size_t status_length = sizeof(status);
    if (dt181_session_status(&session_, status, &status_length) != 0 || status_length < 4U)
        return TuneResult::failure(device_error(dt181_session_device_exit(&session_)));
    if (status[3] == 0U)
        return TuneResult::failure(Error::NO_CARD);

    TuneProgramming programming{};
    const std::uint64_t sampled_ms = clock_.monotonic_ms();
    if (sampled_ms != 0U && sampled_ms <= std::numeric_limits<std::uint64_t>::max() / 1000000U) {
        programming.lock.state = wire::MeasureState::valid;
        programming.lock.value = true;
        programming.lock.sampled_at_ns = sampled_ms * 1000000U;
    }
    return TuneResult::success(programming);
}

dtv::userland::Result<bool> ReceiverAdapter::is_locked(ReceiverId, ReceiverSystem) noexcept
{
    return dtv::userland::Result<bool>::failure(Error::UNSUPPORTED);
}

ReceiverAdapter::Result ReceiverAdapter::select_satellite_slot(
    ReceiverId receiver, std::uint8_t slot, std::uint32_t) noexcept
{
    if (!matches(receiver) || slot > 7U) return result(Error::INVALID_ARGUMENT);
    satellite_selection_valid_ = true;
    satellite_tsid_ = -1;
    satellite_slot_ = static_cast<int>(slot);
    return Result::success();
}

ReceiverAdapter::Result ReceiverAdapter::select_satellite_tsid(
    ReceiverId receiver, std::uint16_t tsid, std::uint32_t) noexcept
{
    if (!matches(receiver)) return result(Error::INVALID_ARGUMENT);
    satellite_selection_valid_ = true;
    satellite_tsid_ = tsid == 0xffffU ? -1 : static_cast<int>(tsid);
    satellite_slot_ = -1;
    return Result::success();
}

ReceiverAdapter::Result ReceiverAdapter::close(ReceiverId receiver) noexcept
{
    if (!matches(receiver)) return result(Error::INVALID_ARGUMENT);
    if (stream_attached_) return result(Error::BUSY);
    opened_ = false;
    satellite_selection_valid_ = false;
    return Result::success();
}

ReceiverAdapter::Result ReceiverAdapter::begin_tune_power(
    ReceiverId receiver, ReceiverSystem system, std::uint8_t voltage) noexcept
{
    if (!supports(receiver, system)) return result(Error::UNSUPPORTED);
    return voltage == 0U ? Result::success() : result(Error::UNSUPPORTED);
}
ReceiverAdapter::Result ReceiverAdapter::commit_tune_power(ReceiverId receiver) noexcept
{ return matches(receiver) ? Result::success() : result(Error::INVALID_ARGUMENT); }
ReceiverAdapter::Result ReceiverAdapter::rollback_tune_power(ReceiverId receiver) noexcept
{ return matches(receiver) ? Result::success() : result(Error::INVALID_ARGUMENT); }
void ReceiverAdapter::mark_disconnected(ReceiverId) noexcept {}
ReceiverAdapter::Result ReceiverAdapter::shutdown() noexcept
{ return stream_attached_ ? result(Error::BUSY) : Result::success(); }

ReceiverAdapter::Result ReceiverAdapter::start_capture(ReceiverId receiver,
                                                        ReceiverSystem system) noexcept
{
    return supports(receiver, system) && opened_ && has_session()
        ? Result::success() : result(Error::NOT_FOUND);
}

ReceiverAdapter::Result ReceiverAdapter::stop_capture(ReceiverId receiver,
                                                       ReceiverSystem system) noexcept
{
    if (!supports(receiver, system)) return result(Error::INVALID_ARGUMENT);
    return stream_attached_ ? stop_attached(attachment_) : Result::success();
}

void ReceiverAdapter::pump() noexcept
{
    std::uint64_t last_certify = clock_.monotonic_ms();
    while (!pump_stop_.load(std::memory_order_acquire)) {
        auto polled = consumer_.poll(50U);
        if (!polled) {
            pump_error_.store(static_cast<int>(polled.error()), std::memory_order_release);
            break;
        }
        const std::uint64_t now = clock_.monotonic_ms();
        if (now >= last_certify && now - last_certify >= 1000U) {
            auto certified = consumer_.certify();
            if (!certified) {
                pump_error_.store(static_cast<int>(certified.error()), std::memory_order_release);
                break;
            }
            last_certify = now;
        }
    }
}

void* ReceiverAdapter::pump_entry(void* opaque) noexcept
{
    static_cast<ReceiverAdapter*>(opaque)->pump();
    return nullptr;
}

ReceiverAdapter::Result ReceiverAdapter::attach(const Attachment& attachment) noexcept
{
    if (!opened_ || !has_session() || stream_attached_)
        return result(Error::BUSY);
    auto started = consumer_.start(&session_, attachment, queue_packets_);
    if (!started) return started;
    attachment_ = attachment;
    pump_stop_.store(false, std::memory_order_release);
    pump_error_.store(0, std::memory_order_release);
    stream_attached_ = true;
    if (pthread_create(&pump_thread_, nullptr, pump_entry, this) != 0) {
        pump_stop_.store(true, std::memory_order_release);
        auto stopped = consumer_.stop();
        if (!consumer_.active()) stream_attached_ = false;
        return !stopped ? stopped : result(Error::INTERNAL);
    }
    pump_joinable_ = true;
    return Result::success();
}

ReceiverAdapter::Result ReceiverAdapter::stop_attached(const Attachment& attachment) noexcept
{
    if (!stream_attached_) return Result::success();
    if (attachment.attachment_id != attachment_.attachment_id ||
        attachment.lease_id != attachment_.lease_id ||
        attachment.owner_client_id != attachment_.owner_client_id ||
        attachment.receiver != attachment_.receiver ||
        attachment.system != attachment_.system ||
        attachment.token != attachment_.token)
        return result(Error::NOT_FOUND);
    pump_stop_.store(true, std::memory_order_release);
    if (pump_joinable_) {
        if (pthread_join(pump_thread_, nullptr) != 0) return result(Error::INTERNAL);
        pump_joinable_ = false;
    }
    auto stopped = consumer_.stop();
    if (!consumer_.active()) stream_attached_ = false;
    if (!consumer_.active()) {
        const int pump_error = pump_error_.load(std::memory_order_acquire);
        if (pump_error != 0) return result(static_cast<Error>(pump_error));
    }
    if (!stopped && stopped.error() == Error::NOT_FOUND) return Result::success();
    return stopped;
}

ReceiverAdapter::Result ReceiverAdapter::detach(const Attachment& attachment) noexcept
{
    return stop_attached(attachment);
}

dtv::userland::Result<dtv::userland::detail::StreamCounters> ReceiverAdapter::stats(
    const Attachment& attachment) const noexcept
{ return consumer_.stats(attachment); }
dtv::userland::Result<dtv::userland::detail::StreamFinalSnapshot>
ReceiverAdapter::final_snapshot(const Attachment& attachment) const noexcept
{ return consumer_.final_snapshot(attachment); }
dtv::userland::Result<Consumer::ReadResult> ReceiverAdapter::read(
    const Attachment& attachment, dtv::userland::MutableByteView bytes,
    std::chrono::milliseconds timeout) noexcept
{ return consumer_.read(attachment, bytes, timeout); }
ReceiverAdapter::Result ReceiverAdapter::release_final(const Attachment& attachment) noexcept
{ return consumer_.release_final(attachment); }

}  // namespace pix::capture::dt181
