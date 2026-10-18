// SPDX-License-Identifier: MIT
#include <dtv/userland/control_client.h>

#include <dtv/userland/control_connection.h>
#include <dtv/userland/wire.h>

#include <array>
#include <cstring>
#include <limits>

namespace dtv::userland::wire {
namespace {

struct RawReply final {
    Header header{};
    std::array<std::uint8_t, kMaxControlPayload> payload{};
    std::size_t length = 0U;
    bool seen = false;
};

class ReplyHandler final : public FrameHandler {
public:
    ReplyHandler(std::uint16_t opcode, std::uint32_t request_id,
                 RawReply& reply) noexcept
        : opcode_(opcode), request_id_(request_id), reply_(reply) {}

    Result<void> on_frame(const Frame& frame) noexcept override
    {
        if (reply_.seen || frame.header.kind == Kind::event ||
            frame.header.kind == Kind::request || frame.header.opcode != opcode_ ||
            frame.header.request_id != request_id_ ||
            frame.payload.size > reply_.payload.size())
            return Result<void>::failure(Error::PROTOCOL_ERROR);
        reply_.header = frame.header;
        reply_.length = frame.payload.size;
        if (frame.payload.size != 0U)
            std::memcpy(reply_.payload.data(), frame.payload.data, frame.payload.size);
        reply_.seen = true;
        return Result<void>::success();
    }

private:
    std::uint16_t opcode_;
    std::uint32_t request_id_;
    RawReply& reply_;
};

std::uint32_t remaining_ms(ClientTransport& transport, std::uint64_t deadline) noexcept
{
    const std::uint64_t now = transport.monotonic_ms();
    if (now >= deadline) return 0U;
    const std::uint64_t remaining = deadline - now;
    return remaining > std::numeric_limits<std::uint32_t>::max()
               ? std::numeric_limits<std::uint32_t>::max()
               : static_cast<std::uint32_t>(remaining);
}

Result<RawReply> exchange(ClientTransport& transport, std::uint16_t opcode,
                          std::uint32_t request_id, ByteView payload,
                          std::uint64_t deadline) noexcept
{
    std::array<std::uint8_t, kHeaderSize + kMaxControlPayload> outgoing{};
    Header header;
    header.kind = Kind::request;
    header.opcode = opcode;
    header.request_id = request_id;
    header.payload_length = static_cast<std::uint32_t>(payload.size);
    const auto encoded = encode_frame(header, payload,
                                      MutableByteView{outgoing.data(), outgoing.size()});
    if (!encoded) return Result<RawReply>::failure(encoded.error());
    std::uint32_t left = remaining_ms(transport, deadline);
    if (left == 0U) return Result<RawReply>::failure(Error::TIMEOUT);
    const auto sent = transport.write_all(ByteView{outgoing.data(), encoded.value()}, Timeout{left});
    if (!sent) return Result<RawReply>::failure(sent.error());

    RawReply reply{};
    ReplyHandler handler(opcode, request_id, reply);
    std::array<std::uint8_t, kMaxControlPayload> scratch{};
    FrameReceiver receiver(MutableByteView{scratch.data(), scratch.size()});
    std::array<std::uint8_t, 8192U> input{};
    while (!reply.seen) {
        left = remaining_ms(transport, deadline);
        if (left == 0U) return Result<RawReply>::failure(Error::TIMEOUT);
        const auto read = transport.read_some(MutableByteView{input.data(), input.size()},
                                             Timeout{left});
        if (!read) {
            if (read.error() == Error::DISCONNECTED) {
                const auto ended = receiver.finish();
                return Result<RawReply>::failure(ended ? Error::DISCONNECTED : ended.error());
            }
            return Result<RawReply>::failure(read.error());
        }
        const auto fed = receiver.feed(ByteView{input.data(), read.value()}, handler);
        if (!fed) return Result<RawReply>::failure(fed.error());
        if (reply.seen) {
            const auto finished = receiver.finish();
            if (!finished) return Result<RawReply>::failure(finished.error());
        }
    }
    return Result<RawReply>::success(std::move(reply));
}

Error remote_error(std::uint32_t code) noexcept
{
    switch (code) {
    case 1U: return Error::INVALID_ARGUMENT;
    case 2U: return Error::VERSION_MISMATCH;
    case 3U: return Error::NOT_FOUND;
    case 4U: return Error::BUSY;
    case 5U: return Error::NOT_READY;
    case 6U: return Error::TIMEOUT;
    case 7U: return Error::USB_IO;
    case 8U: return Error::DISCONNECTED;
    case 9U: return Error::PROTOCOL_ERROR;
    case 10U: return Error::FIRMWARE_REJECTED;
    case 11U: return Error::UNSUPPORTED;
    case 12U: return Error::NO_CARD;
    case 13U: return Error::CARD_REMOVED;
    case 14U: return Error::BUFFER_TOO_SMALL;
    case 15U: return Error::SLOW_CONSUMER;
    case 17U: return Error::RESOURCE_LIMIT;
    case 255U: return Error::INTERNAL;
    default: return Error::PROTOCOL_ERROR;
    }
}

template <std::size_t N>
bool same_text(const WString<N>& left, const WString<N>& right) noexcept
{
    return left.length == right.length &&
           std::memcmp(left.bytes.data(), right.bytes.data(), left.length) == 0;
}

template <std::size_t N>
bool same_optional_text(const Optional<WString<N>>& left,
                        const Optional<WString<N>>& right) noexcept
{
    return left.present == right.present &&
           (!left.present || same_text(left.value, right.value));
}

bool same_model(const Optional<Model>& left, const Optional<Model>& right) noexcept
{
    return left.present == right.present &&
           (!left.present || (same_text(left.value.id, right.value.id) &&
                              same_text(left.value.name, right.value.name)));
}

bool same_identity(const Identity& left, const Identity& right) noexcept
{
    return left.serial_state == right.serial_state &&
           same_optional_text(left.serial, right.serial);
}

bool same_location(const Location& left, const Location& right) noexcept
{
    return same_optional_text(left.usb_path, right.usb_path) &&
           left.bus.present == right.bus.present &&
           (!left.bus.present || left.bus.value == right.bus.value) &&
           left.address.present == right.address.present &&
           (!left.address.present || left.address.value == right.address.value);
}

bool same_function_identity(const UsbFunction& left, const UsbFunction& right) noexcept
{
    return left.id == right.id && left.vid == right.vid && left.pid == right.pid &&
           same_location(left.location, right.location) &&
           left.serial_state == right.serial_state &&
           same_optional_text(left.serial, right.serial) &&
           same_model(left.model, right.model);
}

bool same_snapshot_identity(const ListResponse& list,
                            const StatusResponse& status) noexcept
{
    if (list.enclosure.present != status.enclosure.present) return false;
    if (list.enclosure.present &&
        (list.enclosure.value.id != status.enclosure.value.id ||
         !same_model(list.enclosure.value.model, status.enclosure.value.model) ||
         !same_identity(list.enclosure.value.identity, status.enclosure.value.identity)))
        return false;
    if (list.functions.count != status.functions.value.count) return false;
    for (std::size_t index = 0U; index < list.functions.count; ++index) {
        const UsbFunction& listed = list.functions.items[index];
        std::size_t observed_index = status.functions.value.count;
        for (std::size_t candidate = 0U; candidate < status.functions.value.count;
             ++candidate) {
            if (status.functions.value.items[candidate].id == listed.id) {
                if (observed_index != status.functions.value.count) return false;
                observed_index = candidate;
            }
        }
        if (observed_index == status.functions.value.count ||
            !same_function_identity(listed,
                                    status.functions.value.items[observed_index]))
            return false;
    }
    return true;
}

template <typename Record>
Result<std::size_t> request_payload(const Record& value, MutableByteView output,
                                    Result<std::size_t> (*encode)(const Record&,
                                                                   MutableByteView) noexcept) noexcept
{
    return encode(value, output);
}

Result<RawReply> roundtrip(ClientTransport& transport, std::uint16_t opcode,
                           std::uint32_t request_id, ByteView payload,
                           std::uint64_t deadline, std::uint32_t& remote_code,
                           const Uuid* expected_epoch = nullptr) noexcept
{
    auto reply = exchange(transport, opcode, request_id, payload, deadline);
    if (!reply) return reply;
    if (reply.value().header.kind == Kind::error_response) {
        const auto error = decode_error_response(
            ByteView{reply.value().payload.data(), reply.value().length});
        if (!error) return Result<RawReply>::failure(error.error());
        if (expected_epoch != nullptr && error.value().epoch != *expected_epoch)
            return Result<RawReply>::failure(Error::PROTOCOL_ERROR);
        remote_code = error.value().error.code;
        return Result<RawReply>::failure(remote_error(error.value().error.code));
    }
    if (reply.value().header.kind != Kind::response)
        return Result<RawReply>::failure(Error::PROTOCOL_ERROR);
    return reply;
}

}  // namespace

StatusFetchResult fetch_status(ClientTransport& transport, Timeout timeout) noexcept
{
    StatusFetchResult result{};
    if (timeout.milliseconds == 0U) {
        result.error = Error::INVALID_ARGUMENT;
        transport.close();
        return result;
    }
    const std::uint64_t started = transport.monotonic_ms();
    if (started > std::numeric_limits<std::uint64_t>::max() - timeout.milliseconds) {
        result.error = Error::TIMEOUT;
        transport.close();
        return result;
    }
    const std::uint64_t deadline = started + timeout.milliseconds;
    const auto fail = [&transport, &result](Error error) noexcept {
        result.error = error;
        transport.close();
        return result;
    };

    HelloRequest hello_request{};
    hello_request.min = Version{kVersionMajor, kVersionMinor};
    hello_request.max = Version{kVersionMajor, kVersionMinor};
    std::array<std::uint8_t, kHelloRequestSize> hello_bytes{};
    const auto hello_size = encode_hello_request(
        hello_request, MutableByteView{hello_bytes.data(), hello_bytes.size()});
    if (!hello_size) return fail(hello_size.error());
    std::uint32_t remote_code = 0U;
    auto hello_reply = roundtrip(transport, kOpcodeHello, 1U,
                                 ByteView{hello_bytes.data(), hello_size.value()}, deadline,
                                 remote_code);
    if (!hello_reply) {
        result.remote_error_code = remote_code;
        return fail(hello_reply.error());
    }
    const auto hello = decode_hello_response(
        ByteView{hello_reply.value().payload.data(), hello_reply.value().length});
    if (!hello) return fail(hello.error());
    if (hello.value().major != kVersionMajor || hello.value().minor > kVersionMinor)
        return fail(Error::VERSION_MISMATCH);

    ListRequest list_request{};
    list_request.epoch = hello.value().epoch;
    std::array<std::uint8_t, kMaxControlPayload> request_bytes{};
    const auto list_size = request_payload(
        list_request, MutableByteView{request_bytes.data(), request_bytes.size()},
        encode_list_request);
    if (!list_size) return fail(list_size.error());
    remote_code = 0U;
    auto list_reply = roundtrip(transport, kOpcodeList, 2U,
                                ByteView{request_bytes.data(), list_size.value()}, deadline,
                                remote_code, &hello.value().epoch);
    if (!list_reply) {
        result.remote_error_code = remote_code;
        return fail(list_reply.error());
    }
    const auto list = decode_list_response(
        ByteView{list_reply.value().payload.data(), list_reply.value().length});
    if (!list) return fail(list.error());
    if (list.value().epoch != hello.value().epoch ||
        validate_list_response(list.value()) != Error::OK ||
        list.value().producer.product.length != hello.value().producer.product.length ||
        std::memcmp(list.value().producer.product.bytes.data(),
                    hello.value().producer.product.bytes.data(),
                    list.value().producer.product.length) != 0 ||
        list.value().producer.version.length != hello.value().producer.version.length ||
        std::memcmp(list.value().producer.version.bytes.data(),
                    hello.value().producer.version.bytes.data(),
                    list.value().producer.version.length) != 0)
    {
        return fail(Error::PROTOCOL_ERROR);
    }

    StatusRequest status_request{};
    status_request.epoch = hello.value().epoch;
    status_request.scope.kind = ScopeKind::enclosure;
    status_request.scope.id = 0U;
    const auto status_size = request_payload(
        status_request, MutableByteView{request_bytes.data(), request_bytes.size()},
        encode_status_request);
    if (!status_size) return fail(status_size.error());
    remote_code = 0U;
    auto status_reply = roundtrip(transport, kOpcodeStatus, 3U,
                                  ByteView{request_bytes.data(), status_size.value()}, deadline,
                                  remote_code, &hello.value().epoch);
    if (!status_reply) {
        result.remote_error_code = remote_code;
        return fail(status_reply.error());
    }
    const auto status = decode_status_response(
        ByteView{status_reply.value().payload.data(), status_reply.value().length});
    if (!status) return fail(status.error());
    if (status.value().scope.kind != status_request.scope.kind ||
        status.value().scope.id != status_request.scope.id)
        return fail(Error::PROTOCOL_ERROR);
    if (status.value().context.epoch != hello.value().epoch ||
        status.value().producer.product.length != hello.value().producer.product.length ||
        std::memcmp(status.value().producer.product.bytes.data(),
                    hello.value().producer.product.bytes.data(),
                    status.value().producer.product.length) != 0 ||
        status.value().producer.version.length != hello.value().producer.version.length ||
        std::memcmp(status.value().producer.version.bytes.data(),
                    hello.value().producer.version.bytes.data(),
                    status.value().producer.version.length) != 0)
        return fail(Error::PROTOCOL_ERROR);
    StatusContext context{};
    context.list = &list.value();
    const Error valid = validate_status_response(status.value(), context);
    if (valid != Error::OK) return fail(valid);
    if (!same_snapshot_identity(list.value(), status.value()))
        return fail(Error::PROTOCOL_ERROR);
    result.snapshot.list = list.value();
    result.snapshot.status = status.value();
    result.error = Error::OK;
    return result;
}

}  // namespace dtv::userland::wire
