// SPDX-License-Identifier: MIT
#include "dtv/userland/detail/packet_stream_session.h"

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <mutex>
#include <thread>
#include <vector>

namespace {

using namespace dtv::userland;
using namespace dtv::userland::detail;

#define SESSION_CHECK(condition)                                                  \
    do {                                                                          \
        if (!(condition)) {                                                       \
            std::fprintf(stderr, "packet session check failed at line %d: %s\n",  \
                         __LINE__, #condition);                                   \
            return false;                                                         \
        }                                                                         \
    } while (false)

constexpr std::chrono::milliseconds kWaitBound{10000};

Attachment identity(std::uint64_t attachment_id, std::uint16_t receiver = 1U)
{
    Attachment value{};
    value.owner_client_id = 7U;
    value.lease_id = 3U;
    value.attachment_id = attachment_id;
    value.receiver = receiver;
    value.system = ReceiverSystem::terrestrial;
    value.token[0] = static_cast<std::uint8_t>(attachment_id);
    return value;
}

std::array<std::uint8_t, PacketStreamSession::packet_size> packet(std::uint8_t fill)
{
    std::array<std::uint8_t, PacketStreamSession::packet_size> value{};
    value[0] = 0x47U;
    value[1] = fill;
    for (std::size_t index = 2U; index < value.size(); ++index)
        value[index] = static_cast<std::uint8_t>(index + fill);
    return value;
}

ByteView view(const std::array<std::uint8_t, PacketStreamSession::packet_size>& value)
{
    return ByteView{value.data(), value.size()};
}

// Fieldwise counter equality.  A refusal must not change any of the eight
// accounting fields, so the test compares each field rather than only packets.
bool same_counters(const StreamCounters& left, const StreamCounters& right)
{
    return left.packets == right.packets && left.bytes == right.bytes &&
           left.sync_errors == right.sync_errors && left.tei_packets == right.tei_packets &&
           left.continuity_errors == right.continuity_errors &&
           left.queue_drops == right.queue_drops && left.usb_errors == right.usb_errors &&
           left.empty_intervals == right.empty_intervals;
}

bool test_attach_detach_and_queries()
{
    PacketStreamSession session;
    const Attachment first = identity(11U);
    {
        std::lock_guard<std::mutex> lock(session.mutex);
        SESSION_CHECK(session.begin_attach_locked(first, 4U));
        SESSION_CHECK(session.incarnation == 1U);
        SESSION_CHECK(session.attached && !session.draining);
    }
    // A second attach on an active session is BUSY and does not advance the
    // incarnation.
    {
        std::lock_guard<std::mutex> lock(session.mutex);
        const auto busy = session.begin_attach_locked(identity(12U), 4U);
        SESSION_CHECK(!busy && busy.error() == Error::BUSY);
        SESSION_CHECK(session.incarnation == 1U);
    }
    const auto counters = session.stats(first);
    SESSION_CHECK(counters && counters.value().packets == 0U);
    const auto missing = session.stats(identity(12U));
    SESSION_CHECK(!missing && missing.error() == Error::NOT_FOUND);

    const auto detach = [&]() {
        std::lock_guard<std::mutex> lock(session.mutex);
        return session.detach_locked(first);
    }();
    SESSION_CHECK(detach);
    SESSION_CHECK(session.incarnation == 1U);
    SESSION_CHECK(!session.attached && session.final_snapshot_valid);
    const auto final = session.final_snapshot_query(first);
    SESSION_CHECK(final && final.value().terminal ==
                             static_cast<std::uint8_t>(StreamTerminal::stopped));
    // A stale identity can never release or read the retained final.
    const auto stale = session.final_snapshot_query(identity(12U));
    SESSION_CHECK(!stale && stale.error() == Error::NOT_FOUND);
    SESSION_CHECK(session.release_final(first));
    return true;
}

bool test_append_overflow_accounting_and_wrap()
{
    PacketStreamSession session;
    const Attachment attached = identity(21U);
    {
        std::lock_guard<std::mutex> lock(session.mutex);
        SESSION_CHECK(session.begin_attach_locked(attached, 3U));
    }
    const auto a = packet(0x01U);
    const auto b = packet(0x02U);
    const auto c = packet(0x03U);
    {
        std::lock_guard<std::mutex> lock(session.mutex);
        session.append_locked(view(a));
        session.append_locked(view(b));
        session.append_locked(view(c));
        SESSION_CHECK(session.count == 3U);
        SESSION_CHECK(session.counters.packets == 3U);
        SESSION_CHECK(session.counters.bytes == 3U * PacketStreamSession::packet_size);
        // Accounting precedes the full-ring rejection, and the terminal is set
        // once without a second counter cache.
        session.append_locked(view(a));
        SESSION_CHECK(session.counters.packets == 4U);
        SESSION_CHECK(session.counters.queue_drops == 1U);
        SESSION_CHECK(session.count == 3U);
        SESSION_CHECK(session.terminal == StreamTerminal::slow_consumer);
    }
    return true;
}

bool test_two_span_wrap_read()
{
    PacketStreamSession session;
    const Attachment attached = identity(22U);
    constexpr std::size_t kCapacity = 5U;
    {
        std::lock_guard<std::mutex> lock(session.mutex);
        SESSION_CHECK(session.begin_attach_locked(attached, kCapacity));
    }
    const auto p0 = packet(0x10U);
    const auto p1 = packet(0x11U);
    const auto p2 = packet(0x12U);
    const auto p3 = packet(0x13U);
    const auto p4 = packet(0x14U);
    {
        std::lock_guard<std::mutex> lock(session.mutex);
        session.append_locked(view(p0));
        session.append_locked(view(p1));
        session.append_locked(view(p2));
        session.append_locked(view(p3));
        session.append_locked(view(p4));
        SESSION_CHECK(session.head == 0U && session.count == kCapacity);
    }
    // Advance head by two so a later append has to cross the physical end.
    std::array<std::uint8_t, 2U * PacketStreamSession::packet_size> front{};
    {
        std::unique_lock<std::mutex> lock(session.mutex);
        const auto ticket = session.begin_read_locked(attached);
        SESSION_CHECK(ticket);
        const auto read = session.finish_read_locked(
            attached, ticket.value(), MutableByteView{front.data(), front.size()});
        SESSION_CHECK(read && read.value().bytes == front.size());
        SESSION_CHECK(session.head == 2U && session.count == kCapacity - 2U);
    }
    // The front read must return the two consumed packets byte-for-byte.
    SESSION_CHECK(std::memcmp(front.data(), p0.data(), PacketStreamSession::packet_size) == 0);
    SESSION_CHECK(std::memcmp(front.data() + PacketStreamSession::packet_size, p1.data(),
                              PacketStreamSession::packet_size) == 0);
    // Both appends land ahead of the current head: the first at physical index
    // 0 and the second at index 1, so the ring wraps past its physical end.
    const auto p5 = packet(0x15U);
    const auto p6 = packet(0x16U);
    {
        std::lock_guard<std::mutex> lock(session.mutex);
        session.append_locked(view(p5));
        session.append_locked(view(p6));
        SESSION_CHECK(session.head == 2U && session.count == kCapacity);
        SESSION_CHECK(session.counters.packets == 7U);
        SESSION_CHECK(session.counters.bytes == 7U * PacketStreamSession::packet_size);
        SESSION_CHECK(session.counters.queue_drops == 0U);
    }
    // This single read spans the physical end, so it exercises both the direct
    // and the wrapped copy.  Compare every complete 188-byte packet in order.
    std::array<std::uint8_t, kCapacity * PacketStreamSession::packet_size> output{};
    {
        std::unique_lock<std::mutex> lock(session.mutex);
        const auto ticket = session.begin_read_locked(attached);
        SESSION_CHECK(ticket);
        const auto read = session.finish_read_locked(
            attached, ticket.value(), MutableByteView{output.data(), output.size()});
        SESSION_CHECK(read && read.value().bytes == output.size());
        SESSION_CHECK(session.head == 2U && session.count == 0U);
    }
    const std::array<std::array<std::uint8_t, PacketStreamSession::packet_size>, kCapacity>
        expected{p2, p3, p4, p5, p6};
    for (std::size_t index = 0U; index < expected.size(); ++index) {
        SESSION_CHECK(std::memcmp(output.data() + index * PacketStreamSession::packet_size,
                                  expected[index].data(),
                                  PacketStreamSession::packet_size) == 0);
    }
    return true;
}

bool test_read_round_trip_and_eof()
{
    PacketStreamSession session;
    const Attachment attached = identity(31U);
    {
        std::lock_guard<std::mutex> lock(session.mutex);
        SESSION_CHECK(session.begin_attach_locked(attached, 4U));
        session.append_locked(view(packet(0x0aU)));
        session.append_locked(view(packet(0x0bU)));
    }
    std::array<std::uint8_t, 4U * PacketStreamSession::packet_size> output{};
    // The product owns the bounded wait; drive the same helper sequence a
    // product read uses.  The ticket is captured while the attachment is still
    // active, so the empty detach below keeps this reader on its EOF path.
    PacketStreamSession::ReadTicket ticket{};
    {
        std::unique_lock<std::mutex> lock(session.mutex);
        const auto begun = session.begin_read_locked(attached);
        SESSION_CHECK(begun);
        ticket = begun.value();
        const auto copied = session.finish_read_locked(
            attached, ticket,
            MutableByteView{output.data(), 2U * PacketStreamSession::packet_size});
        SESSION_CHECK(copied);
        SESSION_CHECK(copied.value().bytes == 2U * PacketStreamSession::packet_size);
    }
    SESSION_CHECK(output[0] == 0x47U);
    SESSION_CHECK(output[1] == 0x0aU);
    SESSION_CHECK(output[PacketStreamSession::packet_size + 1U] == 0x0bU);
    {
        std::lock_guard<std::mutex> lock(session.mutex);
        SESSION_CHECK(session.detach_locked(attached));
    }
    // A fresh read after an empty detach is NOT_FOUND: neither active nor
    // draining.  Only the already-validated reader keeps its EOF path.
    {
        std::lock_guard<std::mutex> lock(session.mutex);
        const auto after_detach = session.begin_read_locked(attached);
        SESSION_CHECK(!after_detach &&
                      after_detach.error() == Error::NOT_FOUND);
    }
    {
        std::unique_lock<std::mutex> lock(session.mutex);
        std::array<std::uint8_t, PacketStreamSession::packet_size> eof_output{};
        const auto eof = session.finish_read_locked(
            attached, ticket, MutableByteView{eof_output.data(), eof_output.size()});
        SESSION_CHECK(eof && eof.value().eof);
        SESSION_CHECK(eof.value().terminal == StreamTerminal::stopped);
    }
    return true;
}

bool test_reuse_delayed_reader_returns_not_found()
{
    PacketStreamSession session;
    const Attachment first = identity(41U);
    const Attachment second = identity(42U);
    PacketStreamSession::ReadTicket ticket{};
    {
        std::lock_guard<std::mutex> lock(session.mutex);
        SESSION_CHECK(session.begin_attach_locked(first, 4U));
    }
    // Reader A validates the attachment and captures the epoch, then releases
    // the lock (the product's bounded wait does the same).
    {
        std::lock_guard<std::mutex> lock(session.mutex);
        const auto begun = session.begin_read_locked(first);
        SESSION_CHECK(begun);
        ticket = begun.value();
    }
    {
        std::lock_guard<std::mutex> lock(session.mutex);
        SESSION_CHECK(session.detach_locked(first));
        SESSION_CHECK(session.begin_attach_locked(second, 4U));
        SESSION_CHECK(session.incarnation == 2U);
        session.append_locked(view(packet(0x42U)));
    }
    std::array<std::uint8_t, PacketStreamSession::packet_size> output{};
    {
        std::lock_guard<std::mutex> lock(session.mutex);
        // The replacement is visible, but the delayed reader must not consume
        // or retire it.
        SESSION_CHECK(session.read_wake_locked(ticket));
        const auto finished = session.finish_read_locked(
            first, ticket, MutableByteView{output.data(), output.size()});
        SESSION_CHECK(!finished && finished.error() == Error::NOT_FOUND);
        SESSION_CHECK(session.count == 1U);
        SESSION_CHECK(session.attached && !session.draining);
        SESSION_CHECK(session.counters.packets == 1U);
    }
    const auto second_stats = session.stats(second);
    SESSION_CHECK(second_stats && second_stats.value().packets == 1U);
    return true;
}

bool test_blocked_reader_wakes_on_reuse()
{
    PacketStreamSession session;
    const Attachment first = identity(51U);
    const Attachment second = identity(52U);
    {
        std::lock_guard<std::mutex> lock(session.mutex);
        SESSION_CHECK(session.begin_attach_locked(first, 4U));
    }
    // Finite rendezvous: the parent waits for the reader either to enter the
    // bounded wait or to report that begin_read_locked refused to start.  This
    // replaces the previous unbounded start spin, so a reader that never starts
    // cannot hang the fixture.
    std::mutex gate_mutex;
    std::condition_variable gate_changed;
    bool reader_started = false;
    bool reader_start_failed = false;
    bool reader_returned = false;
    bool reader_result = false;
    std::thread reader([&]() noexcept {
        std::unique_lock<std::mutex> lock(session.mutex);
        const auto ticket = session.begin_read_locked(first);
        if (!ticket) {
            lock.unlock();
            {
                std::lock_guard<std::mutex> gate(gate_mutex);
                reader_start_failed = true;
            }
            gate_changed.notify_all();
            return;
        }
        {
            std::lock_guard<std::mutex> gate(gate_mutex);
            reader_started = true;
        }
        gate_changed.notify_all();
        (void)session.changed.wait_for(lock, kWaitBound, [&]() noexcept {
            return session.read_wake_locked(ticket.value());
        });
        std::array<std::uint8_t, PacketStreamSession::packet_size> output{};
        const auto finished = session.finish_read_locked(
            first, ticket.value(), MutableByteView{output.data(), output.size()});
        reader_result = !finished && finished.error() == Error::NOT_FOUND;
        reader_returned = true;
    });
    const bool rendezvous = [&]() noexcept {
        std::unique_lock<std::mutex> gate(gate_mutex);
        return gate_changed.wait_for(gate, kWaitBound, [&]() noexcept {
            return reader_started || reader_start_failed;
        });
    }();
    if (!rendezvous || reader_start_failed) {
        // The reader never entered the bounded wait (it either failed to start
        // or, if the rendezvous timed out, is already on its finite wait path).
        // Join before returning so no joinable thread is destroyed and no
        // reader outlives the session storage.
        reader.join();
        return false;
    }
    bool detached = false;
    bool reattached = false;
    {
        std::lock_guard<std::mutex> lock(session.mutex);
        detached = static_cast<bool>(session.detach_locked(first));
        reattached =
            detached && static_cast<bool>(session.begin_attach_locked(second, 4U));
        if (reattached) session.append_locked(view(packet(0x52U)));
        session.changed.notify_all();
    }
    // Join on every outcome before any assertion returns, so an early failure
    // cannot leave the reader running or the thread object joinable.
    reader.join();
    SESSION_CHECK(detached);
    SESSION_CHECK(reattached);
    SESSION_CHECK(reader_returned);
    SESSION_CHECK(reader_result);
    {
        std::lock_guard<std::mutex> lock(session.mutex);
        SESSION_CHECK(session.count == 1U);
        SESSION_CHECK(session.counters.packets == 1U);
    }
    return true;
}

bool test_incarnation_exhaustion()
{
    PacketStreamSession session;
    const Attachment attached = identity(61U);
    std::array<std::uint8_t, PacketStreamSession::packet_size> output{};
    {
        std::unique_lock<std::mutex> lock(session.mutex);
        SESSION_CHECK(session.packets == nullptr);
        SESSION_CHECK(session.capacity == 0U);
        // Seed the last issuable epoch and drive the real publication path: an
        // admitted attach from MAX-1 must publish exactly MAX rather than refuse
        // or wrap.
        session.incarnation = std::numeric_limits<std::uint64_t>::max() - 1U;
        SESSION_CHECK(session.begin_attach_locked(attached, 4U));
        SESSION_CHECK(session.incarnation == std::numeric_limits<std::uint64_t>::max());
        SESSION_CHECK(session.attached && !session.draining);
        SESSION_CHECK(session.packets != nullptr && session.capacity == 4U);
        SESSION_CHECK(session.head == 0U && session.count == 0U);
        // The external BUSY check keeps its precedence over exhaustion while a
        // session is still active: the refused attach does not mutate state.
        const std::uint8_t* const busy_allocation = session.packets.get();
        const auto busy = session.begin_attach_locked(identity(63U), 4U);
        SESSION_CHECK(!busy && busy.error() == Error::BUSY);
        SESSION_CHECK(session.incarnation == std::numeric_limits<std::uint64_t>::max());
        SESSION_CHECK(session.attached && !session.draining);
        SESSION_CHECK(PacketStreamSession::same_identity(session.identity, attached));
        SESSION_CHECK(session.packets.get() == busy_allocation);
        SESSION_CHECK(session.capacity == 4U);
        SESSION_CHECK(session.head == 0U && session.count == 0U);
        SESSION_CHECK(same_counters(session.counters, StreamCounters{}));
        session.append_locked(view(packet(0x61U)));
        // Drain the queued packet so the detach below is an empty detach and
        // the exhaustion path, not the BUSY path, is exercised.
        const auto ticket = session.begin_read_locked(attached);
        SESSION_CHECK(ticket);
        const auto drained = session.finish_read_locked(
            attached, ticket.value(),
            MutableByteView{output.data(), output.size()});
        SESSION_CHECK(drained && drained.value().bytes == output.size());
        SESSION_CHECK(session.detach_locked(attached));
    }
    // The session is quarantined for new attachments, but the retained final
    // and release keep working.  The refusal must leave the allocation and every
    // meaningful state field exactly as they were.
    const std::uint8_t* const allocation = session.packets.get();
    const std::uint64_t exhausted = session.incarnation;
    const StreamCounters refused_counters = session.counters;
    const StreamCounters refused_final_counters = session.final_snapshot.counters;
    const std::uint8_t final_terminal = session.final_snapshot.terminal;
    const std::size_t refused_head = session.head;
    const std::size_t refused_count = session.count;
    const Attachment refused_identity = session.identity;
    const Attachment refused_final_identity = session.final_identity;
    // The drained packet's bytes remain initialized in the allocation.  They
    // must survive the refusal unchanged; uninitialized tail bytes are not
    // compared.
    std::array<std::uint8_t, PacketStreamSession::packet_size> retained_payload{};
    std::memcpy(retained_payload.data(), session.packets.get(), retained_payload.size());
    {
        std::lock_guard<std::mutex> lock(session.mutex);
        const auto refused = session.begin_attach_locked(identity(62U), 4U);
        SESSION_CHECK(!refused && refused.error() == Error::RESOURCE_LIMIT);
        SESSION_CHECK(session.incarnation == exhausted);
        SESSION_CHECK(!session.attached && !session.draining);
        SESSION_CHECK(session.final_snapshot_valid);
        SESSION_CHECK(
            PacketStreamSession::same_identity(session.identity, refused_identity));
        SESSION_CHECK(PacketStreamSession::same_identity(session.final_identity,
                                                         refused_final_identity));
        SESSION_CHECK(session.packets.get() == allocation);
        SESSION_CHECK(session.capacity == 4U);
        SESSION_CHECK(session.head == refused_head && session.count == refused_count);
        SESSION_CHECK(std::memcmp(session.packets.get(), retained_payload.data(),
                                  retained_payload.size()) == 0);
        SESSION_CHECK(same_counters(session.counters, refused_counters));
        SESSION_CHECK(
            same_counters(session.final_snapshot.counters, refused_final_counters));
        SESSION_CHECK(session.final_snapshot.terminal == final_terminal);
        SESSION_CHECK(session.terminal == StreamTerminal::stopped);
    }
    const auto final = session.final_snapshot_query(attached);
    SESSION_CHECK(final && final.value().counters.packets == 1U);
    SESSION_CHECK(final && final.value().counters.bytes ==
                               PacketStreamSession::packet_size);
    SESSION_CHECK(final.value().terminal ==
                  static_cast<std::uint8_t>(StreamTerminal::stopped));
    SESSION_CHECK(session.release_final(attached));
    // After release the quarantined session still refuses new attachments and
    // keeps the exhausted incarnation.
    {
        std::lock_guard<std::mutex> lock(session.mutex);
        const auto refused = session.begin_attach_locked(identity(64U), 4U);
        SESSION_CHECK(!refused && refused.error() == Error::RESOURCE_LIMIT);
        SESSION_CHECK(session.incarnation == exhausted);
    }
    return true;
}

bool test_abandon_attach_keeps_incarnation()
{
    PacketStreamSession session;
    const Attachment attached = identity(71U);
    {
        std::lock_guard<std::mutex> lock(session.mutex);
        SESSION_CHECK(session.begin_attach_locked(attached, 4U));
        session.abandon_attach_locked();
        SESSION_CHECK(!session.attached);
        SESSION_CHECK(session.terminal == StreamTerminal::stopped);
        SESSION_CHECK(session.incarnation == 1U);
    }
    {
        std::lock_guard<std::mutex> lock(session.mutex);
        SESSION_CHECK(session.begin_attach_locked(identity(72U), 4U));
        SESSION_CHECK(session.incarnation == 2U);
    }
    return true;
}

bool test_shutdown_detach_discards_queue()
{
    PacketStreamSession session;
    const Attachment attached = identity(81U);
    {
        std::lock_guard<std::mutex> lock(session.mutex);
        SESSION_CHECK(session.begin_attach_locked(attached, 4U));
        session.append_locked(view(packet(0x81U)));
        session.shutdown_detach_locked();
        SESSION_CHECK(!session.attached && !session.draining);
        SESSION_CHECK(session.count == 0U);
        SESSION_CHECK(session.final_snapshot_valid);
        SESSION_CHECK(session.final_snapshot.counters.packets == 1U);
    }
    const auto final = session.final_snapshot_query(attached);
    SESSION_CHECK(final && final.value().counters.packets == 1U);
    return true;
}

}  // namespace

int main()
{
    return test_attach_detach_and_queries() &&
                   test_append_overflow_accounting_and_wrap() &&
                   test_two_span_wrap_read() &&
                   test_read_round_trip_and_eof() &&
                   test_reuse_delayed_reader_returns_not_found() &&
                   test_blocked_reader_wakes_on_reuse() &&
                   test_incarnation_exhaustion() &&
                   test_abandon_attach_keeps_incarnation() &&
                   test_shutdown_detach_discards_queue()
               ? 0
               : 1;
}
