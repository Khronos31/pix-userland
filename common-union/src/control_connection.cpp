// SPDX-License-Identifier: MIT
#include <dtv/userland/control_connection.h>

namespace dtv::userland::wire {
namespace {

template <std::size_t N>
void set_ascii(WString<N>& out, const char* text) noexcept
{
    out.length = 0U;
    if (text == nullptr) {
        return;
    }
    while (out.length < N && text[out.length] != '\0') {
        out.bytes[out.length] = text[out.length];
        ++out.length;
    }
}

bool same_epoch(const Uuid& left, const Uuid& right) noexcept
{
    for (std::size_t i = 0U; i < left.size(); ++i) {
        if (left[i] != right[i]) {
            return false;
        }
    }
    return true;
}

const char* operation_name(std::uint16_t opcode) noexcept
{
    switch (opcode) {
    case kOpcodeHello: return "hello";
    case kOpcodeList: return "list";
    case kOpcodeStatus: return "status";
    case kOpcodeAcquire: return "acquire";
    case kOpcodeRelease: return "release";
    case kOpcodeTune: return "tune";
    default: return "request";
    }
}

Scope enclosure_scope() noexcept
{
    return Scope{ScopeKind::enclosure, 0U};
}

}  // namespace

ErrorRecord make_error_record(std::uint32_t code, const char* operation) noexcept
{
    ErrorRecord record;
    record.code = code;
    set_ascii(record.operation, operation);
    const char* symbol = error_symbol(code);
    set_ascii(record.detail, symbol != nullptr ? symbol : "UNKNOWN");
    return record;
}

Result<std::size_t> encode_error_frame(std::uint16_t opcode, std::uint32_t request_id,
                                       const Uuid& epoch, std::uint32_t code,
                                       const char* operation, const Scope& target,
                                       MutableByteView output) noexcept
{
    if (code == 0U) {
        return Result<std::size_t>::failure(Error::INVALID_ARGUMENT);
    }
    ErrorResponse response;
    response.epoch = epoch;
    response.error = make_error_record(code, operation);
    response.target = target;
    return encode_record_frame(Kind::error_response, opcode, request_id, response,
                               &encode_error_response, output);
}

ControlConnection::ControlConnection(const ServerHello& server) noexcept : server_(server) {}

Inbound ControlConnection::refuse(Error reason) noexcept
{
    closed_ = true;
    Inbound inbound;
    inbound.action = InboundAction::close;
    inbound.code = static_cast<std::uint32_t>(reason);
    return inbound;
}

Inbound ControlConnection::reply_error(std::uint16_t opcode, std::uint32_t request_id,
                                       std::uint32_t code, const char* operation,
                                       bool close_after, MutableByteView output) noexcept
{
    const auto written = encode_error_frame(opcode, request_id, server_.epoch, code, operation,
                                            enclosure_scope(), output);
    if (!written) {
        return refuse(written.error());
    }
    outstanding_ = true;
    if (close_after) {
        closed_ = true;
    }
    Inbound inbound;
    inbound.action = InboundAction::respond;
    inbound.reply_size = written.value();
    inbound.close_after_reply = close_after;
    inbound.code = code;
    return inbound;
}

Inbound ControlConnection::accept(const Frame& frame, ControlRequest& request,
                                  MutableByteView output) noexcept
{
    // A client sends only requests on a control connection; a response,
    // error or event from the peer is a protocol violation.
    if (closed_ || frame.header.kind != Kind::request || outstanding_) {
        return refuse(Error::PROTOCOL_ERROR);
    }
    const std::uint32_t request_id = frame.header.request_id;
    const std::uint16_t opcode = frame.header.opcode;
    // The envelope already rejects request_id 0.  Requests are never resent,
    // so the same or an earlier ID is a protocol error.
    if (request_id <= last_request_id_) {
        return refuse(Error::PROTOCOL_ERROR);
    }
    last_request_id_ = request_id;

    if (!established_) {
        if (opcode != kOpcodeHello) {
            return refuse(Error::PROTOCOL_ERROR);
        }
        const auto hello = decode_hello_request(frame.payload);
        if (!hello) {
            if (hello.error() == Error::PROTOCOL_ERROR) {
                return refuse(Error::PROTOCOL_ERROR);
            }
            return reply_error(opcode, request_id, static_cast<std::uint32_t>(hello.error()),
                               "hello", true, output);
        }
        const auto version = negotiate_version(hello.value());
        if (!version) {
            return reply_error(opcode, request_id, static_cast<std::uint32_t>(version.error()),
                               "hello", true, output);
        }
        if ((hello.value().required_features & ~server_.features) != 0U) {
            return reply_error(opcode, request_id,
                               static_cast<std::uint32_t>(Error::UNSUPPORTED), "hello", true,
                               output);
        }
        HelloResponse response;
        response.major = version.value().major;
        response.minor = version.value().minor;
        response.features =
            (hello.value().required_features | hello.value().optional_features) &
            server_.features;
        response.epoch = server_.epoch;
        response.producer = server_.producer;
        response.max_control_payload = static_cast<std::uint32_t>(kMaxControlPayload);
        response.max_ts_bytes = static_cast<std::uint32_t>(kMaxTsPacketBytes);
        const auto written = encode_record_frame(Kind::response, opcode, request_id, response,
                                                 &encode_hello_response, output);
        if (!written) {
            return refuse(written.error());
        }
        established_ = true;
        outstanding_ = true;
        features_ = response.features;
        Inbound inbound;
        inbound.action = InboundAction::respond;
        inbound.reply_size = written.value();
        return inbound;
    }

    // HELLO is negotiated once per connection.
    if (opcode == kOpcodeHello) {
        return refuse(Error::PROTOCOL_ERROR);
    }

    request.opcode = opcode;
    request.request_id = request_id;
    Error decoded = Error::OK;
    const Uuid* epoch = nullptr;
    switch (opcode) {
    case kOpcodeList: {
        const auto value = decode_list_request(frame.payload);
        decoded = value.error();
        if (value) {
            request.kind = ControlRequestKind::list;
            request.list = value.value();
            epoch = &request.list.epoch;
        }
        break;
    }
    case kOpcodeStatus: {
        const auto value = decode_status_request(frame.payload);
        decoded = value.error();
        if (value) {
            request.kind = ControlRequestKind::status;
            request.status = value.value();
            epoch = &request.status.epoch;
        }
        break;
    }
    case kOpcodeAcquire: {
        const auto value = decode_acquire_request(frame.payload);
        decoded = value.error();
        if (value) {
            request.kind = ControlRequestKind::acquire;
            request.acquire = value.value();
            epoch = &request.acquire.epoch;
        }
        break;
    }
    case kOpcodeRelease: {
        const auto value = decode_release_request(frame.payload);
        decoded = value.error();
        if (value) {
            request.kind = ControlRequestKind::release;
            request.release = value.value();
            epoch = &request.release.epoch;
        }
        break;
    }
    case kOpcodeTune: {
        const auto value = decode_tune_request(frame.payload);
        decoded = value.error();
        if (value) {
            request.kind = ControlRequestKind::tune;
            request.tune = value.value();
            epoch = &request.tune.epoch;
        }
        break;
    }
    default:
        // A well-formed request for an operation this server does not serve.
        return reply_error(opcode, request_id, static_cast<std::uint32_t>(Error::UNSUPPORTED),
                           operation_name(opcode), false, output);
    }
    if (decoded == Error::PROTOCOL_ERROR) {
        return refuse(Error::PROTOCOL_ERROR);
    }
    if (decoded != Error::OK) {
        return reply_error(opcode, request_id, static_cast<std::uint32_t>(decoded),
                           operation_name(opcode), false, output);
    }
    if (request.kind == ControlRequestKind::tune && request.tune.pids.present &&
        (features_ & kFeaturePidFilter) == 0U) {
        return reply_error(opcode, request_id, static_cast<std::uint32_t>(Error::UNSUPPORTED),
                           operation_name(opcode), false, output);
    }
    if (!same_epoch(*epoch, server_.epoch)) {
        return reply_error(opcode, request_id, kWireErrorStaleHandle, operation_name(opcode),
                           false, output);
    }
    outstanding_ = true;
    Inbound inbound;
    inbound.action = InboundAction::dispatch;
    return inbound;
}

void ControlConnection::complete() noexcept
{
    outstanding_ = false;
}

Error check_status_scope(const ListResponse& canonical, const Scope& scope) noexcept
{
    if (validate_scope(scope) != Error::OK) {
        return Error::INVALID_ARGUMENT;
    }
    switch (scope.kind) {
    case ScopeKind::enclosure:
        return Error::OK;
    case ScopeKind::receiver:
        if (canonical.receivers.present) {
            for (std::size_t i = 0U; i < canonical.receivers.value.count; ++i) {
                if (canonical.receivers.value.items[i].id == scope.id) {
                    return Error::OK;
                }
            }
        }
        return Error::NOT_FOUND;
    case ScopeKind::reader:
        if (canonical.readers.present) {
            for (std::size_t i = 0U; i < canonical.readers.value.count; ++i) {
                if (canonical.readers.value.items[i].id == scope.id) {
                    return Error::OK;
                }
            }
        }
        return Error::NOT_FOUND;
    case ScopeKind::stream:
        return Error::NOT_FOUND;
    }
    return Error::INVALID_ARGUMENT;
}

Error build_status_shell(const ListResponse& canonical, const Context& context,
                         const Snapshot& snapshot, const Scope& scope,
                         StatusResponse& out) noexcept
{
    const Error membership = check_status_scope(canonical, scope);
    if (membership != Error::OK) {
        return membership;
    }
    out.producer = canonical.producer;
    out.context = context;
    out.snapshot = snapshot;
    out.scope = scope;
    out.enclosure = canonical.enclosure;
    if (out.enclosure.present) {
        Identity& identity = out.enclosure.value.identity;
        identity.uniqueness = identity.serial_state == IdentitySerialState::absent
                                  ? Uniqueness::not_applicable
                                  : Uniqueness::unknown;
    }
    out.topology = canonical.topology;
    out.functions.present = false;
    out.receivers.present = false;
    out.readers.present = false;
    out.functions.value.count = 0U;
    out.receivers.value.count = 0U;
    out.readers.value.count = 0U;
    const bool global = scope.kind == ScopeKind::enclosure;
    if (global) {
        out.functions.present = true;
        out.functions.value = canonical.functions;
    }
    const bool known = canonical.topology.state != TopologyState::unknown;
    if (canonical.receivers.present && known &&
        (global || scope.kind == ScopeKind::receiver)) {
        out.receivers.present = true;
        for (std::size_t i = 0U; i < canonical.receivers.value.count; ++i) {
            const ReceiverDescriptor& descriptor = canonical.receivers.value.items[i];
            if (!global && descriptor.id != scope.id) {
                continue;
            }
            ReceiverStatus& status = out.receivers.value.items[out.receivers.value.count++];
            status = ReceiverStatus{};
            status.descriptor = descriptor;
        }
    }
    if (canonical.readers.present && known && (global || scope.kind == ScopeKind::reader)) {
        out.readers.present = true;
        for (std::size_t i = 0U; i < canonical.readers.value.count; ++i) {
            const ReaderDescriptor& descriptor = canonical.readers.value.items[i];
            if (!global && descriptor.id != scope.id) {
                continue;
            }
            ReaderStatus& status = out.readers.value.items[out.readers.value.count++];
            status = ReaderStatus{};
            status.descriptor = descriptor;
        }
    }
    return Error::OK;
}

Result<std::size_t> encode_status_frame(std::uint32_t request_id, const StatusResponse& value,
                                        const ListResponse& canonical,
                                        MutableByteView output) noexcept
{
    StatusContext context;
    context.list = &canonical;
    const Error valid = validate_status_response(value, context);
    if (valid != Error::OK) {
        return Result<std::size_t>::failure(valid);
    }
    return encode_record_frame(Kind::response, kOpcodeStatus, request_id, value,
                               &encode_status_response, output);
}

}  // namespace dtv::userland::wire
