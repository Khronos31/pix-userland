// SPDX-License-Identifier: MIT
#ifndef DTV_USERLAND_DETAIL_PACKET_STREAM_SESSION_H
#define DTV_USERLAND_DETAIL_PACKET_STREAM_SESSION_H

#include "dtv/userland/detail/stream_session_types.h"
#include "dtv/userland/error.h"
#include "dtv/userland/transport.h"

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>

// One generic complete-packet session: storage, read, drain, and retained-final
// state shared by product data planes.  It owns no worker, socket, or device;
// hardware, demux, startup, continuity, and producer stop/join policy stay in
// the product.  A product Session derives or composes this state and shares its
// single mutex/condition variable, so append, terminal, and final decisions
// remain atomic with the product's observation of them.
//
// This is internal compatibility storage, not the adopted v1 capture contract:
// a retained final does not promise that producer callbacks were joined or a
// borrowed handle returned.  These declarations may change without notice and
// are not a stable API.
namespace dtv::userland::detail {

class PacketStreamSession {
public:
    // Fixed complete-packet granularity of the queue payload.
    static constexpr std::size_t packet_size = 188U;

    PacketStreamSession() noexcept = default;
    ~PacketStreamSession() noexcept = default;
    PacketStreamSession(const PacketStreamSession&) = delete;
    PacketStreamSession& operator=(const PacketStreamSession&) = delete;

    // Full compound attachment equality.  It is the identity boundary for a
    // read or a retained final; a shorter comparison would accept a stale
    // reader against a replacement session.
    static bool same_identity(const Attachment& left,
                              const Attachment& right) noexcept;

    // --- Locked helpers.  The caller already holds mutex. ---

    // Admitted logical attachment publication.  Order: BUSY for an active or
    // draining session, then nonwrapping incarnation exhaustion, then checked
    // allocation, then the reset/publication.  The incarnation increments as
    // part of publication, so a reader that validated the previous epoch
    // observes a mismatch after a detach/reuse.  A later product bridge-start
    // failure calls abandon_attach_locked() and keeps the increment.
    Result<void> begin_attach_locked(const Attachment& identity,
                                     std::size_t queue_packets) noexcept;

    // Compensation for a failed product startup after publication.  The
    // logical publication already happened, so this only clears the active
    // state; it does not decrement the incarnation.
    void abandon_attach_locked() noexcept;

    // Detach linearization.  It snapshots counters/terminal and invalidates the
    // active state for this exact identity, keeping already queued packets
    // drainable and waking blocked readers.  The incarnation does not change;
    // a later attach is the replacement boundary.  Returns NOT_FOUND when this
    // identity is not the attached one.
    Result<void> detach_locked(const Attachment& identity) noexcept;

    // Generic packet append after the product's continuity/TEI observation:
    // accounting precedes a full-ring rejection, the slow-consumer terminal is
    // set once, and a normal append writes the tail and wakes readers.
    void append_locked(ByteView packet) noexcept;

    // Amend the retained final terminal for an exact still-valid identity.
    void amend_final_terminal_locked(const Attachment& identity,
                                     StreamTerminal reason) noexcept;

    // Amend the retained final terminal for any valid final (shutdown path).
    void amend_final_terminal_any_locked(StreamTerminal reason) noexcept;

    // Shutdown detach: snapshot the final, discard the queue, wake readers.
    // The product owns the physical stop and publication point.
    void shutdown_detach_locked() noexcept;

    // --- Read helpers.  The product owns the bounded wait and its test seam;
    // these keep the generic identity/incarnation/packet logic in one place. ---

    // Capture the epoch when a read first validates the attachment.  Returns
    // NOT_FOUND for an identity that is neither active nor draining.
    struct ReadTicket final {
        std::uint64_t incarnation = 0U;
    };
    Result<ReadTicket> begin_read_locked(const Attachment& identity) const noexcept;

    // The bounded wait predicate: data, a terminal, or a session-reuse
    // mismatch ends the wait.
    bool read_wake_locked(const ReadTicket& ticket) const noexcept;

    // Post-wait decision and copy.  A ticket incarnation mismatch returns
    // NOT_FOUND before any packet, counter, draining, or final access, so a
    // delayed reader never touches a replacement session.  A same-epoch empty
    // terminal reports EOF; otherwise the two-span wrap copy and head/count
    // update are performed.
    Result<StreamReadResult> finish_read_locked(const Attachment& identity,
                                                const ReadTicket& ticket,
                                                MutableByteView output) noexcept;

    // --- Self-locking queries. ---

    Result<StreamCounters> stats(const Attachment& identity) const noexcept;
    Result<StreamFinalSnapshot> final_snapshot_query(
        const Attachment& identity) const noexcept;
    Result<void> release_final(const Attachment& identity) noexcept;
    Result<StreamTerminal> terminal_query(const Attachment& identity) const noexcept;

    // --- Shared state.  Product code observes these under mutex. ---

    mutable std::mutex mutex;
    std::condition_variable changed;
    std::unique_ptr<std::uint8_t[]> packets;
    std::size_t capacity = 0U;
    std::size_t head = 0U;
    std::size_t count = 0U;
    bool attached = false;
    bool draining = false;
    Attachment identity{};
    bool final_snapshot_valid = false;
    Attachment final_identity{};
    StreamFinalSnapshot final_snapshot{};
    StreamTerminal terminal = StreamTerminal::none;
    StreamCounters counters{};
    // Session-local replacement identity.  It is never zero-after-wrap: a
    // logged publication increments it, and exhaustion quarantines the
    // session's attachments rather than resetting.
    std::uint64_t incarnation = 0U;
};

}  // namespace dtv::userland::detail

#endif  // DTV_USERLAND_DETAIL_PACKET_STREAM_SESSION_H
