// SPDX-License-Identifier: MIT
#ifndef DTV_USERLAND_WIRE_H
#define DTV_USERLAND_WIRE_H

#include <dtv/userland/error.h>
#include <dtv/userland/transport.h>

#include <array>
#include <cstddef>
#include <cstdint>

// UDTV 1.0 wire subset: the common 20-byte envelope, a fragment-tolerant
// receiver, and typed HELLO request and ACQUIRE request/response payloads.
// Every integer is little-endian and is written byte by byte; no struct is
// copied to or from the wire. See docs/wire-subset.md for scope and limits.
namespace dtv::userland::wire {

inline constexpr std::array<std::uint8_t, 4> kMagic = {'U', 'D', 'T', 'V'};
// Product-specific magics of the earlier PX4 and ASICEN wires. Always rejected;
// their payloads are never interpreted as UDTV.
inline constexpr std::array<std::uint8_t, 4> kLegacyPx4Magic = {'P', 'X', '4', 'U'};
inline constexpr std::array<std::uint8_t, 4> kLegacyAsicenMagic = {'A', 'S', 'C', 'N'};

inline constexpr std::uint16_t kVersionMajor = 1U;
inline constexpr std::uint16_t kVersionMinor = 0U;

inline constexpr std::size_t kHeaderSize = 20U;
inline constexpr std::size_t kMaxControlPayload = 65536U;
inline constexpr std::size_t kTsPacketSize = 188U;
// Largest multiple of 188 not exceeding 1 MiB.
inline constexpr std::size_t kMaxTsPacketBytes = 1048476U;
// Fixed TS_DATA payload fields that precede the raw packet bytes.
inline constexpr std::size_t kTsDataPrefixSize = 28U;
inline constexpr std::size_t kMaxTsDataPayload = kMaxTsPacketBytes + kTsDataPrefixSize;
inline constexpr std::size_t kMaxFrameSize = kHeaderSize + kMaxTsDataPayload;

inline constexpr std::uint16_t kFlagResponse = 0x0001U;
inline constexpr std::uint16_t kFlagError = 0x0002U;

// Opcodes are carried as raw u16 values so that a well-formed frame with an
// opcode this subset does not know reaches the dispatcher unchanged.
// Bit 15 marks an event opcode for every opcode, known or not.
inline constexpr std::uint16_t kOpcodeEventBit = 0x8000U;
inline constexpr std::uint16_t kOpcodeHello = 0x0001U;
inline constexpr std::uint16_t kOpcodeAcquire = 0x0020U;
inline constexpr std::uint16_t kOpcodeTsData = 0x8040U;

constexpr bool is_event_opcode(std::uint16_t opcode) noexcept
{
    return (opcode & kOpcodeEventBit) != 0;
}

inline constexpr std::size_t kHelloRequestSize = 24U;
inline constexpr std::size_t kEpochSize = 16U;
inline constexpr std::size_t kAcquireRequestSize = kEpochSize + 2U;
inline constexpr std::size_t kAcquireResponseSize = kEpochSize + 8U;

// The only valid combinations of opcode bit 15, flags, and request_id:
//   request         bit 15 clear, flags 0, request_id != 0
//   response        bit 15 clear, flags 1, request_id != 0
//   error_response  bit 15 clear, flags 3, request_id != 0
//   event           bit 15 set,   flags 0, request_id == 0
// Anything else is a malformed envelope (PROTOCOL_ERROR), whether or not the
// opcode is known.
enum class Kind : std::uint8_t {
    request,
    response,
    error_response,
    event,
};

struct Header final {
    Kind kind = Kind::request;
    std::uint16_t opcode = 0U;
    std::uint32_t request_id = 0U;
    std::uint32_t payload_length = 0U;
};

// payload aliases the bytes the frame was decoded from.
struct Frame final {
    Header header;
    ByteView payload;
};

// Detailed reason for rejecting a header. header_error() maps it onto Error.
enum class HeaderStatus : std::uint8_t {
    ok,
    invalid_view,
    wrong_size,
    legacy_magic,
    bad_magic,
    unsupported_version,
    bad_flags,
    bad_request_id,
    bad_kind_for_opcode,
    payload_too_large,
};

Error header_error(HeaderStatus status) noexcept;

// Payload cap for a header: kMaxTsDataPayload for a TS_DATA event, otherwise
// kMaxControlPayload. The cap covers the whole payload, including the TS_DATA
// prefix; it is not a cap on raw packet bytes.
std::size_t payload_limit(Kind kind, std::uint16_t opcode) noexcept;

// Validates an envelope without reading or allocating its payload. input must
// be exactly kHeaderSize bytes. On failure, *header is left unchanged.
HeaderStatus check_header(ByteView input, Header* header) noexcept;
Result<Header> decode_header(ByteView input) noexcept;

// Writes kHeaderSize bytes with version 1.0. The header must pass the same
// rules decode_header enforces.
Result<std::size_t> encode_header(const Header& header, MutableByteView output) noexcept;

// input must be exactly one frame: kHeaderSize + payload_length bytes.
Result<Frame> decode_frame(ByteView input) noexcept;

// header.payload_length must equal payload.size.
Result<std::size_t> encode_frame(const Header& header, ByteView payload,
                                 MutableByteView output) noexcept;

class FrameHandler {
public:
    virtual ~FrameHandler() noexcept = default;

    // frame.payload aliases FrameReceiver storage and is valid only until this
    // call returns. Any opcode, known or not, arrives here unchanged.
    virtual Result<void> on_frame(const Frame& frame) noexcept = 0;
};

// Reassembles frames from arbitrary fragments without sockets or allocation.
// The header is checked as soon as its 20 bytes are present, before any
// payload byte is stored. A payload longer than the caller storage fails with
// BUFFER_TOO_SMALL. Any failure, including one returned by the handler,
// poisons the receiver until reset(), mirroring a closed connection.
class FrameReceiver final {
public:
    explicit FrameReceiver(MutableByteView payload_storage) noexcept;

    // Returns the number of frames delivered during this call.
    Result<std::size_t> feed(ByteView input, FrameHandler& handler) noexcept;

    // Call when the input reaches EOF. Succeeds only on a clean frame
    // boundary. EOF inside a header or payload is PROTOCOL_ERROR and poisons
    // the receiver: the stream ended mid-frame and is never resynchronized.
    Result<void> finish() noexcept;

    // Bytes of the current incomplete frame (header and payload) held so far;
    // 0 exactly on a frame boundary.
    std::size_t pending_size() const noexcept;

    void reset() noexcept;

    bool failed() const noexcept { return failure_ != Error::OK; }
    Error failure() const noexcept { return failure_; }
    // Status of the last header check; distinguishes, for example, legacy magic.
    HeaderStatus header_status() const noexcept { return header_status_; }
    // True after finish() found EOF inside a frame.
    bool truncated() const noexcept { return truncated_; }

    FrameReceiver(const FrameReceiver&) = delete;
    FrameReceiver& operator=(const FrameReceiver&) = delete;

private:
    Result<std::size_t> fail(Error error) noexcept;

    MutableByteView storage_;
    std::array<std::uint8_t, kHeaderSize> header_bytes_{};
    std::size_t header_filled_ = 0U;
    bool have_header_ = false;
    Header header_{};
    std::size_t payload_filled_ = 0U;
    HeaderStatus header_status_ = HeaderStatus::ok;
    bool truncated_ = false;
    Error failure_ = Error::OK;
};

struct Version final {
    std::uint16_t major = 0U;
    std::uint16_t minor = 0U;
};

// HELLO request: u16 min_major, u16 min_minor, u16 max_major, u16 max_minor,
// u64 required_features, u64 optional_features. Both masks, including bits
// this subset does not know, are carried unchanged; feature policy belongs to
// the dispatcher.
struct HelloRequest final {
    Version min;
    Version max;
    std::uint64_t required_features = 0U;
    std::uint64_t optional_features = 0U;
};

// Opaque daemon epoch; never interpreted here.
using Epoch = std::array<std::uint8_t, kEpochSize>;

// ACQUIRE request: u8 epoch[16], u16 receiver_id. Every u16 receiver_id,
// including 0 and values above 255, is representable; the daemon validates it
// against its public receiver set.
struct AcquireRequest final {
    Epoch epoch{};
    std::uint16_t receiver_id = 0U;
};

// ACQUIRE response: u8 epoch[16], u64 lease_id. lease_id 0 is never valid.
// There is no nonce; the attach token belongs to START in the later wire.
struct AcquireResponse final {
    Epoch epoch{};
    std::uint64_t lease_id = 0U;
};

// Typed codecs require the exact payload size (PROTOCOL_ERROR otherwise) and
// reject semantically invalid values with INVALID_ARGUMENT. Encoders return
// BUFFER_TOO_SMALL when output is shorter than the payload.
Result<std::size_t> encode_hello_request(const HelloRequest& value,
                                         MutableByteView output) noexcept;
// Rejects min > max (compared as major, then minor).
Result<HelloRequest> decode_hello_request(ByteView input) noexcept;

// Returns 1.0 when the requested range contains it, otherwise
// VERSION_MISMATCH. An inverted range is INVALID_ARGUMENT.
Result<Version> negotiate_version(const HelloRequest& request) noexcept;

Result<std::size_t> encode_acquire_request(const AcquireRequest& value,
                                           MutableByteView output) noexcept;
Result<AcquireRequest> decode_acquire_request(ByteView input) noexcept;

Result<std::size_t> encode_acquire_response(const AcquireResponse& value,
                                            MutableByteView output) noexcept;
Result<AcquireResponse> decode_acquire_response(ByteView input) noexcept;

}  // namespace dtv::userland::wire

#endif  // DTV_USERLAND_WIRE_H
