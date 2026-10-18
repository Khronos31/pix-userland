// SPDX-License-Identifier: MIT
#include <dtv/userland/control_connection.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>

namespace {

namespace w = dtv::userland::wire;
using dtv::userland::ByteView;
using dtv::userland::Error;
using dtv::userland::MutableByteView;

int g_failures = 0;
int g_checks = 0;

// Always active, independent of NDEBUG.
#define CHECK(expr)                                                              \
    do {                                                                         \
        ++g_checks;                                                              \
        if (!(expr)) {                                                           \
            ++g_failures;                                                        \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, \
                         #expr);                                                 \
        }                                                                        \
    } while (0)

constexpr std::size_t kFrameCapacity = w::kHeaderSize + w::kMaxControlPayload;

template <std::size_t N>
void set_text(w::WString<N>& out, const char* text)
{
    out.length = 0U;
    while (text[out.length] != '\0') {
        out.bytes[out.length] = text[out.length];
        ++out.length;
    }
}

w::ServerHello make_server()
{
    w::ServerHello server;
    for (std::size_t i = 0U; i < server.epoch.size(); ++i) {
        server.epoch[i] = static_cast<std::uint8_t>(0xa0U + i);
    }
    set_text(server.producer.product, "test-product");
    set_text(server.producer.version, "1.2.3");
    server.features = w::kFeatureStateEvent;
    return server;
}

// One request frame encoded into owned storage and decoded back, so the test
// feeds the connection exactly what a FrameReceiver delivers.
struct RequestFrame final {
    std::array<std::uint8_t, kFrameCapacity> bytes{};
    w::Frame frame{};
};

template <typename Record>
bool make_request(RequestFrame& out, std::uint16_t opcode, std::uint32_t request_id,
                  const Record& value,
                  dtv::userland::Result<std::size_t> (*encode)(const Record&,
                                                               MutableByteView) noexcept)
{
    std::array<std::uint8_t, w::kMaxControlPayload> payload{};
    const auto size = encode(value, MutableByteView{payload.data(), payload.size()});
    if (!size) return false;
    w::Header header;
    header.kind = w::Kind::request;
    header.opcode = opcode;
    header.request_id = request_id;
    header.payload_length = static_cast<std::uint32_t>(size.value());
    const auto written = w::encode_frame(header, ByteView{payload.data(), size.value()},
                                         MutableByteView{out.bytes.data(), out.bytes.size()});
    if (!written) return false;
    const auto decoded = w::decode_frame(ByteView{out.bytes.data(), written.value()});
    if (!decoded) return false;
    out.frame = decoded.value();
    return true;
}

bool make_raw_request(RequestFrame& out, std::uint16_t opcode, std::uint32_t request_id,
                      ByteView payload, w::Kind kind = w::Kind::request)
{
    w::Header header;
    header.kind = kind;
    header.opcode = opcode;
    header.request_id = request_id;
    header.payload_length = static_cast<std::uint32_t>(payload.size);
    const auto written = w::encode_frame(header, payload,
                                         MutableByteView{out.bytes.data(), out.bytes.size()});
    if (!written) return false;
    const auto decoded = w::decode_frame(ByteView{out.bytes.data(), written.value()});
    if (!decoded) return false;
    out.frame = decoded.value();
    return true;
}

w::HelloRequest hello_request(std::uint64_t required = 0U, std::uint64_t optional = 0U)
{
    w::HelloRequest hello;
    hello.min = w::Version{1U, 0U};
    hello.max = w::Version{1U, 0U};
    hello.required_features = required;
    hello.optional_features = optional;
    return hello;
}

struct Harness final {
    w::ServerHello server = make_server();
    std::unique_ptr<w::ControlConnection> connection{new w::ControlConnection(server)};
    std::unique_ptr<w::ControlRequest> request{new w::ControlRequest()};
    std::unique_ptr<std::array<std::uint8_t, kFrameCapacity>> output{
        new std::array<std::uint8_t, kFrameCapacity>()};
    std::unique_ptr<RequestFrame> frame{new RequestFrame()};

    w::Inbound accept()
    {
        return connection->accept(frame->frame, *request,
                                  MutableByteView{output->data(), output->size()});
    }

    bool hello(std::uint32_t request_id, std::uint64_t optional = 0U)
    {
        if (!make_request(*frame, w::kOpcodeHello, request_id, hello_request(0U, optional),
                          &w::encode_hello_request))
            return false;
        const w::Inbound inbound = accept();
        if (inbound.action != w::InboundAction::respond) return false;
        connection->complete();
        return true;
    }

    // Decodes the reply frame the connection wrote.
    bool reply(const w::Inbound& inbound, w::Frame& out) const
    {
        const auto decoded = w::decode_frame(ByteView{output->data(), inbound.reply_size});
        if (!decoded) return false;
        out = decoded.value();
        return true;
    }

    bool error_reply(const w::Inbound& inbound, std::uint32_t code, std::uint16_t opcode,
                     std::uint32_t request_id) const
    {
        w::Frame reply_frame{};
        if (inbound.action != w::InboundAction::respond || !reply(inbound, reply_frame))
            return false;
        if (reply_frame.header.kind != w::Kind::error_response ||
            reply_frame.header.opcode != opcode || reply_frame.header.request_id != request_id)
            return false;
        const auto error = w::decode_error_response(reply_frame.payload);
        return error && error.value().error.code == code && inbound.code == code &&
               error.value().epoch == server.epoch &&
               error.value().target.kind == w::ScopeKind::enclosure;
    }
};

void test_hello_negotiation()
{
    Harness h;
    CHECK(make_request(*h.frame, w::kOpcodeHello, 7U,
                       hello_request(0U, w::kFeatureStateEvent | w::kFeatureCardService |
                                             (1ULL << 40)),
                       &w::encode_hello_request));
    const w::Inbound inbound = h.accept();
    CHECK(inbound.action == w::InboundAction::respond && !inbound.close_after_reply &&
          inbound.code == 0U);
    w::Frame reply{};
    CHECK(h.reply(inbound, reply));
    CHECK(reply.header.kind == w::Kind::response && reply.header.opcode == w::kOpcodeHello &&
          reply.header.request_id == 7U);
    const auto hello = w::decode_hello_response(reply.payload);
    CHECK(hello);
    if (hello) {
        CHECK(hello.value().major == 1U && hello.value().minor == 0U);
        // Only the offered optional bit survives; unknown and unoffered bits drop.
        CHECK(hello.value().features == w::kFeatureStateEvent);
        CHECK(hello.value().epoch == h.server.epoch);
        CHECK(hello.value().max_control_payload == w::kMaxControlPayload);
        CHECK(hello.value().max_ts_bytes == w::kMaxTsPacketBytes);
        CHECK(hello.value().producer.product.length == 12U);
    }
    CHECK(h.connection->established() && h.connection->outstanding());
    CHECK(h.connection->features() == w::kFeatureStateEvent);
    // The reply must be completed before the next request is accepted.
    w::ListRequest list;
    list.epoch = h.server.epoch;
    CHECK(make_request(*h.frame, w::kOpcodeList, 8U, list, &w::encode_list_request));
    CHECK(h.accept().action == w::InboundAction::close);
}

void test_hello_failures()
{
    {
        // Required feature the server does not offer: UNSUPPORTED, then close.
        Harness h;
        CHECK(make_request(*h.frame, w::kOpcodeHello, 1U,
                           hello_request(w::kFeatureCardService), &w::encode_hello_request));
        const w::Inbound inbound = h.accept();
        CHECK(h.error_reply(inbound, static_cast<std::uint32_t>(Error::UNSUPPORTED),
                            w::kOpcodeHello, 1U));
        CHECK(inbound.close_after_reply && !h.connection->established());
    }
    {
        Harness h;
        w::HelloRequest hello = hello_request();
        hello.min = w::Version{2U, 0U};
        hello.max = w::Version{2U, 5U};
        CHECK(make_request(*h.frame, w::kOpcodeHello, 1U, hello, &w::encode_hello_request));
        const w::Inbound inbound = h.accept();
        CHECK(h.error_reply(inbound, static_cast<std::uint32_t>(Error::VERSION_MISMATCH),
                            w::kOpcodeHello, 1U));
        CHECK(inbound.close_after_reply);
    }
    {
        // HELLO is required first.
        Harness h;
        w::ListRequest list;
        list.epoch = h.server.epoch;
        CHECK(make_request(*h.frame, w::kOpcodeList, 1U, list, &w::encode_list_request));
        CHECK(h.accept().action == w::InboundAction::close);
    }
    {
        // A truncated HELLO payload is malformed: close without a reply.
        Harness h;
        const std::array<std::uint8_t, 4U> payload{1U, 0U, 1U, 0U};
        CHECK(make_raw_request(*h.frame, w::kOpcodeHello, 1U,
                               ByteView{payload.data(), payload.size()}));
        CHECK(h.accept().action == w::InboundAction::close);
    }
    {
        // HELLO only once.
        Harness h;
        CHECK(h.hello(1U));
        CHECK(make_request(*h.frame, w::kOpcodeHello, 2U, hello_request(),
                           &w::encode_hello_request));
        CHECK(h.accept().action == w::InboundAction::close);
    }
}

void test_request_rules()
{
    {
        // The same or an earlier request ID is a protocol error.
        Harness h;
        CHECK(h.hello(5U));
        w::ListRequest list;
        list.epoch = h.server.epoch;
        CHECK(make_request(*h.frame, w::kOpcodeList, 5U, list, &w::encode_list_request));
        CHECK(h.accept().action == w::InboundAction::close);
    }
    {
        Harness h;
        CHECK(h.hello(5U));
        w::ListRequest list;
        list.epoch = h.server.epoch;
        CHECK(make_request(*h.frame, w::kOpcodeList, 4U, list, &w::encode_list_request));
        CHECK(h.accept().action == w::InboundAction::close);
    }
    {
        // A response or event from the client is refused.
        Harness h;
        CHECK(h.hello(1U));
        CHECK(make_raw_request(*h.frame, w::kOpcodeList, 2U, ByteView{nullptr, 0U},
                               w::Kind::response));
        CHECK(h.accept().action == w::InboundAction::close);
        Harness e;
        CHECK(e.hello(1U));
        CHECK(make_raw_request(*e.frame, w::kOpcodeStateEvent, 0U, ByteView{nullptr, 0U},
                               w::Kind::event));
        CHECK(e.accept().action == w::InboundAction::close);
    }
    {
        // Unknown and unserved opcodes are UNSUPPORTED and the connection
        // continues; the next request is accepted after completion.
        Harness h;
        CHECK(h.hello(1U));
        const std::array<std::uint8_t, 3U> anything{1U, 2U, 3U};
        CHECK(make_raw_request(*h.frame, 0x0123U, 2U,
                               ByteView{anything.data(), anything.size()}));
        w::Inbound inbound = h.accept();
        CHECK(h.error_reply(inbound, static_cast<std::uint32_t>(Error::UNSUPPORTED), 0x0123U,
                            2U));
        CHECK(!inbound.close_after_reply);
        h.connection->complete();
        w::StartRequest start;
        start.epoch = h.server.epoch;
        start.lease_id = 1U;
        start.tune_revision = 1U;
        CHECK(make_request(*h.frame, w::kOpcodeStartStream, 3U, start,
                           &w::encode_start_request));
        inbound = h.accept();
        CHECK(h.error_reply(inbound, static_cast<std::uint32_t>(Error::UNSUPPORTED),
                            w::kOpcodeStartStream, 3U));
        h.connection->complete();
        w::ListRequest list;
        list.epoch = h.server.epoch;
        CHECK(make_request(*h.frame, w::kOpcodeList, 4U, list, &w::encode_list_request));
        CHECK(h.accept().action == w::InboundAction::dispatch);
        CHECK(h.request->kind == w::ControlRequestKind::list && h.request->request_id == 4U);
    }
    {
        // Malformed payload of a served opcode closes; a semantically invalid
        // one is answered with INVALID_ARGUMENT.
        Harness h;
        CHECK(h.hello(1U));
        const std::array<std::uint8_t, 3U> short_list{0U, 0U, 0U};
        CHECK(make_raw_request(*h.frame, w::kOpcodeList, 2U,
                               ByteView{short_list.data(), short_list.size()}));
        CHECK(h.accept().action == w::InboundAction::close);

        Harness t;
        CHECK(t.hello(1U));
        w::TuneRequest tune;
        tune.epoch = t.server.epoch;
        tune.lease_id = 1U;
        tune.system = w::System::isdb_t;
        tune.frequency_hz = 473142857U;
        tune.timeout_ms = 1000U;
        CHECK(make_request(*t.frame, w::kOpcodeTune, 2U, tune, &w::encode_tune_request));
        // ISDB-T with a TSID selection: valid layout, invalid value.
        t.frame->bytes[w::kHeaderSize + 16U + 8U + 1U + 8U] =
            static_cast<std::uint8_t>(w::Selection::tsid);
        const auto redecoded = w::decode_frame(
            ByteView{t.frame->bytes.data(), w::kHeaderSize + t.frame->frame.payload.size});
        CHECK(redecoded);
        if (redecoded) t.frame->frame = redecoded.value();
        const w::Inbound inbound = t.accept();
        CHECK(t.error_reply(inbound, static_cast<std::uint32_t>(Error::INVALID_ARGUMENT),
                            w::kOpcodeTune, 2U));
        CHECK(!inbound.close_after_reply);
    }
}

void test_epoch_binding_and_dispatch()
{
    Harness h;
    CHECK(h.hello(1U));
    w::AcquireRequest acquire;
    acquire.epoch = h.server.epoch;
    acquire.epoch[3] ^= 0x01U;
    acquire.receiver_id = 513U;
    CHECK(make_request(*h.frame, w::kOpcodeAcquire, 2U, acquire, &w::encode_acquire_request));
    w::Inbound inbound = h.accept();
    CHECK(h.error_reply(inbound, w::kWireErrorStaleHandle, w::kOpcodeAcquire, 2U));
    h.connection->complete();

    acquire.epoch = h.server.epoch;
    CHECK(make_request(*h.frame, w::kOpcodeAcquire, 3U, acquire, &w::encode_acquire_request));
    inbound = h.accept();
    CHECK(inbound.action == w::InboundAction::dispatch);
    CHECK(h.request->kind == w::ControlRequestKind::acquire &&
          h.request->acquire.receiver_id == 513U && h.request->opcode == w::kOpcodeAcquire);
    h.connection->complete();

    w::StatusRequest status;
    status.epoch = h.server.epoch;
    status.scope = w::Scope{w::ScopeKind::receiver, 0U};
    CHECK(make_request(*h.frame, w::kOpcodeStatus, 4U, status, &w::encode_status_request));
    inbound = h.accept();
    CHECK(inbound.action == w::InboundAction::dispatch &&
          h.request->kind == w::ControlRequestKind::status &&
          h.request->status.scope.kind == w::ScopeKind::receiver &&
          h.request->status.scope.id == 0U);
    h.connection->complete();

    w::ReleaseRequest release;
    release.epoch = h.server.epoch;
    release.lease_id = 9U;
    CHECK(make_request(*h.frame, w::kOpcodeRelease, 5U, release, &w::encode_release_request));
    inbound = h.accept();
    CHECK(inbound.action == w::InboundAction::dispatch &&
          h.request->kind == w::ControlRequestKind::release && h.request->release.lease_id == 9U);
}

void test_pid_filter_requires_negotiation()
{
    Harness unselected;
    unselected.server.features |= w::kFeaturePidFilter;
    CHECK(unselected.hello(1U));
    w::TuneRequest tune;
    tune.epoch = unselected.server.epoch;
    tune.lease_id = 1U;
    tune.system = w::System::isdb_t;
    tune.frequency_hz = 473142857U;
    tune.timeout_ms = 1000U;
    tune.pids.present = true;
    tune.pids.value.count = 1U;
    tune.pids.value.items[0] = 256U;
    CHECK(make_request(*unselected.frame, w::kOpcodeTune, 2U, tune,
                       &w::encode_tune_request));
    w::Inbound inbound = unselected.accept();
    CHECK(inbound.action == w::InboundAction::respond);
    CHECK(unselected.error_reply(inbound, static_cast<std::uint32_t>(Error::UNSUPPORTED),
                                 w::kOpcodeTune, 2U));
    unselected.connection->complete();

    Harness selected;
    selected.server.features |= w::kFeaturePidFilter;
    CHECK(selected.hello(1U, w::kFeaturePidFilter));
    CHECK(selected.connection->features() == w::kFeaturePidFilter);
    tune.epoch = selected.server.epoch;
    CHECK(make_request(*selected.frame, w::kOpcodeTune, 2U, tune,
                       &w::encode_tune_request));
    inbound = selected.accept();
    CHECK(inbound.action == w::InboundAction::dispatch);
    CHECK(selected.request->kind == w::ControlRequestKind::tune &&
          selected.request->tune.pids.present);
}

// Synthetic context with sparse public IDs (including 0) and a known-empty
// reader set: generic dispatcher coverage, not a product topology.
void sparse_list(w::ListResponse& list, const w::Uuid& epoch)
{
    list = w::ListResponse{};
    list.epoch = epoch;
    set_text(list.producer.product, "test-product");
    set_text(list.producer.version, "1.2.3");
    list.topology.state = w::TopologyState::complete;
    list.topology.expected_function_count.present = true;
    list.topology.expected_function_count.value = 1U;
    list.enclosure.present = true;
    list.enclosure.value.identity.serial_state = w::IdentitySerialState::absent;
    list.enclosure.value.identity.uniqueness = w::Uniqueness::not_applicable;
    list.functions.count = 1U;
    list.functions.items[0].id = 0U;
    list.functions.items[0].vid = 0x1234U;
    list.functions.items[0].pid = 0x5678U;
    list.functions.items[0].mode = w::Mode::runtime;
    list.functions.items[0].serial_state = w::SerialState::absent;
    list.functions.items[0].present = true;
    list.functions.items[0].identification_reason = w::IdentificationReason::model_unconfirmed;
    list.receivers.present = true;
    list.receivers.value.count = 3U;
    const std::uint16_t ids[3] = {0U, 7U, 513U};
    for (std::size_t i = 0U; i < 3U; ++i) {
        w::ReceiverDescriptor& receiver = list.receivers.value.items[i];
        receiver.id = ids[i];
        receiver.systems.mask = 0x01U;
        receiver.binding.present = true;
        receiver.binding.value = w::Binding{0U, static_cast<std::uint16_t>(i)};
    }
    list.readers.present = true;
    list.readers.value.count = 0U;
}

void test_status_shell()
{
    w::ServerHello server = make_server();
    std::unique_ptr<w::ListResponse> list(new w::ListResponse());
    sparse_list(*list, server.epoch);
    CHECK(w::validate_list_response(*list) == Error::OK);

    w::Context context;
    context.epoch = server.epoch;
    context.state = w::ContextState::ready;
    const w::Snapshot snapshot{3U, 5000U};
    std::unique_ptr<w::StatusResponse> status(new w::StatusResponse());

    CHECK(w::check_status_scope(*list, w::Scope{w::ScopeKind::receiver, 513U}) == Error::OK);
    CHECK(w::check_status_scope(*list, w::Scope{w::ScopeKind::receiver, 0U}) == Error::OK);
    CHECK(w::check_status_scope(*list, w::Scope{w::ScopeKind::receiver, 1U}) ==
          Error::NOT_FOUND);
    CHECK(w::check_status_scope(*list, w::Scope{w::ScopeKind::reader, 0U}) == Error::NOT_FOUND);
    CHECK(w::check_status_scope(*list, w::Scope{w::ScopeKind::stream, 1U}) == Error::NOT_FOUND);
    CHECK(w::check_status_scope(*list, w::Scope{w::ScopeKind::receiver, 0x10000U}) ==
          Error::INVALID_ARGUMENT);

    std::unique_ptr<std::array<std::uint8_t, kFrameCapacity>> output(
        new std::array<std::uint8_t, kFrameCapacity>());
    const MutableByteView out{output->data(), output->size()};

    CHECK(w::build_status_shell(*list, context, snapshot, w::Scope{w::ScopeKind::enclosure, 0U},
                                *status) == Error::OK);
    CHECK(status->enclosure.present &&
          status->enclosure.value.identity.uniqueness == w::Uniqueness::not_applicable);
    CHECK(status->functions.present && status->functions.value.count == 1U);
    CHECK(status->receivers.present && status->receivers.value.count == 3U);
    CHECK(status->readers.present && status->readers.value.count == 0U);
    CHECK(status->receivers.value.items[2].descriptor.id == 513U);
    const auto encoded = w::encode_status_frame(9U, *status, *list, out);
    CHECK(encoded);
    if (encoded) {
        const auto frame = w::decode_frame(ByteView{output->data(), encoded.value()});
        CHECK(frame && frame.value().header.kind == w::Kind::response &&
              frame.value().header.opcode == w::kOpcodeStatus &&
              frame.value().header.request_id == 9U);
        if (frame) {
            const auto decoded = w::decode_status_response(frame.value().payload);
            CHECK(decoded && decoded.value().receivers.value.count == 3U &&
                  decoded.value().snapshot.sequence == 3U);
        }
    }

    w::ListResponse unique_list = *list;
    unique_list.enclosure.value.identity.serial_state = w::IdentitySerialState::present;
    unique_list.enclosure.value.identity.serial.present = true;
    set_text(unique_list.enclosure.value.identity.serial.value, "scan-unique-serial");
    unique_list.enclosure.value.identity.uniqueness = w::Uniqueness::unique_in_scan;
    CHECK(w::validate_list_response(unique_list) == Error::OK);
    CHECK(w::build_status_shell(unique_list, context, snapshot,
                                w::Scope{w::ScopeKind::enclosure, 0U}, *status) == Error::OK);
    CHECK(status->enclosure.present &&
          status->enclosure.value.identity.serial_state == w::IdentitySerialState::present &&
          status->enclosure.value.identity.uniqueness == w::Uniqueness::unknown);
    CHECK(w::encode_status_frame(13U, *status, unique_list, out));

    CHECK(w::build_status_shell(*list, context, snapshot, w::Scope{w::ScopeKind::receiver, 7U},
                                *status) == Error::OK);
    CHECK(!status->functions.present && !status->readers.present &&
          status->receivers.value.count == 1U && status->receivers.value.items[0].descriptor.id == 7U);
    CHECK(w::encode_status_frame(10U, *status, *list, out));

    // A projection that disagrees with the canonical descriptor or epoch is
    // refused before anything is encoded.
    status->receivers.value.items[0].descriptor.systems.mask = 0x03U;
    CHECK(!w::encode_status_frame(11U, *status, *list, out));
    CHECK(w::build_status_shell(*list, context, snapshot, w::Scope{w::ScopeKind::enclosure, 0U},
                                *status) == Error::OK);
    status->context.epoch[0] ^= 0x01U;
    CHECK(!w::encode_status_frame(12U, *status, *list, out));
}

}  // namespace

int main()
{
    test_hello_negotiation();
    test_hello_failures();
    test_request_rules();
    test_epoch_binding_and_dispatch();
    test_pid_filter_requires_negotiation();
    test_status_shell();
    std::printf("control_connection_tests: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
