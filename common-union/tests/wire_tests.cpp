// SPDX-License-Identifier: MIT
#include <dtv/userland/wire.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>

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

template <std::size_t N>
ByteView view(const std::array<std::uint8_t, N>& bytes)
{
    return ByteView{bytes.data(), N};
}

template <std::size_t N>
MutableByteView mutable_view(std::array<std::uint8_t, N>& bytes)
{
    return MutableByteView{bytes.data(), N};
}

template <std::size_t N>
bool equals(const std::uint8_t* data, std::size_t size, const std::array<std::uint8_t, N>& expected)
{
    if (size != N) {
        return false;
    }
    for (std::size_t i = 0U; i < N; ++i) {
        if (data[i] != expected[i]) {
            return false;
        }
    }
    return true;
}

// Builds a 20-byte header by hand: magic, major, minor, opcode, flags,
// request_id, payload_length, each written little-endian.
std::array<std::uint8_t, w::kHeaderSize> raw_header(std::uint8_t m0, std::uint8_t m1,
                                                    std::uint8_t m2, std::uint8_t m3,
                                                    std::uint16_t major, std::uint16_t minor,
                                                    std::uint16_t opcode, std::uint16_t flags,
                                                    std::uint32_t request_id,
                                                    std::uint32_t payload_length)
{
    return {m0,
            m1,
            m2,
            m3,
            static_cast<std::uint8_t>(major),
            static_cast<std::uint8_t>(major >> 8),
            static_cast<std::uint8_t>(minor),
            static_cast<std::uint8_t>(minor >> 8),
            static_cast<std::uint8_t>(opcode),
            static_cast<std::uint8_t>(opcode >> 8),
            static_cast<std::uint8_t>(flags),
            static_cast<std::uint8_t>(flags >> 8),
            static_cast<std::uint8_t>(request_id),
            static_cast<std::uint8_t>(request_id >> 8),
            static_cast<std::uint8_t>(request_id >> 16),
            static_cast<std::uint8_t>(request_id >> 24),
            static_cast<std::uint8_t>(payload_length),
            static_cast<std::uint8_t>(payload_length >> 8),
            static_cast<std::uint8_t>(payload_length >> 16),
            static_cast<std::uint8_t>(payload_length >> 24)};
}

std::array<std::uint8_t, w::kHeaderSize> udtv(std::uint16_t opcode, std::uint16_t flags,
                                              std::uint32_t request_id,
                                              std::uint32_t payload_length)
{
    return raw_header('U', 'D', 'T', 'V', 1U, 0U, opcode, flags, request_id, payload_length);
}

w::HeaderStatus status_of(const std::array<std::uint8_t, w::kHeaderSize>& bytes)
{
    w::Header header;
    return w::check_header(view(bytes), &header);
}

void test_header_vectors()
{
    // HELLO request, request_id 0x01020304, 24-byte payload.
    const std::array<std::uint8_t, 20> hello = {'U', 'D', 'T', 'V', 0x01, 0x00, 0x00,
                                                0x00, 0x01, 0x00, 0x00, 0x00, 0x04, 0x03,
                                                0x02, 0x01, 0x18, 0x00, 0x00, 0x00};
    const auto decoded = w::decode_header(view(hello));
    CHECK(decoded.has_value());
    CHECK(decoded.value().kind == w::Kind::request);
    CHECK(decoded.value().opcode == 0x0001U);
    CHECK(decoded.value().request_id == 0x01020304U);
    CHECK(decoded.value().payload_length == 24U);

    std::array<std::uint8_t, 20> out{};
    w::Header header;
    header.kind = w::Kind::request;
    header.opcode = w::kOpcodeHello;
    header.request_id = 0x01020304U;
    header.payload_length = 24U;
    const auto written = w::encode_header(header, mutable_view(out));
    CHECK(written.has_value() && written.value() == 20U);
    CHECK(equals(out.data(), out.size(), hello));

    // ACQUIRE response: flags 0x0001.
    const std::array<std::uint8_t, 20> response = {'U', 'D', 'T', 'V', 0x01, 0x00, 0x00,
                                                   0x00, 0x20, 0x00, 0x01, 0x00, 0xff, 0x00,
                                                   0x00, 0x00, 0x18, 0x00, 0x00, 0x00};
    const auto r = w::decode_header(view(response));
    CHECK(r.has_value() && r.value().kind == w::Kind::response &&
          r.value().request_id == 255U && r.value().payload_length == 24U);
    header = w::Header{w::Kind::response, w::kOpcodeAcquire, 255U, 24U};
    CHECK(w::encode_header(header, mutable_view(out)).has_value());
    CHECK(equals(out.data(), out.size(), response));

    // Error response: flags 0x0003.
    const std::array<std::uint8_t, 20> error = {'U', 'D', 'T', 'V', 0x01, 0x00, 0x00,
                                                0x00, 0x20, 0x00, 0x03, 0x00, 0x07, 0x00,
                                                0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    const auto e = w::decode_header(view(error));
    CHECK(e.has_value() && e.value().kind == w::Kind::error_response &&
          e.value().request_id == 7U && e.value().payload_length == 0U);
    header = w::Header{w::Kind::error_response, w::kOpcodeAcquire, 7U, 0U};
    CHECK(w::encode_header(header, mutable_view(out)).has_value());
    CHECK(equals(out.data(), out.size(), error));

    // TS_DATA event: flags 0, request_id 0, opcode 0x8040, payload 0x000fffb8
    // (1048504, the TS_DATA cap).
    const std::array<std::uint8_t, 20> event = {'U', 'D', 'T', 'V', 0x01, 0x00, 0x00,
                                                0x00, 0x40, 0x80, 0x00, 0x00, 0x00, 0x00,
                                                0x00, 0x00, 0xb8, 0xff, 0x0f, 0x00};
    const auto ev = w::decode_header(view(event));
    CHECK(ev.has_value() && ev.value().kind == w::Kind::event &&
          ev.value().opcode == w::kOpcodeTsData && ev.value().request_id == 0U &&
          ev.value().payload_length == 1048504U);
    header = w::Header{w::Kind::event, w::kOpcodeTsData, 0U, 0x000fffb8U};
    CHECK(w::encode_header(header, mutable_view(out)).has_value());
    CHECK(equals(out.data(), out.size(), event));
}

void test_magic_and_version()
{
    w::Header header;
    header.opcode = 0x1234U;
    auto legacy = raw_header('P', 'X', '4', 'U', 1U, 0U, w::kOpcodeHello, 0U, 1U, 12U);
    CHECK(w::check_header(view(legacy), &header) == w::HeaderStatus::legacy_magic);
    CHECK(header.opcode == 0x1234U);  // untouched on failure
    CHECK(w::decode_header(view(legacy)).error() == Error::PROTOCOL_ERROR);
    // ASICEN legacy magic, otherwise a perfectly valid UDTV-looking header.
    const auto asicen = raw_header('A', 'S', 'C', 'N', 1U, 0U, w::kOpcodeHello, 0U, 1U, 24U);
    CHECK(status_of(asicen) == w::HeaderStatus::legacy_magic);
    CHECK(w::decode_header(view(asicen)).error() == Error::PROTOCOL_ERROR);
    CHECK(status_of(raw_header('A', 'S', 'C', 'n', 1U, 0U, 1U, 0U, 1U, 0U)) ==
          w::HeaderStatus::bad_magic);

    CHECK(status_of(raw_header('U', 'D', 'T', 'v', 1U, 0U, 1U, 0U, 1U, 0U)) ==
          w::HeaderStatus::bad_magic);
    CHECK(status_of(raw_header('V', 'T', 'D', 'U', 1U, 0U, 1U, 0U, 1U, 0U)) ==
          w::HeaderStatus::bad_magic);
    CHECK(status_of(raw_header(0, 0, 0, 0, 1U, 0U, 1U, 0U, 1U, 0U)) ==
          w::HeaderStatus::bad_magic);

    const std::uint16_t versions[][2] = {{1U, 1U}, {0U, 0U}, {2U, 0U}, {0U, 1U}, {0x0100U, 0U}};
    for (const auto& v : versions) {
        const auto bytes = raw_header('U', 'D', 'T', 'V', v[0], v[1], 1U, 0U, 1U, 0U);
        CHECK(status_of(bytes) == w::HeaderStatus::unsupported_version);
        CHECK(w::decode_header(view(bytes)).error() == Error::VERSION_MISMATCH);
    }
}

void test_flags_and_ids()
{
    const std::uint16_t bad_flags[] = {0x0002U, 0x0004U, 0x0005U, 0x0007U, 0x0100U, 0x8000U,
                                       0xffffU};
    for (const auto flags : bad_flags) {
        CHECK(status_of(udtv(0x0010U, flags, 1U, 0U)) == w::HeaderStatus::bad_flags);
        CHECK(status_of(udtv(0x0010U, flags, 0U, 0U)) == w::HeaderStatus::bad_flags);
    }
    CHECK(status_of(udtv(0x0010U, 0x0001U, 0U, 0U)) == w::HeaderStatus::bad_request_id);
    CHECK(status_of(udtv(0x0010U, 0x0003U, 0U, 0U)) == w::HeaderStatus::bad_request_id);
    CHECK(status_of(udtv(0x0010U, 0x0000U, 9U, 0U)) == w::HeaderStatus::ok);
    CHECK(status_of(udtv(0x80f0U, 0x0000U, 0U, 0U)) == w::HeaderStatus::ok);

    // The two cases named by review: a normal opcode shaped as an event, and an
    // event opcode shaped as a response. Both are malformed envelopes.
    const auto list_as_event = udtv(0x0010U, 0U, 0U, 0U);
    CHECK(w::decode_header(view(list_as_event)).error() == Error::PROTOCOL_ERROR);
    CHECK(w::decode_frame(view(list_as_event)).error() == Error::PROTOCOL_ERROR);
    const auto stream_end_as_response = udtv(0x80ffU, 1U, 1U, 0U);
    CHECK(w::decode_header(view(stream_end_as_response)).error() == Error::PROTOCOL_ERROR);
    CHECK(w::decode_frame(view(stream_end_as_response)).error() == Error::PROTOCOL_ERROR);
    CHECK(status_of(stream_end_as_response) == w::HeaderStatus::bad_kind_for_opcode);

    // Generic bit 15 rule over known and unknown opcodes on both sides.
    // Normal: HELLO, STATUS, ACQUIRE, and unknown 0x0099/0x7abc/0x7fff.
    // Event: TS_DATA, STATE_EVENT, STREAM_END, and unknown 0x8000/0x8123/0xffff.
    const std::uint16_t normal_opcodes[] = {0x0001U, 0x0011U, 0x0020U, 0x0099U, 0x7abcU, 0x7fffU};
    const std::uint16_t event_opcodes[] = {0x8040U, 0x80f0U, 0x80ffU, 0x8000U, 0x8123U, 0xffffU};
    struct Shape {
        std::uint16_t flags;
        std::uint32_t request_id;
        w::HeaderStatus normal;
        w::HeaderStatus event;
        w::Kind kind;
    };
    const Shape shapes[] = {
        {0U, 1U, w::HeaderStatus::ok, w::HeaderStatus::bad_request_id, w::Kind::request},
        {1U, 1U, w::HeaderStatus::ok, w::HeaderStatus::bad_kind_for_opcode, w::Kind::response},
        {3U, 1U, w::HeaderStatus::ok, w::HeaderStatus::bad_kind_for_opcode,
         w::Kind::error_response},
        {0U, 0U, w::HeaderStatus::bad_request_id, w::HeaderStatus::ok, w::Kind::event},
        {1U, 0U, w::HeaderStatus::bad_request_id, w::HeaderStatus::bad_kind_for_opcode,
         w::Kind::response},
        {3U, 0U, w::HeaderStatus::bad_request_id, w::HeaderStatus::bad_kind_for_opcode,
         w::Kind::error_response},
        {0U, 0xffffffffU, w::HeaderStatus::ok, w::HeaderStatus::bad_request_id, w::Kind::request},
    };
    for (const auto& shape : shapes) {
        for (const auto opcode : normal_opcodes) {
            const auto bytes = udtv(opcode, shape.flags, shape.request_id, 0U);
            w::Header header;
            CHECK(w::check_header(view(bytes), &header) == shape.normal);
            const auto decoded = w::decode_header(view(bytes));
            if (shape.normal == w::HeaderStatus::ok) {
                CHECK(decoded.has_value() && decoded.value().kind == shape.kind &&
                      decoded.value().opcode == opcode &&
                      decoded.value().request_id == shape.request_id);
            } else {
                CHECK(!decoded.has_value() && decoded.error() == Error::PROTOCOL_ERROR);
            }
        }
        for (const auto opcode : event_opcodes) {
            const auto bytes = udtv(opcode, shape.flags, shape.request_id, 0U);
            w::Header header;
            CHECK(w::check_header(view(bytes), &header) == shape.event);
            const auto decoded = w::decode_header(view(bytes));
            if (shape.event == w::HeaderStatus::ok) {
                CHECK(decoded.has_value() && decoded.value().kind == w::Kind::event &&
                      decoded.value().opcode == opcode && decoded.value().request_id == 0U);
            } else {
                CHECK(!decoded.has_value() && decoded.error() == Error::PROTOCOL_ERROR);
            }
        }
    }
    CHECK(w::is_event_opcode(0x8000U) && w::is_event_opcode(0xffffU));
    CHECK(!w::is_event_opcode(0x7fffU) && !w::is_event_opcode(0x0000U));

    // Encoder enforces the same combinations.
    std::array<std::uint8_t, 20> out{};
    out.fill(0xaaU);
    CHECK(w::encode_header(w::Header{w::Kind::event, 0x80f0U, 3U, 0U}, mutable_view(out))
              .error() == Error::INVALID_ARGUMENT);
    CHECK(w::encode_header(w::Header{w::Kind::request, 0x0010U, 0U, 0U}, mutable_view(out))
              .error() == Error::INVALID_ARGUMENT);
    CHECK(w::encode_header(w::Header{w::Kind::response, 0x0010U, 0U, 0U}, mutable_view(out))
              .error() == Error::INVALID_ARGUMENT);
    CHECK(w::encode_header(w::Header{w::Kind::error_response, 0x0010U, 0U, 0U},
                           mutable_view(out))
              .error() == Error::INVALID_ARGUMENT);
    CHECK(w::encode_header(w::Header{w::Kind::event, w::kOpcodeHello, 0U, 0U}, mutable_view(out))
              .error() == Error::INVALID_ARGUMENT);
    CHECK(w::encode_header(w::Header{w::Kind::event, 0x0010U, 0U, 0U}, mutable_view(out))
              .error() == Error::INVALID_ARGUMENT);
    CHECK(w::encode_header(w::Header{w::Kind::request, w::kOpcodeTsData, 1U, 0U},
                           mutable_view(out))
              .error() == Error::INVALID_ARGUMENT);
    CHECK(w::encode_header(w::Header{w::Kind::response, 0x80ffU, 1U, 0U}, mutable_view(out))
              .error() == Error::INVALID_ARGUMENT);
    CHECK(w::encode_header(w::Header{w::Kind::error_response, 0x8123U, 1U, 0U},
                           mutable_view(out))
              .error() == Error::INVALID_ARGUMENT);
    CHECK(w::encode_header(w::Header{static_cast<w::Kind>(9), 0x0010U, 1U, 0U},
                           mutable_view(out))
              .error() == Error::INVALID_ARGUMENT);
    CHECK(out[0] == 0xaaU && out[19] == 0xaaU);

    // Valid unknown event and unknown request encode to independently formed bytes.
    const std::array<std::uint8_t, 20> unknown_event = {'U', 'D', 'T', 'V', 0x01, 0x00, 0x00,
                                                        0x00, 0x23, 0x81, 0x00, 0x00, 0x00, 0x00,
                                                        0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    CHECK(w::encode_header(w::Header{w::Kind::event, 0x8123U, 0U, 0U}, mutable_view(out))
              .has_value());
    CHECK(equals(out.data(), out.size(), unknown_event));
    const std::array<std::uint8_t, 20> unknown_request = {'U', 'D', 'T', 'V', 0x01, 0x00, 0x00,
                                                          0x00, 0xff, 0x7f, 0x00, 0x00, 0x09, 0x00,
                                                          0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    CHECK(w::encode_header(w::Header{w::Kind::request, 0x7fffU, 9U, 0U}, mutable_view(out))
              .has_value());
    CHECK(equals(out.data(), out.size(), unknown_request));
}

void test_payload_caps()
{
    CHECK(w::kMaxTsDataPayload == 1048504U);
    CHECK(w::kMaxTsPacketBytes % w::kTsPacketSize == 0U);
    CHECK(w::kMaxTsPacketBytes + w::kTsPacketSize > 1048576U);
    CHECK(w::payload_limit(w::Kind::event, w::kOpcodeTsData) == 1048504U);
    CHECK(w::payload_limit(w::Kind::event, 0x80f0U) == 65536U);
    CHECK(w::payload_limit(w::Kind::request, 0x0010U) == 65536U);

    CHECK(status_of(udtv(0x0010U, 0U, 1U, 65536U)) == w::HeaderStatus::ok);
    CHECK(status_of(udtv(0x0010U, 0U, 1U, 65537U)) == w::HeaderStatus::payload_too_large);
    CHECK(status_of(udtv(0x0010U, 1U, 1U, 65537U)) == w::HeaderStatus::payload_too_large);
    CHECK(status_of(udtv(0x80f0U, 0U, 0U, 65536U)) == w::HeaderStatus::ok);
    CHECK(status_of(udtv(0x80f0U, 0U, 0U, 65537U)) == w::HeaderStatus::payload_too_large);
    // The TS_DATA cap is the whole payload (prefix + packets), not the packet cap.
    CHECK(status_of(udtv(w::kOpcodeTsData, 0U, 0U, 1048476U)) == w::HeaderStatus::ok);
    CHECK(status_of(udtv(w::kOpcodeTsData, 0U, 0U, 1048504U)) == w::HeaderStatus::ok);
    CHECK(status_of(udtv(w::kOpcodeTsData, 0U, 0U, 1048505U)) ==
          w::HeaderStatus::payload_too_large);
    CHECK(status_of(udtv(w::kOpcodeTsData, 0U, 0U, 1048596U)) ==
          w::HeaderStatus::payload_too_large);
    CHECK(status_of(udtv(w::kOpcodeTsData, 0U, 0U, 0xffffffffU)) ==
          w::HeaderStatus::payload_too_large);
    CHECK(w::decode_header(view(udtv(0x0010U, 0U, 1U, 0xffffffffU))).error() ==
          Error::PROTOCOL_ERROR);

    std::array<std::uint8_t, 20> out{};
    CHECK(w::encode_header(w::Header{w::Kind::request, 0x0010U, 1U, 65537U}, mutable_view(out))
              .error() == Error::INVALID_ARGUMENT);
    CHECK(w::encode_header(w::Header{w::Kind::event, w::kOpcodeTsData, 0U, 1048504U},
                           mutable_view(out))
              .has_value());
}

void test_views_and_sizes()
{
    w::Header header;
    CHECK(w::check_header(ByteView{nullptr, 20U}, &header) == w::HeaderStatus::invalid_view);
    const auto good = udtv(0x0010U, 0U, 1U, 0U);
    CHECK(w::check_header(view(good), nullptr) == w::HeaderStatus::invalid_view);
    CHECK(w::decode_header(ByteView{nullptr, 20U}).error() == Error::INVALID_ARGUMENT);
    CHECK(w::decode_header(ByteView{good.data(), 19U}).error() == Error::INVALID_ARGUMENT);
    CHECK(w::decode_header(ByteView{nullptr, 0U}).error() == Error::INVALID_ARGUMENT);
    std::array<std::uint8_t, 21> longer{};
    for (std::size_t i = 0U; i < 20U; ++i) {
        longer[i] = good[i];
    }
    CHECK(w::decode_header(view(longer)).error() == Error::INVALID_ARGUMENT);

    CHECK(w::decode_frame(ByteView{nullptr, 25U}).error() == Error::INVALID_ARGUMENT);
    CHECK(w::decode_frame(ByteView{nullptr, 0U}).error() == Error::PROTOCOL_ERROR);
    CHECK(w::encode_header(w::Header{w::Kind::request, 1U, 1U, 12U}, MutableByteView{nullptr, 20U})
              .error() == Error::INVALID_ARGUMENT);
    CHECK(w::encode_header(w::Header{w::Kind::request, 1U, 1U, 12U}, MutableByteView{nullptr, 0U})
              .error() == Error::BUFFER_TOO_SMALL);
    std::array<std::uint8_t, 19> short_out{};
    CHECK(w::encode_header(w::Header{w::Kind::request, 1U, 1U, 12U}, mutable_view(short_out))
              .error() == Error::BUFFER_TOO_SMALL);

    CHECK(w::decode_hello_request(ByteView{nullptr, 24U}).error() == Error::INVALID_ARGUMENT);
    CHECK(w::decode_acquire_request(ByteView{nullptr, 18U}).error() == Error::INVALID_ARGUMENT);
    CHECK(w::decode_acquire_response(ByteView{nullptr, 24U}).error() == Error::INVALID_ARGUMENT);
    CHECK(w::decode_acquire_request(ByteView{nullptr, 0U}).error() == Error::PROTOCOL_ERROR);
    CHECK(w::encode_acquire_request(w::AcquireRequest{}, MutableByteView{nullptr, 18U}).error() ==
          Error::INVALID_ARGUMENT);
}

void test_frames()
{
    // ACQUIRE response frame, request_id 2, epoch 0x10..0x1f, lease 0x0102030405060708.
    const std::array<std::uint8_t, 44> frame = {
        'U',  'D',  'T',  'V',  0x01, 0x00, 0x00, 0x00, 0x20, 0x00, 0x01,
        0x00, 0x02, 0x00, 0x00, 0x00, 0x18, 0x00, 0x00, 0x00, 0x10, 0x11,
        0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x1b, 0x1c,
        0x1d, 0x1e, 0x1f, 0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01};
    const auto decoded = w::decode_frame(view(frame));
    CHECK(decoded.has_value());
    CHECK(decoded.value().header.kind == w::Kind::response);
    CHECK(decoded.value().payload.data == frame.data() + 20);
    CHECK(decoded.value().payload.size == 24U);
    const auto acquire = w::decode_acquire_response(decoded.value().payload);
    CHECK(acquire.has_value() && acquire.value().lease_id == 0x0102030405060708ULL &&
          acquire.value().epoch[0] == 0x10U && acquire.value().epoch[15] == 0x1fU);

    std::array<std::uint8_t, 44> out{};
    std::array<std::uint8_t, 24> payload{};
    CHECK(w::encode_acquire_response(acquire.value(), mutable_view(payload)).has_value());
    const auto written = w::encode_frame(w::Header{w::Kind::response, w::kOpcodeAcquire, 2U, 24U},
                                         view(payload), mutable_view(out));
    CHECK(written.has_value() && written.value() == 44U);
    CHECK(equals(out.data(), out.size(), frame));

    // Exact length: one byte short, one byte extra, header only.
    CHECK(w::decode_frame(ByteView{frame.data(), 43U}).error() == Error::PROTOCOL_ERROR);
    std::array<std::uint8_t, 45> extra{};
    for (std::size_t i = 0U; i < frame.size(); ++i) {
        extra[i] = frame[i];
    }
    CHECK(w::decode_frame(view(extra)).error() == Error::PROTOCOL_ERROR);
    CHECK(w::decode_frame(ByteView{frame.data(), 20U}).error() == Error::PROTOCOL_ERROR);
    CHECK(w::decode_frame(ByteView{frame.data(), 19U}).error() == Error::PROTOCOL_ERROR);
    // Header errors win over length errors.
    const auto legacy = raw_header('P', 'X', '4', 'U', 1U, 0U, 0x0020U, 1U, 2U, 0U);
    CHECK(w::decode_frame(view(legacy)).error() == Error::PROTOCOL_ERROR);
    const auto v2 = raw_header('U', 'D', 'T', 'V', 2U, 0U, 0x0020U, 1U, 2U, 0U);
    CHECK(w::decode_frame(view(v2)).error() == Error::VERSION_MISMATCH);

    // Unknown opcode in a valid envelope passes through unchanged.
    const std::array<std::uint8_t, 23> unknown = {'U',  'D',  'T',  'V',  0x01, 0x00, 0x00, 0x00,
                                                  0xbc, 0x7a, 0x00, 0x00, 0x2a, 0x00, 0x00, 0x00,
                                                  0x03, 0x00, 0x00, 0x00, 'x',  'y',  'z'};
    const auto u = w::decode_frame(view(unknown));
    CHECK(u.has_value() && u.value().header.opcode == 0x7abcU &&
          u.value().header.kind == w::Kind::request && u.value().header.request_id == 42U &&
          u.value().payload.size == 3U && u.value().payload.data[2] == 'z');

    // encode_frame rejects length mismatch and writes nothing when output is short.
    std::array<std::uint8_t, 43> short_out{};
    short_out.fill(0x55U);
    CHECK(w::encode_frame(w::Header{w::Kind::response, w::kOpcodeAcquire, 2U, 24U}, view(payload),
                          mutable_view(short_out))
              .error() == Error::BUFFER_TOO_SMALL);
    CHECK(short_out[0] == 0x55U && short_out[42] == 0x55U);
    CHECK(w::encode_frame(w::Header{w::Kind::response, w::kOpcodeAcquire, 2U, 23U}, view(payload),
                          mutable_view(out))
              .error() == Error::INVALID_ARGUMENT);
    CHECK(w::encode_frame(w::Header{w::Kind::response, w::kOpcodeAcquire, 2U, 24U},
                          ByteView{nullptr, 24U}, mutable_view(out))
              .error() == Error::INVALID_ARGUMENT);
    CHECK(w::encode_frame(w::Header{w::Kind::response, w::kOpcodeAcquire, 0U, 24U}, view(payload),
                          mutable_view(out))
              .error() == Error::INVALID_ARGUMENT);
}

class Recorder final : public w::FrameHandler {
public:
    dtv::userland::Result<void> on_frame(const w::Frame& frame) noexcept override
    {
        if (count < headers.size()) {
            headers[count] = frame.header;
            std::size_t sum = 0U;
            for (std::size_t i = 0U; i < frame.payload.size; ++i) {
                sum += frame.payload.data[i];
            }
            sums[count] = sum;
        }
        ++count;
        if (fail_with != Error::OK) {
            return dtv::userland::Result<void>::failure(fail_with);
        }
        return dtv::userland::Result<void>::success();
    }

    std::array<w::Header, 8> headers{};
    std::array<std::size_t, 8> sums{};
    std::size_t count = 0U;
    Error fail_with = Error::OK;
};

void test_receiver()
{
    // Three frames back to back: unknown request with 3 bytes, empty event,
    // request 0x0011 with 1 byte. Payloads are not interpreted by the receiver.
    const std::array<std::uint8_t, 64> stream = {
        'U',  'D',  'T',  'V',  0x01, 0x00, 0x00, 0x00, 0xbc, 0x7a, 0x00, 0x00, 0x2a,
        0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x01, 0x02, 0x03, 'U',  'D',  'T',
        'V',  0x01, 0x00, 0x00, 0x00, 0xf0, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 'U',  'D',  'T',  'V',  0x01, 0x00, 0x00, 0x00, 0x11,
        0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x07};

    std::array<std::uint8_t, 16> storage{};
    {
        w::FrameReceiver receiver(mutable_view(storage));
        Recorder recorder;
        const auto fed = receiver.feed(view(stream), recorder);
        CHECK(fed.has_value() && fed.value() == 3U && recorder.count == 3U);
        CHECK(recorder.headers[0].opcode == 0x7abcU && recorder.sums[0] == 6U);
        CHECK(recorder.headers[1].kind == w::Kind::event && recorder.headers[1].opcode == 0x80f0U);
        CHECK(recorder.headers[2].opcode == 0x0011U && recorder.headers[2].request_id == 5U &&
              recorder.sums[2] == 7U);
    }
    {
        // One byte at a time.
        w::FrameReceiver receiver(mutable_view(storage));
        Recorder recorder;
        std::size_t total = 0U;
        for (std::size_t i = 0U; i < stream.size(); ++i) {
            const auto fed = receiver.feed(ByteView{stream.data() + i, 1U}, recorder);
            CHECK(fed.has_value());
            total += fed.has_value() ? fed.value() : 0U;
        }
        CHECK(total == 3U && recorder.count == 3U && recorder.sums[0] == 6U &&
              recorder.sums[2] == 7U);
        CHECK(receiver.feed(ByteView{nullptr, 0U}, recorder).value() == 0U);
    }
    {
        // Split inside the header and inside the payload.
        w::FrameReceiver receiver(mutable_view(storage));
        Recorder recorder;
        CHECK(receiver.feed(ByteView{stream.data(), 7U}, recorder).value() == 0U);
        CHECK(receiver.feed(ByteView{stream.data() + 7, 14U}, recorder).value() == 0U);
        CHECK(receiver.feed(ByteView{stream.data() + 21, 30U}, recorder).value() == 2U);
        CHECK(receiver.feed(ByteView{stream.data() + 51, 13U}, recorder).value() == 1U);
        CHECK(recorder.count == 3U);
    }
    {
        // Oversized payload versus caller storage: rejected before any payload byte is stored.
        std::array<std::uint8_t, 4> tiny{};
        tiny.fill(0xeeU);
        w::FrameReceiver receiver(mutable_view(tiny));
        Recorder recorder;
        const auto header = udtv(0x0010U, 0U, 1U, 5U);
        const auto fed = receiver.feed(view(header), recorder);
        CHECK(!fed.has_value() && fed.error() == Error::BUFFER_TOO_SMALL);
        CHECK(receiver.failed() && receiver.failure() == Error::BUFFER_TOO_SMALL);
        CHECK(tiny[0] == 0xeeU && tiny[3] == 0xeeU && recorder.count == 0U);
        // Poisoned until reset.
        CHECK(receiver.feed(view(stream), recorder).error() == Error::BUFFER_TOO_SMALL);
        receiver.reset();
        CHECK(!receiver.failed());
        const auto empty_event = udtv(0x80f0U, 0U, 0U, 0U);
        CHECK(receiver.feed(view(empty_event), recorder).value() == 1U);
    }
    {
        // Header over the protocol cap is a protocol error even with ample storage.
        static std::array<std::uint8_t, w::kMaxTsDataPayload + 1U> big{};
        w::FrameReceiver receiver(mutable_view(big));
        Recorder recorder;
        const auto ts_max = udtv(w::kOpcodeTsData, 0U, 0U, 1048504U);
        CHECK(receiver.feed(view(ts_max), recorder).value() == 0U);
        CHECK(!receiver.failed());
        receiver.reset();
        const auto ts_over = udtv(w::kOpcodeTsData, 0U, 0U, 1048505U);
        CHECK(receiver.feed(view(ts_over), recorder).error() == Error::PROTOCOL_ERROR);
        CHECK(receiver.header_status() == w::HeaderStatus::payload_too_large);
        receiver.reset();
        const auto control_over = udtv(0x0010U, 0U, 1U, 65537U);
        CHECK(receiver.feed(view(control_over), recorder).error() == Error::PROTOCOL_ERROR);
    }
    {
        // Legacy magic and version are reported through the receiver.
        w::FrameReceiver receiver(mutable_view(storage));
        Recorder recorder;
        const auto legacy = raw_header('P', 'X', '4', 'U', 1U, 0U, 1U, 0U, 1U, 0U);
        CHECK(receiver.feed(view(legacy), recorder).error() == Error::PROTOCOL_ERROR);
        CHECK(receiver.header_status() == w::HeaderStatus::legacy_magic);
        receiver.reset();
        CHECK(receiver.header_status() == w::HeaderStatus::ok);
        const auto v11 = raw_header('U', 'D', 'T', 'V', 1U, 1U, 1U, 0U, 1U, 0U);
        CHECK(receiver.feed(view(v11), recorder).error() == Error::VERSION_MISMATCH);
        CHECK(recorder.count == 0U);
    }
    {
        // A failure in a coalesced buffer stops after the frames already delivered.
        std::array<std::uint8_t, 43> mixed{};
        for (std::size_t i = 0U; i < 23U; ++i) {
            mixed[i] = stream[i];
        }
        const auto bad = udtv(0x0010U, 2U, 1U, 0U);
        for (std::size_t i = 0U; i < 20U; ++i) {
            mixed[23U + i] = bad[i];
        }
        w::FrameReceiver receiver(mutable_view(storage));
        Recorder recorder;
        CHECK(receiver.feed(view(mixed), recorder).error() == Error::PROTOCOL_ERROR);
        CHECK(recorder.count == 1U && receiver.header_status() == w::HeaderStatus::bad_flags);
    }
    {
        // Handler failure poisons the receiver.
        w::FrameReceiver receiver(mutable_view(storage));
        Recorder recorder;
        recorder.fail_with = Error::UNSUPPORTED;
        CHECK(receiver.feed(view(stream), recorder).error() == Error::UNSUPPORTED);
        CHECK(recorder.count == 1U && receiver.failure() == Error::UNSUPPORTED);
    }
    {
        Recorder recorder;
        w::FrameReceiver null_storage(MutableByteView{nullptr, 8U});
        CHECK(null_storage.failed() && null_storage.failure() == Error::INVALID_ARGUMENT);
        null_storage.reset();
        CHECK(null_storage.failed());
        w::FrameReceiver receiver(mutable_view(storage));
        CHECK(receiver.feed(ByteView{nullptr, 1U}, recorder).error() == Error::INVALID_ARGUMENT);
        // Zero storage is valid for empty-payload frames only.
        w::FrameReceiver zero(MutableByteView{nullptr, 0U});
        const auto empty_event = udtv(0x80f0U, 0U, 0U, 0U);
        CHECK(zero.feed(view(empty_event), recorder).value() == 1U);
        const auto one = udtv(0x80f0U, 0U, 0U, 1U);
        CHECK(zero.feed(view(one), recorder).error() == Error::BUFFER_TOO_SMALL);
    }
}

void test_receiver_eof()
{
    // Unknown request (3-byte payload), empty STATE_EVENT, request 0x0011 (1 byte).
    const std::array<std::uint8_t, 64> stream = {
        'U',  'D',  'T',  'V',  0x01, 0x00, 0x00, 0x00, 0xbc, 0x7a, 0x00, 0x00, 0x2a,
        0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x01, 0x02, 0x03, 'U',  'D',  'T',
        'V',  0x01, 0x00, 0x00, 0x00, 0xf0, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 'U',  'D',  'T',  'V',  0x01, 0x00, 0x00, 0x00, 0x11,
        0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x07};
    std::array<std::uint8_t, 16> storage{};
    {
        // No input at all and EOF after whole frames are clean.
        w::FrameReceiver receiver(mutable_view(storage));
        Recorder recorder;
        CHECK(receiver.pending_size() == 0U && receiver.finish().has_value());
        CHECK(receiver.feed(view(stream), recorder).value() == 3U);
        CHECK(receiver.pending_size() == 0U);
        CHECK(receiver.finish().has_value());
        CHECK(!receiver.failed() && !receiver.truncated());
    }
    {
        // Clean boundary right after an empty-payload frame.
        w::FrameReceiver receiver(mutable_view(storage));
        Recorder recorder;
        CHECK(receiver.feed(ByteView{stream.data(), 43U}, recorder).value() == 2U);
        CHECK(receiver.pending_size() == 0U && receiver.finish().has_value());
    }
    // EOF inside the first header, right after it, inside its payload, and
    // inside a later header after two complete frames.
    const std::size_t cut_points[] = {1U, 7U, 19U, 20U, 21U, 22U, 24U, 42U, 44U, 51U, 63U};
    const std::size_t expected_delivered[] = {0U, 0U, 0U, 0U, 0U, 0U, 1U, 1U, 2U, 2U, 2U};
    const std::size_t expected_pending[] = {1U, 7U, 19U, 20U, 21U, 22U, 1U, 19U, 1U, 8U, 20U};
    for (std::size_t i = 0U; i < 11U; ++i) {
        w::FrameReceiver receiver(mutable_view(storage));
        Recorder recorder;
        const auto fed = receiver.feed(ByteView{stream.data(), cut_points[i]}, recorder);
        CHECK(fed.has_value() && fed.value() == expected_delivered[i]);
        CHECK(receiver.pending_size() == expected_pending[i]);
        const auto finished = receiver.finish();
        CHECK(!finished.has_value() && finished.error() == Error::PROTOCOL_ERROR);
        CHECK(receiver.failed() && receiver.truncated() &&
              receiver.failure() == Error::PROTOCOL_ERROR);
        // No resynchronization: a complete valid frame is refused afterwards.
        const auto empty_event = udtv(0x80f0U, 0U, 0U, 0U);
        CHECK(receiver.feed(view(empty_event), recorder).error() == Error::PROTOCOL_ERROR);
        CHECK(recorder.count == expected_delivered[i]);
        CHECK(receiver.finish().error() == Error::PROTOCOL_ERROR);
        receiver.reset();
        CHECK(!receiver.failed() && !receiver.truncated() && receiver.pending_size() == 0U);
        CHECK(receiver.feed(view(empty_event), recorder).value() == 1U);
        CHECK(receiver.finish().has_value());
    }
    {
        // A receiver that already failed reports that failure, not truncation.
        std::array<std::uint8_t, 2> tiny{};
        w::FrameReceiver receiver(mutable_view(tiny));
        Recorder recorder;
        CHECK(receiver.feed(ByteView{stream.data(), 21U}, recorder).error() ==
              Error::BUFFER_TOO_SMALL);
        CHECK(receiver.finish().error() == Error::BUFFER_TOO_SMALL);
        CHECK(!receiver.truncated());
    }
    {
        // EOF after a maximum TS_DATA header: bounded, no payload stored, truncated.
        static std::array<std::uint8_t, w::kMaxTsDataPayload> ts_storage{};
        w::FrameReceiver receiver(mutable_view(ts_storage));
        Recorder recorder;
        const auto ts_max = udtv(w::kOpcodeTsData, 0U, 0U, 1048504U);
        CHECK(receiver.feed(view(ts_max), recorder).value() == 0U);
        CHECK(receiver.pending_size() == 20U);
        CHECK(receiver.finish().error() == Error::PROTOCOL_ERROR && receiver.truncated());
    }
}

void test_hello()
{
    // min 1.0, max 1.3, required 0x0000000000000003, optional 0x000000000000000c.
    const std::array<std::uint8_t, 24> bytes = {0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x03, 0x00,
                                                0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                                                0x0c, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    const auto decoded = w::decode_hello_request(view(bytes));
    CHECK(decoded.has_value());
    CHECK(decoded.value().min.major == 1U && decoded.value().min.minor == 0U);
    CHECK(decoded.value().max.major == 1U && decoded.value().max.minor == 3U);
    CHECK(decoded.value().required_features == 0x3U);
    CHECK(decoded.value().optional_features == 0xcU);

    std::array<std::uint8_t, 24> out{};
    w::HelloRequest request;
    request.min = w::Version{1U, 0U};
    request.max = w::Version{1U, 3U};
    request.required_features = 0x3U;
    request.optional_features = 0xcU;
    CHECK(w::encode_hello_request(request, mutable_view(out)).value() == 24U);
    CHECK(equals(out.data(), out.size(), bytes));

    // High and unknown feature bits are preserved in both masks; every
    // multi-byte field is little-endian.
    const std::array<std::uint8_t, 24> wide = {0x00, 0x00, 0x34, 0x12, 0x02, 0x01, 0x00, 0x00,
                                               0xef, 0xcd, 0xab, 0x89, 0x67, 0x45, 0x23, 0x81,
                                               0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80};
    const auto w2 = w::decode_hello_request(view(wide));
    CHECK(w2.has_value() && w2.value().min.minor == 0x1234U && w2.value().max.major == 0x0102U);
    CHECK(w2.value().required_features == 0x8123456789abcdefULL);
    CHECK(w2.value().optional_features == 0x8000000000000001ULL);
    std::array<std::uint8_t, 24> wide_out{};
    CHECK(w::encode_hello_request(w2.value(), mutable_view(wide_out)).has_value());
    CHECK(equals(wide_out.data(), wide_out.size(), wide));

    // Inverted range.
    const std::array<std::uint8_t, 24> inverted = {0x01, 0x00, 0x02, 0x00, 0x01, 0x00, 0x01, 0x00};
    CHECK(w::decode_hello_request(view(inverted)).error() == Error::INVALID_ARGUMENT);
    request.min = w::Version{2U, 0U};
    request.max = w::Version{1U, 9U};
    out.fill(0x33U);
    CHECK(w::encode_hello_request(request, mutable_view(out)).error() == Error::INVALID_ARGUMENT);
    CHECK(out[0] == 0x33U);
    CHECK(w::negotiate_version(request).error() == Error::INVALID_ARGUMENT);

    // Truncation (including the old 12-byte layout) and trailing bytes.
    CHECK(w::decode_hello_request(ByteView{bytes.data(), 23U}).error() == Error::PROTOCOL_ERROR);
    CHECK(w::decode_hello_request(ByteView{bytes.data(), 12U}).error() == Error::PROTOCOL_ERROR);
    std::array<std::uint8_t, 25> longer{};
    CHECK(w::decode_hello_request(view(longer)).error() == Error::PROTOCOL_ERROR);
    std::array<std::uint8_t, 23> short_out{};
    request.min = w::Version{1U, 0U};
    request.max = w::Version{1U, 0U};
    CHECK(w::encode_hello_request(request, mutable_view(short_out)).error() ==
          Error::BUFFER_TOO_SMALL);

    struct Case {
        w::Version min;
        w::Version max;
        bool ok;
    };
    const Case cases[] = {
        {{1U, 0U}, {1U, 0U}, true},   {{0U, 5U}, {1U, 0U}, true},
        {{1U, 0U}, {1U, 7U}, true},   {{0U, 0U}, {9U, 9U}, true},
        {{1U, 1U}, {1U, 5U}, false},  {{2U, 0U}, {2U, 0U}, false},
        {{0U, 0U}, {0U, 9U}, false},  {{0U, 0xffffU}, {0U, 0xffffU}, false},
    };
    for (const auto& c : cases) {
        w::HelloRequest r;
        r.min = c.min;
        r.max = c.max;
        const auto v = w::negotiate_version(r);
        if (c.ok) {
            CHECK(v.has_value() && v.value().major == 1U && v.value().minor == 0U);
        } else {
            CHECK(!v.has_value() && v.error() == Error::VERSION_MISMATCH);
        }
    }
}

void test_acquire()
{
    // epoch 0xa0..0xaf, receiver_id 0x0102 (above 255).
    const std::array<std::uint8_t, 18> request_bytes = {
        0xa0, 0xa1, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7, 0xa8,
        0xa9, 0xaa, 0xab, 0xac, 0xad, 0xae, 0xaf, 0x02, 0x01};
    const auto request = w::decode_acquire_request(view(request_bytes));
    CHECK(request.has_value() && request.value().receiver_id == 0x0102U);
    CHECK(request.value().epoch[0] == 0xa0U && request.value().epoch[15] == 0xafU);
    std::array<std::uint8_t, 18> out18{};
    CHECK(w::encode_acquire_request(request.value(), mutable_view(out18)).value() == 18U);
    CHECK(equals(out18.data(), out18.size(), request_bytes));
    w::AcquireRequest built;
    for (std::size_t i = 0U; i < w::kEpochSize; ++i) {
        built.epoch[i] = static_cast<std::uint8_t>(0xa0U + i);
    }
    built.receiver_id = 0x0102U;
    out18.fill(0U);
    CHECK(w::encode_acquire_request(built, mutable_view(out18)).has_value());
    CHECK(equals(out18.data(), out18.size(), request_bytes));
    CHECK(w::encode_acquire_request(built, MutableByteView{nullptr, 0U}).error() ==
          Error::BUFFER_TOO_SMALL);
    std::array<std::uint8_t, 17> short18{};
    CHECK(w::encode_acquire_request(built, mutable_view(short18)).error() ==
          Error::BUFFER_TOO_SMALL);
    // The old 1-byte layout, truncation and trailing bytes.
    CHECK(w::decode_acquire_request(ByteView{request_bytes.data(), 1U}).error() ==
          Error::PROTOCOL_ERROR);
    CHECK(w::decode_acquire_request(ByteView{request_bytes.data(), 17U}).error() ==
          Error::PROTOCOL_ERROR);
    std::array<std::uint8_t, 19> nineteen{};
    CHECK(w::decode_acquire_request(view(nineteen)).error() == Error::PROTOCOL_ERROR);
    // receiver_id range is daemon policy: 0 and 0xffff decode.
    const std::array<std::uint8_t, 18> zero_id{};
    CHECK(w::decode_acquire_request(view(zero_id)).value().receiver_id == 0U);
    std::array<std::uint8_t, 18> max_id{};
    max_id[16] = 0xffU;
    max_id[17] = 0xffU;
    CHECK(w::decode_acquire_request(view(max_id)).value().receiver_id == 0xffffU);

    // epoch 0x10..0x1f, lease_id 0x0123456789abcdef.
    const std::array<std::uint8_t, 24> response_bytes = {
        0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x1b,
        0x1c, 0x1d, 0x1e, 0x1f, 0xef, 0xcd, 0xab, 0x89, 0x67, 0x45, 0x23, 0x01};
    const auto response = w::decode_acquire_response(view(response_bytes));
    CHECK(response.has_value() && response.value().lease_id == 0x0123456789abcdefULL);
    CHECK(response.value().epoch[0] == 0x10U && response.value().epoch[15] == 0x1fU);
    std::array<std::uint8_t, 24> out{};
    CHECK(w::encode_acquire_response(response.value(), mutable_view(out)).value() == 24U);
    CHECK(equals(out.data(), out.size(), response_bytes));

    // Epoch carries through a request/response pair unchanged.
    w::AcquireResponse echoed;
    echoed.epoch = request.value().epoch;
    echoed.lease_id = 1U;
    CHECK(w::encode_acquire_response(echoed, mutable_view(out)).has_value());
    const std::array<std::uint8_t, 24> echoed_bytes = {
        0xa0, 0xa1, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7, 0xa8, 0xa9, 0xaa, 0xab,
        0xac, 0xad, 0xae, 0xaf, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    CHECK(equals(out.data(), out.size(), echoed_bytes));
    CHECK(w::decode_acquire_response(view(echoed_bytes)).value().epoch == request.value().epoch);

    // lease_id 0 is rejected both ways, even with a nonzero epoch; all-ones is valid.
    std::array<std::uint8_t, 24> zero_lease = response_bytes;
    for (std::size_t i = 16U; i < 24U; ++i) {
        zero_lease[i] = 0U;
    }
    CHECK(w::decode_acquire_response(view(zero_lease)).error() == Error::INVALID_ARGUMENT);
    w::AcquireResponse value;
    value.epoch = response.value().epoch;
    value.lease_id = 0U;
    out.fill(0x44U);
    CHECK(w::encode_acquire_response(value, mutable_view(out)).error() == Error::INVALID_ARGUMENT);
    CHECK(out[0] == 0x44U);
    value.lease_id = 0xffffffffffffffffULL;
    CHECK(w::encode_acquire_response(value, mutable_view(out)).has_value());
    CHECK(out[15] == 0x1fU && out[16] == 0xffU && out[23] == 0xffU);
    // Only the top byte set: catches a 32-bit truncation.
    std::array<std::uint8_t, 24> top_only{};
    top_only[23] = 0x80U;
    CHECK(w::decode_acquire_response(view(top_only)).value().lease_id == 0x8000000000000000ULL);

    CHECK(w::decode_acquire_response(ByteView{response_bytes.data(), 23U}).error() ==
          Error::PROTOCOL_ERROR);
    std::array<std::uint8_t, 25> longer{};
    longer[0] = 1U;
    CHECK(w::decode_acquire_response(view(longer)).error() == Error::PROTOCOL_ERROR);
    std::array<std::uint8_t, 23> short_out{};
    CHECK(w::encode_acquire_response(response.value(), mutable_view(short_out)).error() ==
          Error::BUFFER_TOO_SMALL);
}

void test_error_mapping()
{
    CHECK(w::header_error(w::HeaderStatus::ok) == Error::OK);
    CHECK(w::header_error(w::HeaderStatus::invalid_view) == Error::INVALID_ARGUMENT);
    CHECK(w::header_error(w::HeaderStatus::wrong_size) == Error::INVALID_ARGUMENT);
    CHECK(w::header_error(w::HeaderStatus::legacy_magic) == Error::PROTOCOL_ERROR);
    CHECK(w::header_error(w::HeaderStatus::bad_magic) == Error::PROTOCOL_ERROR);
    CHECK(w::header_error(w::HeaderStatus::unsupported_version) == Error::VERSION_MISMATCH);
    CHECK(w::header_error(w::HeaderStatus::bad_flags) == Error::PROTOCOL_ERROR);
    CHECK(w::header_error(w::HeaderStatus::bad_request_id) == Error::PROTOCOL_ERROR);
    CHECK(w::header_error(w::HeaderStatus::bad_kind_for_opcode) == Error::PROTOCOL_ERROR);
    CHECK(w::header_error(w::HeaderStatus::payload_too_large) == Error::PROTOCOL_ERROR);
}

}  // namespace

int main()
{
    test_header_vectors();
    test_magic_and_version();
    test_flags_and_ids();
    test_payload_caps();
    test_views_and_sizes();
    test_frames();
    test_receiver();
    test_receiver_eof();
    test_hello();
    test_acquire();
    test_error_mapping();
    if (g_failures != 0) {
        std::fprintf(stderr, "wire_tests: %d of %d checks failed\n", g_failures, g_checks);
        return 1;
    }
    std::printf("wire_tests: %d checks passed\n", g_checks);
    return 0;
}
