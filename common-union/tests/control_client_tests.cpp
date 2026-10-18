// SPDX-License-Identifier: MIT
#include <dtv/userland/control_client.h>

#include <dtv/userland/wire.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>

namespace {

namespace w = dtv::userland::wire;
using dtv::userland::ByteView;
using dtv::userland::Error;
using dtv::userland::MutableByteView;
using dtv::userland::Timeout;

#define CHECK(condition)                                                                    \
    do {                                                                                    \
        if (!(condition)) {                                                                 \
            std::fprintf(stderr, "check failed at %s:%d: %s\n", __FILE__, __LINE__,       \
                         #condition);                                                       \
            return false;                                                                   \
        }                                                                                   \
    } while (false)

template <std::size_t N>
bool set_text(w::WString<N>& out, const char* value) noexcept
{
    const std::size_t length = std::strlen(value);
    if (length > out.bytes.size()) return false;
    out.length = static_cast<std::uint16_t>(length);
    std::memcpy(out.bytes.data(), value, length);
    return true;
}

enum class Mode {
    unknown_list_error,
    remote_list_error,
    timeout_after_hello,
    partial_hello_eof,
    scope_mismatch,
    enclosure_identity_mismatch,
    function_identity_mismatch,
    dynamic_status_delta,
    list_enclosure_identity_mismatch,
    list_function_identity_mismatch,
    function_order_delta,
    coalesced_hello_partial,
};

void set_text(w::Producer& producer) noexcept
{
    (void)set_text(producer.product, "px4-userland");
    producer.version.length = 5U;
    std::memcpy(producer.version.bytes.data(), "1.2.3", 5U);
}

w::ListResponse fixture_list(Mode mode = Mode::scope_mismatch) noexcept
{
    w::ListResponse list{};
    list.epoch[0] = 0x5aU;
    set_text(list.producer);
    list.topology.state = w::TopologyState::complete;
    list.topology.expected_function_count.present = true;
    list.topology.expected_function_count.value =
        mode == Mode::function_order_delta ? 2U : 1U;
    list.functions.count = 1U;
    auto& function = list.functions.items[0];
    function.id = 0U;
    function.vid = 0x1234U;
    function.pid = 0x5678U;
    function.mode = w::Mode::runtime;
    function.location.usb_path.present = true;
    function.location.usb_path.value = {};
    function.location.usb_path.value.length = 3U;
    std::memcpy(function.location.usb_path.value.bytes.data(), "1-2", 3U);
    function.serial_state = w::SerialState::present;
    function.serial.present = true;
    (void)set_text(function.serial.value, "function-serial");
    function.model.present = true;
    (void)set_text(function.model.value.id, "model-id");
    (void)set_text(function.model.value.name, "Model name");
    function.identification_reason = w::IdentificationReason::none;
    if (mode == Mode::function_order_delta) {
        list.functions.count = 2U;
        list.functions.items[1] = function;
        list.functions.items[1].id = 1U;
        std::memcpy(list.functions.items[1].location.usb_path.value.bytes.data(),
                    "1-3", 3U);
    }
    list.enclosure.present = true;
    list.enclosure.value.id = 0U;
    list.enclosure.value.model.present = true;
    (void)set_text(list.enclosure.value.model.value.id, "model-id");
    (void)set_text(list.enclosure.value.model.value.name, "Model name");
    list.enclosure.value.identity.serial_state = w::IdentitySerialState::present;
    list.enclosure.value.identity.serial.present = true;
    (void)set_text(list.enclosure.value.identity.serial.value, "enclosure-serial");
    list.enclosure.value.identity.uniqueness = w::Uniqueness::unique_in_scan;
    list.receivers.present = true;
    list.receivers.value.count = 1U;
    list.receivers.value.items[0].id = 0U;
    list.receivers.value.items[0].systems.mask = 1U;
    list.readers.present = true;
    list.readers.value.count = 0U;
    if (mode == Mode::list_enclosure_identity_mismatch)
        (void)set_text(list.enclosure.value.identity.serial.value, "other-serial");
    else if (mode == Mode::list_function_identity_mismatch)
        list.functions.items[0].pid = 0x4321U;
    return list;
}

w::StatusResponse fixture_status(const w::ListResponse& list, Mode mode) noexcept
{
    w::StatusResponse status{};
    status.producer = list.producer;
    status.context.epoch = list.epoch;
    status.context.state = w::ContextState::ready;
    status.context.instance.present = true;
    (void)set_text(status.context.instance.value, "instance");
    status.snapshot.sequence = 1U;
    status.snapshot.observed_at_ns = 5U;
    status.scope.kind = w::ScopeKind::enclosure;
    status.topology = list.topology;
    status.functions.present = true;
    status.functions.value = list.functions;
    status.receivers.present = true;
    status.receivers.value.count = 1U;
    status.receivers.value.items[0].descriptor = list.receivers.value.items[0];
    status.readers.present = true;
    status.readers.value.count = 0U;
    status.enclosure.present = true;
    status.enclosure.value = list.enclosure.value;
    status.enclosure.value.identity.uniqueness = w::Uniqueness::unknown;
    if (mode == Mode::scope_mismatch) {
        status.scope.kind = w::ScopeKind::receiver;
        status.scope.id = 0U;
        status.functions.present = false;
        status.readers.present = false;
    } else if (mode == Mode::enclosure_identity_mismatch) {
        (void)set_text(status.enclosure.value.identity.serial.value, "other-serial");
    } else if (mode == Mode::function_identity_mismatch) {
        status.functions.value.items[0].vid = 0x4321U;
    } else if (mode == Mode::dynamic_status_delta) {
        status.functions.value.items[0].present = false;
        status.enclosure.value.identity.uniqueness = w::Uniqueness::unknown;
    } else if (mode == Mode::function_order_delta) {
        std::swap(status.functions.value.items[0], status.functions.value.items[1]);
    }
    return status;
}

class FakeTransport final : public w::ClientTransport {
public:
    explicit FakeTransport(Mode mode, std::uint32_t error_code = 999U) noexcept
        : mode_(mode), error_code_(error_code) {}

    dtv::userland::Result<void> write_all(ByteView bytes, Timeout) noexcept override
    {
        const auto frame = w::decode_frame(bytes);
        if (!frame) return dtv::userland::Result<void>::failure(frame.error());
        ++request_count_;
        if (request_count_ == 1U && frame.value().header.opcode == w::kOpcodeHello)
            queue_hello(frame.value().header.request_id);
        else if (request_count_ == 2U && frame.value().header.opcode == w::kOpcodeList &&
                 mode_ == Mode::unknown_list_error)
            queue_remote_error(frame.value().header.request_id, error_code_);
        else if (request_count_ == 2U && frame.value().header.opcode == w::kOpcodeList &&
                 mode_ == Mode::remote_list_error)
            queue_remote_error(frame.value().header.request_id, error_code_);
        else if (request_count_ == 2U && frame.value().header.opcode == w::kOpcodeList &&
                 mode_ >= Mode::scope_mismatch) {
            const w::ListResponse list = fixture_list(mode_);
            const Error valid = w::validate_list_response(list);
            if (valid != Error::OK)
                std::fprintf(stderr, "fixture LIST invalid: %u\n",
                             static_cast<unsigned int>(valid));
            std::array<std::uint8_t, w::kMaxControlPayload> payload{};
            const auto size = w::encode_list_response(
                list, MutableByteView{payload.data(), payload.size()});
            if (!size) return dtv::userland::Result<void>::failure(size.error());
            queue_frame(w::kOpcodeList, frame.value().header.request_id,
                        ByteView{payload.data(), size.value()});
        }
        else if (request_count_ == 3U && frame.value().header.opcode == w::kOpcodeStatus &&
                 mode_ >= Mode::scope_mismatch) {
            const w::ListResponse list = fixture_list(
                mode_ == Mode::function_order_delta ? mode_ : Mode::scope_mismatch);
            const w::StatusResponse status = fixture_status(list, mode_);
            std::array<std::uint8_t, w::kMaxControlPayload> payload{};
            const auto size = w::encode_status_response(
                status, MutableByteView{payload.data(), payload.size()});
            if (!size) return dtv::userland::Result<void>::failure(size.error());
            queue_frame(w::kOpcodeStatus, frame.value().header.request_id,
                        ByteView{payload.data(), size.value()});
        }
        else if (mode_ != Mode::timeout_after_hello)
            return dtv::userland::Result<void>::failure(Error::PROTOCOL_ERROR);
        return dtv::userland::Result<void>::success();
    }

    dtv::userland::Result<std::size_t> read_some(MutableByteView output,
                                                 Timeout timeout) noexcept override
    {
        if (queued_offset_ < queued_size_) {
            const std::size_t count = std::min(output.size, queued_size_ - queued_offset_);
            std::memcpy(output.data, queued_.data() + queued_offset_, count);
            queued_offset_ += count;
            if (mode_ == Mode::partial_hello_eof && queued_size_ > 0U &&
                queued_offset_ == queued_size_)
                eof_ = true;
            return dtv::userland::Result<std::size_t>::success(count);
        }
        if (mode_ == Mode::timeout_after_hello && request_count_ == 2U) {
            now_ += timeout.milliseconds;
            return dtv::userland::Result<std::size_t>::failure(Error::TIMEOUT);
        }
        if (eof_ || mode_ == Mode::partial_hello_eof)
            return dtv::userland::Result<std::size_t>::failure(Error::DISCONNECTED);
        return dtv::userland::Result<std::size_t>::failure(Error::PROTOCOL_ERROR);
    }

    std::uint64_t monotonic_ms() noexcept override { return now_; }
    void close() noexcept override { ++close_count_; }
    std::size_t request_count() const noexcept { return request_count_; }
    std::size_t close_count() const noexcept { return close_count_; }

private:
    void queue_frame(std::uint16_t opcode, std::uint32_t request_id,
                     ByteView payload) noexcept
    {
        w::Header header{};
        header.kind = w::Kind::response;
        header.opcode = opcode;
        header.request_id = request_id;
        header.payload_length = static_cast<std::uint32_t>(payload.size);
        const auto size = w::encode_frame(
            header, payload, MutableByteView{queued_.data(), queued_.size()});
        if (size) {
            queued_size_ = size.value();
            queued_offset_ = 0U;
        }
    }

    void queue_hello(std::uint32_t request_id) noexcept
    {
        w::HelloResponse hello{};
        hello.major = w::kVersionMajor;
        hello.minor = w::kVersionMinor;
        hello.epoch[0] = 0x5aU;
        (void)set_text(hello.producer.product, "px4-userland");
        // The protocol's version field is 64 bytes, so copy it separately.
        hello.producer.version.length = 5U;
        std::memcpy(hello.producer.version.bytes.data(), "1.2.3", 5U);
        hello.max_control_payload = static_cast<std::uint32_t>(w::kMaxControlPayload);
        hello.max_ts_bytes = static_cast<std::uint32_t>(w::kMaxTsPacketBytes);
        std::array<std::uint8_t, 256U> payload{};
        const auto size = w::encode_hello_response(
            hello, MutableByteView{payload.data(), payload.size()});
        if (!size) return;
        queue_frame(w::kOpcodeHello, request_id, ByteView{payload.data(), size.value()});
        if (mode_ == Mode::partial_hello_eof) queued_size_ = 5U;
        if (mode_ == Mode::coalesced_hello_partial) {
            w::Header header{};
            header.kind = w::Kind::response;
            header.opcode = w::kOpcodeHello;
            header.request_id = request_id;
            header.payload_length = static_cast<std::uint32_t>(size.value());
            std::array<std::uint8_t, w::kHeaderSize + 256U> trailing{};
            const auto framed = w::encode_frame(
                header, ByteView{payload.data(), size.value()},
                MutableByteView{trailing.data(), trailing.size()});
            if (framed && framed.value() > 1U &&
                queued_size_ + framed.value() - 1U <= queued_.size()) {
                std::memcpy(queued_.data() + queued_size_, trailing.data(),
                            framed.value() - 1U);
                queued_size_ += framed.value() - 1U;
            }
        }
    }

    void queue_remote_error(std::uint32_t request_id, std::uint32_t code) noexcept
    {
        w::ErrorResponse error{};
        error.epoch[0] = 0x5aU;
        error.error.code = code;
        error.error.operation.length = 4U;
        std::memcpy(error.error.operation.bytes.data(), "LIST", 4U);
        std::array<std::uint8_t, 256U> payload{};
        const auto size = w::encode_error_response(
            error, MutableByteView{payload.data(), payload.size()});
        if (!size) {
            std::fprintf(stderr, "error response encode failed: %u\n",
                         static_cast<unsigned int>(size.error()));
            return;
        }
        w::Header header{};
        header.kind = w::Kind::error_response;
        header.opcode = w::kOpcodeList;
        header.request_id = request_id;
        header.payload_length = static_cast<std::uint32_t>(size.value());
        const auto framed = w::encode_frame(
            header, ByteView{payload.data(), size.value()},
            MutableByteView{queued_.data(), queued_.size()});
        if (framed) {
            queued_size_ = framed.value();
            queued_offset_ = 0U;
        }
    }

    Mode mode_;
    std::uint32_t error_code_;
    std::array<std::uint8_t, w::kHeaderSize + w::kMaxControlPayload> queued_{};
    std::size_t queued_size_ = 0U;
    std::size_t queued_offset_ = 0U;
    std::size_t request_count_ = 0U;
    std::size_t close_count_ = 0U;
    std::uint64_t now_ = 0U;
    bool eof_ = false;
};

bool test_unknown_error_closes_without_retry()
{
    FakeTransport transport(Mode::unknown_list_error);
    const auto result = w::fetch_status(transport, Timeout{100U});
    if (result || result.error != Error::PROTOCOL_ERROR || result.remote_error_code != 999U)
        std::fprintf(stderr, "unknown error result=%u remote=%u requests=%zu closes=%zu\n",
                     static_cast<unsigned int>(result.error),
                     static_cast<unsigned int>(result.remote_error_code),
                     transport.request_count(), transport.close_count());
    CHECK(!result && result.error == Error::PROTOCOL_ERROR &&
          result.remote_error_code == 999U);
    CHECK(transport.request_count() == 2U && transport.close_count() == 1U);
    return true;
}

bool test_remote_error_codebook()
{
    struct Case final { std::uint32_t code; Error error; };
    const Case cases[] = {
        {1U, Error::INVALID_ARGUMENT}, {2U, Error::VERSION_MISMATCH},
        {3U, Error::NOT_FOUND}, {4U, Error::BUSY}, {5U, Error::NOT_READY},
        {6U, Error::TIMEOUT}, {7U, Error::USB_IO}, {8U, Error::DISCONNECTED},
        {9U, Error::PROTOCOL_ERROR}, {10U, Error::FIRMWARE_REJECTED},
        {11U, Error::UNSUPPORTED}, {12U, Error::NO_CARD},
        {13U, Error::CARD_REMOVED}, {14U, Error::BUFFER_TOO_SMALL},
        {15U, Error::SLOW_CONSUMER}, {17U, Error::RESOURCE_LIMIT},
        {255U, Error::INTERNAL}, {16U, Error::PROTOCOL_ERROR},
    };
    for (const Case& item : cases) {
        FakeTransport transport(Mode::remote_list_error, item.code);
        const auto result = w::fetch_status(transport, Timeout{100U});
        CHECK(!result && result.error == item.error &&
              result.remote_error_code == item.code);
        CHECK(transport.request_count() == 2U && transport.close_count() == 1U);
    }
    return true;
}

bool test_status_is_bound_to_request_and_list_identity()
{
    const Mode rejected[] = {Mode::scope_mismatch,
                             Mode::enclosure_identity_mismatch,
                             Mode::function_identity_mismatch,
                             Mode::list_enclosure_identity_mismatch,
                             Mode::list_function_identity_mismatch};
    const w::ListResponse canonical = fixture_list();
    const w::StatusResponse partial = fixture_status(canonical, Mode::scope_mismatch);
    w::StatusContext partial_context{};
    partial_context.list = &canonical;
    CHECK(w::validate_status_response(partial, partial_context) == Error::OK);
    for (const Mode mode : rejected) {
        FakeTransport transport(mode);
        const auto result = w::fetch_status(transport, Timeout{100U});
        if (transport.request_count() != 3U || transport.close_count() != 1U)
            std::fprintf(stderr, "status fixture mode=%u error=%u requests=%zu closes=%zu\n",
                         static_cast<unsigned int>(mode),
                         static_cast<unsigned int>(result.error),
                         transport.request_count(), transport.close_count());
        CHECK(!result && result.error == Error::PROTOCOL_ERROR);
        CHECK(transport.request_count() == 3U && transport.close_count() == 1U);
    }
    FakeTransport dynamic(Mode::dynamic_status_delta);
    const auto result = w::fetch_status(dynamic, Timeout{100U});
    CHECK(result && result.error == Error::OK);
    CHECK(dynamic.request_count() == 3U && dynamic.close_count() == 0U);
    FakeTransport reordered(Mode::function_order_delta);
    const auto reordered_result = w::fetch_status(reordered, Timeout{100U});
    CHECK(reordered_result && reordered_result.error == Error::OK);
    return true;
}

bool test_timeout_closes_without_retry()
{
    FakeTransport transport(Mode::timeout_after_hello);
    const auto result = w::fetch_status(transport, Timeout{100U});
    CHECK(!result && result.error == Error::TIMEOUT);
    CHECK(transport.request_count() == 2U && transport.close_count() == 1U);
    return true;
}

bool test_partial_eof_closes_without_retry()
{
    FakeTransport transport(Mode::partial_hello_eof);
    const auto result = w::fetch_status(transport, Timeout{100U});
    CHECK(!result && result.error == Error::PROTOCOL_ERROR);
    CHECK(transport.request_count() == 1U && transport.close_count() == 1U);
    return true;
}

bool test_coalesced_reply_and_partial_next_frame_close_without_retry()
{
    FakeTransport transport(Mode::coalesced_hello_partial);
    const auto result = w::fetch_status(transport, Timeout{100U});
    CHECK(!result && result.error == Error::PROTOCOL_ERROR);
    CHECK(transport.request_count() == 1U && transport.close_count() == 1U);
    return true;
}

}  // namespace

int main()
{
    return test_unknown_error_closes_without_retry() && test_remote_error_codebook() &&
                   test_status_is_bound_to_request_and_list_identity() &&
                   test_timeout_closes_without_retry() &&
                   test_partial_eof_closes_without_retry() &&
                   test_coalesced_reply_and_partial_next_frame_close_without_retry()
               ? 0
               : 1;
}
