// SPDX-License-Identifier: MIT
#ifndef DTV_USERLAND_CONTROL_CONNECTION_H
#define DTV_USERLAND_CONTROL_CONNECTION_H

#include <dtv/userland/records.h>

#include <cstddef>
#include <cstdint>

// Portable UDTV 1.0 control-connection serializer and response helpers.  One
// ControlConnection belongs to one control connection and is driven by the
// owner's event loop; it owns no socket, thread, timer or service.  It applies
// the handshake, request-ID, one-outstanding-request and opcode rules of the
// adopted wire contract and decodes each served request exactly once into an
// owned typed value.  Scheduling, ownership and the service calls stay with
// the caller.  See docs/control-connection.md.
namespace dtv::userland::wire {

// Wire error codes outside the foundation Error enumeration.
inline constexpr std::uint32_t kWireErrorStaleHandle = 16U;

// Daemon-wide values offered to every connection.  The caller keeps the
// object alive and unchanged for the lifetime of its connections.
struct ServerHello final {
    Uuid epoch{};
    Producer producer;
    // Features this server negotiates.  A required bit outside this set is
    // refused with UNSUPPORTED during HELLO.
    std::uint64_t features = 0U;
};

// Requests a ControlConnection decodes for dispatch.  Every other well-formed
// request opcode is answered with UNSUPPORTED.
enum class ControlRequestKind : std::uint8_t {
    list,
    status,
    acquire,
    release,
    tune,
};

// Owned decoded request; only the member selected by kind is meaningful.  It
// is large (TUNE carries a bounded PID array), so callers keep one per event
// loop rather than one per connection.
struct ControlRequest final {
    ControlRequestKind kind = ControlRequestKind::list;
    std::uint16_t opcode = 0U;
    std::uint32_t request_id = 0U;
    ListRequest list{};
    StatusRequest status{};
    AcquireRequest acquire{};
    ReleaseRequest release{};
    TuneRequest tune{};
};

enum class InboundAction : std::uint8_t {
    // A complete reply frame was written to the output; send it.
    respond,
    // request holds a decoded request to serve.
    dispatch,
    // The input broke a framing or connection rule: close without a reply.
    close,
};

struct Inbound final {
    InboundAction action = InboundAction::close;
    // Size of the reply frame for respond.
    std::size_t reply_size = 0U;
    // For respond: end the connection once the reply has been sent.
    bool close_after_reply = false;
    // Wire error code of a refusal (close) or of an error reply (respond); 0
    // for a successful HELLO reply and for dispatch.
    std::uint32_t code = 0U;
};

class ControlConnection final {
public:
    explicit ControlConnection(const ServerHello& server) noexcept;

    // Classifies one complete frame.  Each accepted request (respond or
    // dispatch) becomes the single outstanding request, and no further frame
    // is accepted until complete() is called after its whole reply frame has
    // been written.  Request IDs must increase strictly, HELLO must come first
    // and only once, and LIST/STATUS/ACQUIRE/RELEASE/TUNE must carry the
    // daemon epoch (otherwise STALE_HANDLE).  Nothing is written to output for
    // dispatch or close.  output must hold kHeaderSize + kMaxControlPayload
    // bytes so every reply fits.
    Inbound accept(const Frame& frame, ControlRequest& request,
                   MutableByteView output) noexcept;

    void complete() noexcept;

    bool established() const noexcept { return established_; }
    bool outstanding() const noexcept { return outstanding_; }
    std::uint64_t features() const noexcept { return features_; }
    const ServerHello& server() const noexcept { return server_; }

private:
    Inbound refuse(Error reason) noexcept;
    Inbound reply_error(std::uint16_t opcode, std::uint32_t request_id, std::uint32_t code,
                        const char* operation, bool close_after,
                        MutableByteView output) noexcept;

    const ServerHello& server_;
    bool established_ = false;
    bool outstanding_ = false;
    bool closed_ = false;
    std::uint32_t last_request_id_ = 0U;
    std::uint64_t features_ = 0U;
};

// Writes one reply frame: kind response (flags 1) or error_response (flags 3)
// with the record payload.  A record whose encoding exceeds the control
// payload cap is RESOURCE_LIMIT; a short output is BUFFER_TOO_SMALL.
template <typename Record>
Result<std::size_t> encode_record_frame(Kind kind, std::uint16_t opcode,
                                        std::uint32_t request_id, const Record& value,
                                        Result<std::size_t> (*encode)(const Record&,
                                                                      MutableByteView) noexcept,
                                        MutableByteView output) noexcept
{
    if (output.data == nullptr || output.size < kHeaderSize) {
        return Result<std::size_t>::failure(Error::BUFFER_TOO_SMALL);
    }
    const std::size_t room = output.size - kHeaderSize;
    const std::size_t cap = room < kMaxControlPayload ? room : kMaxControlPayload;
    const auto payload = encode(value, MutableByteView{output.data + kHeaderSize, cap});
    if (!payload) {
        if (payload.error() == Error::BUFFER_TOO_SMALL && cap == kMaxControlPayload) {
            return Result<std::size_t>::failure(Error::RESOURCE_LIMIT);
        }
        return Result<std::size_t>::failure(payload.error());
    }
    Header header;
    header.kind = kind;
    header.opcode = opcode;
    header.request_id = request_id;
    header.payload_length = static_cast<std::uint32_t>(payload.value());
    const auto written = encode_header(header, output);
    if (!written) {
        return written;
    }
    return Result<std::size_t>::success(kHeaderSize + payload.value());
}

// Error reply for request_id.  operation is a short ASCII name; the detail is
// the symbol of code (or "UNKNOWN"), never a raw log.  Code 0 is invalid.
Result<std::size_t> encode_error_frame(std::uint16_t opcode, std::uint32_t request_id,
                                       const Uuid& epoch, std::uint32_t code,
                                       const char* operation, const Scope& target,
                                       MutableByteView output) noexcept;

// Error record for a typed completion (RELEASE error, StreamFinal error).
ErrorRecord make_error_record(std::uint32_t code, const char* operation) noexcept;

// Membership of a STATUS scope in the canonical same-epoch LIST: OK for the
// enclosure, a listed receiver or a listed reader; NOT_FOUND for an unlisted
// receiver/reader ID; INVALID_ARGUMENT for an invalid scope.  A stream scope is
// NOT_FOUND here because the canonical LIST carries no stream identity; a
// caller that issues streams resolves stream scopes itself.
Error check_status_scope(const ListResponse& canonical, const Scope& scope) noexcept;

// Fills the immutable part of a STATUS response for a scope that passed
// check_status_scope: producer, context, snapshot, scope, enclosure, topology,
// the canonical function set (enclosure scope only) and one entry per scoped
// receiver/reader carrying its canonical descriptor in LIST order.  Every
// dynamic field is left at its default for the caller to project from its
// service state.
Error build_status_shell(const ListResponse& canonical, const Context& context,
                         const Snapshot& snapshot, const Scope& scope,
                         StatusResponse& out) noexcept;

// Validates value against the canonical LIST with validate_status_response and
// writes the response frame.  Nothing unvalidated is ever encoded.
Result<std::size_t> encode_status_frame(std::uint32_t request_id, const StatusResponse& value,
                                        const ListResponse& canonical,
                                        MutableByteView output) noexcept;

}  // namespace dtv::userland::wire

#endif  // DTV_USERLAND_CONTROL_CONNECTION_H
