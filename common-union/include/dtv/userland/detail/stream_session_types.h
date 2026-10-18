// SPDX-License-Identifier: MIT
#ifndef DTV_USERLAND_DETAIL_STREAM_SESSION_TYPES_H
#define DTV_USERLAND_DETAIL_STREAM_SESSION_TYPES_H

#include <array>
#include <cstddef>
#include <cstdint>

// Neutral stream-session identity, accounting, and terminal declarations shared
// by the internal receiver engine and the internal packet-session storage.  The
// receiver extraction originally carried them inline; they are split into this
// header so a capture-only build can use the neutral types without depending on
// the disabled receiver implementation.  Declaration names, field layout,
// defaults, and enum numbering are preserved so existing consumers keep
// compiling unchanged.  These declarations may change without notice and are
// not a stable public API.
namespace dtv::userland::detail {

// Stable receiver identity chosen by the consumer.  It is independent of the
// storage slot that holds the receiver.
using ReceiverId = std::uint16_t;

inline constexpr std::size_t kReceiverTokenLength = 16U;
using ReceiverToken = std::array<std::uint8_t, kReceiverTokenLength>;

// The numbering matches the legacy PX4 values so that a facade may pass any
// other legacy value through unchanged to backend queries and identity
// comparisons.  Request validation rejects such values before they are acted
// on.
enum class ReceiverSystem : std::uint8_t {
    terrestrial = 1U,
    satellite = 2U,
};

struct Attachment final {
    std::uint64_t owner_client_id = 0U;
    std::uint64_t lease_id = 0U;
    std::uint64_t attachment_id = 0U;
    ReceiverId receiver = 0U;
    ReceiverSystem system = ReceiverSystem::terrestrial;
    ReceiverToken token{};
};

struct StreamCounters final {
    std::uint64_t packets = 0U;
    std::uint64_t bytes = 0U;
    std::uint64_t sync_errors = 0U;
    std::uint64_t tei_packets = 0U;
    std::uint64_t continuity_errors = 0U;
    std::uint64_t queue_drops = 0U;
    std::uint64_t usb_errors = 0U;
    std::uint64_t empty_intervals = 0U;
};

struct StreamFinalSnapshot final {
    StreamCounters counters{};
    std::uint8_t terminal = 0U;
};

// Terminal code a session reports at its detach/final linearization point.  Its
// numbering is the legacy PX4 stream terminal vocabulary, so a product facade
// maps its own terminal enum by value.
enum class StreamTerminal : std::uint8_t {
    none = 0,
    slow_consumer = 1,
    usb_error = 2,
    sync_error = 3,
    bridge_fatal = 4,
    stopped = 5,
    disconnected = 6,
};

// Terminal code a stream reports in its final snapshot after transport loss.
inline constexpr std::uint8_t kStreamTerminalDisconnected =
    static_cast<std::uint8_t>(StreamTerminal::disconnected);

// One packet-session read outcome.  Its field set and defaults are the legacy
// PX4 stream read result so a product facade maps by value.
struct StreamReadResult final {
    std::size_t bytes = 0U;
    bool eof = false;
    bool timed_out = false;
    StreamTerminal terminal = StreamTerminal::none;
};

}  // namespace dtv::userland::detail

#endif  // DTV_USERLAND_DETAIL_STREAM_SESSION_TYPES_H
