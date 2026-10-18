// SPDX-License-Identifier: MIT
#include <dtv/userland/records.h>
#include <dtv/userland/wire.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

namespace w = dtv::userland::wire;
using dtv::userland::ByteView;
using dtv::userland::Error;
using dtv::userland::MutableByteView;

int g_failures = 0;
int g_checks = 0;

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

bool bytes_contain(const std::uint8_t* haystack, std::size_t haystack_size,
                   const std::uint8_t* needle, std::size_t needle_size)
{
    if (needle_size == 0U || needle_size > haystack_size) {
        return false;
    }
    for (std::size_t i = 0U; i + needle_size <= haystack_size; ++i) {
        if (std::memcmp(haystack + i, needle, needle_size) == 0) {
            return true;
        }
    }
    return false;
}

template <std::size_t N>
w::WString<N> make_str(const char* text)
{
    w::WString<N> value;
    const std::size_t length = std::strlen(text);
    value.length = static_cast<std::uint16_t>(length);
    std::memcpy(value.bytes.data(), text, length);
    return value;
}

w::Uuid make_uuid(std::uint8_t base)
{
    w::Uuid value{};
    for (std::size_t i = 0U; i < w::kEpochSize; ++i) {
        value[i] = static_cast<std::uint8_t>(base + i);
    }
    return value;
}

w::Systems make_systems(bool t, bool s)
{
    w::Systems value;
    value.mask = static_cast<std::uint8_t>((t ? 1U : 0U) | (s ? 2U : 0U));
    return value;
}

w::Counter valid_counter(std::uint64_t value, std::uint64_t sampled, bool overflow)
{
    w::Counter counter;
    counter.measurement.state = w::MeasureState::valid;
    counter.measurement.value = value;
    counter.measurement.sampled_at_ns = sampled;
    counter.overflow = overflow;
    return counter;
}

w::Counter unknown_counter(w::UnknownReason reason)
{
    w::Counter counter;
    counter.measurement.state = w::MeasureState::unknown;
    counter.measurement.reason = reason;
    return counter;
}

// Minimal public receiver descriptor: one system, no binding, no capabilities.
w::ReceiverDescriptor minimal_receiver(std::uint16_t id)
{
    w::ReceiverDescriptor descriptor;
    descriptor.id = id;
    descriptor.systems = make_systems(true, false);
    return descriptor;
}

// Minimal reader descriptor.
w::ReaderDescriptor minimal_reader(std::uint16_t id)
{
    w::ReaderDescriptor descriptor;
    descriptor.id = id;
    return descriptor;
}

void test_constants()
{
    CHECK(w::kOpcodeList == 0x0010U);
    CHECK(w::kOpcodeTune == 0x0022U);
    CHECK(w::kOpcodeAttachStream == 0x0040U);
    CHECK(w::kOpcodeCardStatus == 0x0030U);
    CHECK(w::kOpcodeStateEvent == 0x80f0U);
    CHECK(w::is_event_opcode(w::kOpcodeStreamEndEvent));
    CHECK(!w::is_event_opcode(w::kOpcodeStatus));
    CHECK(w::kFeatureStateEvent == 1U && w::kFeaturePidFilter == 4U);
}

// Independent literal vector: HELLO response with product "px4", version
// "1.2.3", no core version, epoch 0xa0..0xaf.
void test_hello_response_golden()
{
    const std::array<std::uint8_t, 49> expected = {
        0x01, 0x00, 0x00, 0x00,                    // major 1, minor 0
        0x05, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // features 0x05
        0xa0, 0xa1, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7,  // epoch
        0xa8, 0xa9, 0xaa, 0xab, 0xac, 0xad, 0xae, 0xaf,
        0x03, 0x00, 'p', 'x', '4',                 // producer.product
        0x05, 0x00, '1', '.', '2', '.', '3',       // producer.version
        0x00,                                      // producer.core_version absent
        0x00, 0x00, 0x01, 0x00,                    // max_control_payload 65536
        0x9c, 0xff, 0x0f, 0x00};                   // max_ts_bytes 1048476
    w::HelloResponse value;
    value.major = 1U;
    value.minor = 0U;
    value.features = 0x05U;
    value.epoch = make_uuid(0xa0U);
    value.producer.product = make_str<32>("px4");
    value.producer.version = make_str<64>("1.2.3");
    value.max_control_payload = 65536U;
    value.max_ts_bytes = 1048476U;

    std::array<std::uint8_t, 64> out{};
    const auto written = w::encode_hello_response(value, mutable_view(out));
    CHECK(written.has_value() && written.value() == expected.size());
    CHECK(equals(out.data(), expected.size(), expected));

    const auto decoded = w::decode_hello_response(view(expected));
    CHECK(decoded.has_value());
    CHECK(decoded.value().major == 1U && decoded.value().minor == 0U);
    CHECK(decoded.value().features == 0x05U);
    CHECK(decoded.value().epoch[0] == 0xa0U && decoded.value().epoch[15] == 0xafU);
    CHECK(decoded.value().producer.product.length == 3U);
    CHECK(decoded.value().producer.version.bytes[4] == '3');
    CHECK(!decoded.value().producer.core_version.present);
    CHECK(decoded.value().max_control_payload == 65536U);
    CHECK(decoded.value().max_ts_bytes == 1048476U);
}

// Independent literal vector: error response, receiver target, retry absent,
// required_payload_bytes present.
void test_error_response_golden()
{
    const std::array<std::uint8_t, 43> expected = {
        0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d,
        0x0e, 0x0f, 0x0f, 0x00, 0x00, 0x00, 0x04, 0x00, 's',  't',  'a',  't',  0x00, 0x00,
        0x01, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x01,
        0x00};
    w::ErrorResponse value;
    value.epoch = make_uuid(0x00U);
    value.error.code = 15U;
    value.error.operation = make_str<32>("stat");
    value.target.kind = w::ScopeKind::receiver;
    value.target.id = 2U;
    value.required_payload_bytes.present = true;
    value.required_payload_bytes.value = 65536U;

    std::array<std::uint8_t, 64> out{};
    const auto written = w::encode_error_response(value, mutable_view(out));
    CHECK(written.has_value() && written.value() == expected.size());
    CHECK(equals(out.data(), expected.size(), expected));

    const auto decoded = w::decode_error_response(view(expected));
    CHECK(decoded.has_value());
    CHECK(decoded.value().error.code == 15U);
    CHECK(decoded.value().error.operation.length == 4U);
    CHECK(decoded.value().target.kind == w::ScopeKind::receiver && decoded.value().target.id == 2U);
    CHECK(!decoded.value().retry_after_ms.present);
    CHECK(decoded.value().required_payload_bytes.present &&
          decoded.value().required_payload_bytes.value == 65536U);
}

// Independent literal vector: TS_DATA, one 188-byte packet starting 47 1F FF 10.
void test_ts_data_golden()
{
    std::array<std::uint8_t, 216> expected{};
    expected[0] = 0x08;  // stream_id 0x0102030405060708
    expected[1] = 0x07;
    expected[2] = 0x06;
    expected[3] = 0x05;
    expected[4] = 0x04;
    expected[5] = 0x03;
    expected[6] = 0x02;
    expected[7] = 0x01;
    expected[8] = 0x02;  // sequence 2
    expected[16] = 0x03;  // cumulative drops 3
    expected[24] = 0xbc;  // length 188
    expected[28] = 0x47;
    expected[29] = 0x1f;
    expected[30] = 0xff;
    expected[31] = 0x10;

    std::array<std::uint8_t, 188> packet{};
    packet[0] = 0x47;
    packet[1] = 0x1f;
    packet[2] = 0xff;
    packet[3] = 0x10;
    w::TsData value;
    value.stream_id = 0x0102030405060708ULL;
    value.sequence = 2U;
    value.cumulative_queue_dropped_packets = 3U;
    value.packets = ByteView{packet.data(), packet.size()};

    std::array<std::uint8_t, 216> out{};
    const auto written = w::encode_ts_data(value, mutable_view(out));
    CHECK(written.has_value() && written.value() == expected.size());
    CHECK(equals(out.data(), expected.size(), expected));

    const auto decoded = w::decode_ts_data(view(expected));
    CHECK(decoded.has_value());
    CHECK(decoded.value().stream_id == 0x0102030405060708ULL);
    CHECK(decoded.value().sequence == 2U);
    CHECK(decoded.value().cumulative_queue_dropped_packets == 3U);
    CHECK(decoded.value().packets.data == expected.data() + 28U && decoded.value().packets.size == 188U);
    CHECK(decoded.value().packets.data[0] == 0x47U && decoded.value().packets.data[188 - 1U] == 0x00U);

    // Stream id 0, misaligned length and over-cap length are rejected.
    value.stream_id = 0U;
    CHECK(!w::encode_ts_data(value, mutable_view(out)).has_value());
    value.stream_id = 1U;
    std::array<std::uint8_t, 190> odd{};
    value.packets = ByteView{odd.data(), odd.size()};
    CHECK(w::encode_ts_data(value, mutable_view(out)).error() == Error::INVALID_ARGUMENT);
    // Truncation and trailing bytes.
    CHECK(w::decode_ts_data(ByteView{expected.data(), 215U}).error() == Error::PROTOCOL_ERROR);
    std::array<std::uint8_t, 217> longer{};
    std::memcpy(longer.data(), expected.data(), expected.size());
    CHECK(w::decode_ts_data(view(longer)).error() == Error::PROTOCOL_ERROR);
}

// Independent literal vector: STATE_EVENT for receiver 7, sequence 42.
void test_state_event_golden()
{
    const std::array<std::uint8_t, 34> expected = {
        0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28, 0x29, 0x2a, 0x2b,
        0x2c, 0x2d, 0x2e, 0x2f, 0x2a, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x01, 0x07, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02};
    w::StateEvent value;
    value.epoch = make_uuid(0x20U);
    value.snapshot_sequence = 42U;
    value.target.kind = w::ScopeKind::receiver;
    value.target.id = 7U;
    value.reason = w::StateEventReason::receiver_changed;

    std::array<std::uint8_t, 64> out{};
    const auto written = w::encode_state_event(value, mutable_view(out));
    CHECK(written.has_value() && written.value() == expected.size());
    CHECK(equals(out.data(), expected.size(), expected));
    const auto decoded = w::decode_state_event(view(expected));
    CHECK(decoded.has_value() && decoded.value().snapshot_sequence == 42U &&
          decoded.value().target.id == 7U &&
          decoded.value().reason == w::StateEventReason::receiver_changed);

    // reason 0 and 5 are undefined for events.
    std::array<std::uint8_t, 34> bad = expected;
    bad[33] = 0U;
    CHECK(w::decode_state_event(view(bad)).error() == Error::INVALID_ARGUMENT);
    bad[33] = 5U;
    CHECK(w::decode_state_event(view(bad)).error() == Error::INVALID_ARGUMENT);
}

// Independent literal vector: usable card session result with a 2-byte ATR.
void test_card_session_golden()
{
    const std::array<std::uint8_t, 54> expected = {
        0x40, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4a, 0x4b, 0x4c, 0x4d,
        0x4e, 0x4f, 0x03, 0x00, 0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01, 0x05, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x02, 0x00, 0x00, 0x00, 0x3b, 0x00};
    w::CardSessionResult value;
    value.epoch = make_uuid(0x40U);
    value.reader_id = 3U;
    value.card_handle = 0x0102030405060708ULL;
    value.bound_generation = 5U;
    value.current_generation = 5U;
    value.present = true;
    value.initialized = true;
    value.usable = true;
    value.protocol.present = true;
    value.protocol.value = w::CardProtocol::t1;
    value.atr.present = true;
    value.atr.value.length = 2U;
    value.atr.value.bytes[0] = 0x3bU;
    value.atr.value.bytes[1] = 0x00U;

    std::array<std::uint8_t, 64> out{};
    const auto written = w::encode_card_session_result(value, mutable_view(out));
    CHECK(written.has_value() && written.value() == expected.size());
    CHECK(equals(out.data(), expected.size(), expected));

    const auto decoded = w::decode_card_session_result(view(expected));
    CHECK(decoded.has_value());
    CHECK(decoded.value().reader_id == 3U && decoded.value().card_handle == 0x0102030405060708ULL);
    CHECK(decoded.value().bound_generation == 5U && decoded.value().current_generation == 5U);
    CHECK(decoded.value().usable && decoded.value().protocol.present &&
          decoded.value().protocol.value == w::CardProtocol::t1);
    CHECK(decoded.value().atr.present && decoded.value().atr.value.length == 2U);
    CHECK(w::validate_card_session_result(decoded.value()) == Error::OK);
}

// Independent literal vector: STOP request pins lease before stream.
void test_stream_lifecycle_golden()
{
    const std::array<std::uint8_t, 32> stop_expected = {
        0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d,
        0x1e, 0x1f, 0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01, 0x18, 0x17, 0x16, 0x15,
        0x14, 0x13, 0x12, 0x11};
    w::StopRequest stop;
    stop.epoch = make_uuid(0x10U);
    stop.lease_id = 0x0102030405060708ULL;
    stop.stream_id = 0x1112131415161718ULL;
    std::array<std::uint8_t, 64> out{};
    const auto stop_written = w::encode_stop_request(stop, mutable_view(out));
    CHECK(stop_written.has_value() && stop_written.value() == stop_expected.size());
    CHECK(equals(out.data(), stop_expected.size(), stop_expected));
    const auto decoded_stop = w::decode_stop_request(view(stop_expected));
    CHECK(decoded_stop.has_value());
    CHECK(decoded_stop.value().lease_id == 0x0102030405060708ULL);
    CHECK(decoded_stop.value().stream_id == 0x1112131415161718ULL);
    // Swapping the two u64 fields changes the decode: the fields are not
    // interchangeable and the test pins their order.
    std::array<std::uint8_t, 32> swapped = stop_expected;
    for (std::size_t i = 0U; i < 8U; ++i) {
        swapped[16U + i] = stop_expected[24U + i];
        swapped[24U + i] = stop_expected[16U + i];
    }
    const auto swapped_decoded = w::decode_stop_request(view(swapped));
    CHECK(swapped_decoded.has_value() &&
          swapped_decoded.value().lease_id != decoded_stop.value().lease_id);

    // StartResponse roundtrip.
    w::StartResponse start;
    start.epoch = make_uuid(0x20U);
    start.stream_id = 0x0102030405060708ULL;
    start.attach_token = make_uuid(0x30U);
    start.expires_after_ms = 5000U;
    const auto start_written = w::encode_start_response(start, mutable_view(out));
    CHECK(start_written.has_value() && start_written.value() == 44U);
    const auto start_decoded =
        w::decode_start_response(ByteView{out.data(), start_written.value()});
    CHECK(start_decoded.has_value() &&
          start_decoded.value().stream_id == 0x0102030405060708ULL &&
          start_decoded.value().attach_token[15] == 0x3fU &&
          start_decoded.value().expires_after_ms == 5000U);

    // AttachRequest carries epoch, lease, stream and the token in order.
    w::AttachRequest attach;
    attach.epoch = make_uuid(0x20U);
    attach.lease_id = 1U;
    attach.stream_id = 2U;
    attach.attach_token = make_uuid(0x30U);
    const auto attach_written = w::encode_attach_request(attach, mutable_view(out));
    CHECK(attach_written.has_value() && attach_written.value() == 48U);
    CHECK(w::decode_attach_request(ByteView{out.data(), attach_written.value()})
              .value()
              .stream_id == 2U);

    // StreamEndEvent wraps the same StreamFinal as a STOP response.
    w::StreamEndEvent event;
    event.final.id = 9U;
    event.final.tune_revision = 4U;
    event.final.reason = w::StreamEndReason::stopped;
    event.final.statistics.entries.count = 2U;
    event.final.statistics.entries.items[0].id = w::kCounterIdTransmittedPackets;
    event.final.statistics.entries.items[0].counter = valid_counter(10U, 100U, false);
    event.final.statistics.entries.items[1].id = w::kCounterIdQueueDroppedPackets;
    event.final.statistics.entries.items[1].counter = unknown_counter(w::UnknownReason::stale);
    const auto event_written = w::encode_stream_end_event(event, mutable_view(out));
    CHECK(event_written.has_value());
    const auto event_decoded =
        w::decode_stream_end_event(ByteView{out.data(), event_written.value()});
    CHECK(event_decoded.has_value() && event_decoded.value().final.id == 9U);
    CHECK(w::validate_stream_final(event_decoded.value().final) == Error::OK);
    // backend_error without an error record is inconsistent.
    w::StreamFinal broken = event_decoded.value().final;
    broken.reason = w::StreamEndReason::backend_error;
    CHECK(w::validate_stream_final(broken) == Error::INVALID_ARGUMENT);
    // stopped with an error is allowed but the record must be valid.
    broken.reason = w::StreamEndReason::stopped;
    broken.error.present = true;
    broken.error.value.code = 7U;
    broken.error.value.operation = make_str<32>("usb");
    CHECK(w::validate_stream_final(broken) == Error::OK);

    // RELEASE: released implies quiesced and no error.
    w::ReleaseResponse release;
    release.epoch = make_uuid(0x40U);
    release.lease_id = 5U;
    release.released = true;
    release.quiesced = true;
    CHECK(w::encode_release_response(release, mutable_view(out)).has_value());
    release.quiesced = false;
    CHECK(w::encode_release_response(release, mutable_view(out)).error() == Error::INVALID_ARGUMENT);
}

void test_tune()
{
    w::TuneRequest request;
    request.epoch = make_uuid(0x10U);
    request.lease_id = 1U;
    request.system = w::System::isdb_s;
    request.frequency_hz = 0x0000010203040506ULL;
    request.selection = w::Selection::slot;
    request.selection_value = 3U;
    request.lnb_voltage.present = true;
    request.lnb_voltage.value = 15U;
    request.lna.present = true;
    request.lna.value = false;
    request.pids.present = true;
    request.pids.value.count = 2U;
    request.pids.value.items[0] = 0U;
    request.pids.value.items[1] = 8191U;
    request.timeout_ms = 30000U;

    std::vector<std::uint8_t> out(8192);
    const auto written = w::encode_tune_request(request, MutableByteView{out.data(), out.size()});
    CHECK(written.has_value());
    const auto decoded = w::decode_tune_request(ByteView{out.data(), written.value()});
    CHECK(decoded.has_value());
    CHECK(decoded.value().system == w::System::isdb_s);
    CHECK(decoded.value().frequency_hz == 0x0000010203040506ULL);
    CHECK(decoded.value().selection == w::Selection::slot && decoded.value().selection_value == 3U);
    CHECK(decoded.value().lnb_voltage.present && decoded.value().lnb_voltage.value == 15U);
    CHECK(decoded.value().lna.present && !decoded.value().lna.value);
    CHECK(decoded.value().pids.present && decoded.value().pids.value.count == 2U &&
          decoded.value().pids.value.items[1] == 8191U);
    CHECK(decoded.value().timeout_ms == 30000U);

    // automatic selection must carry the reserved value 0.
    request.selection = w::Selection::automatic;
    request.selection_value = 1U;
    CHECK(w::encode_tune_request(request, MutableByteView{out.data(), out.size()}).error() ==
          Error::INVALID_ARGUMENT);
    request.selection_value = 0U;
    request.frequency_hz = 0U;
    CHECK(w::encode_tune_request(request, MutableByteView{out.data(), out.size()}).error() ==
          Error::INVALID_ARGUMENT);
    request.frequency_hz = 1U;
    request.pids.value.items[1] = 0U;  // duplicate
    CHECK(w::encode_tune_request(request, MutableByteView{out.data(), out.size()}).error() ==
          Error::INVALID_ARGUMENT);
    request.pids.value.items[1] = 8192U;  // above 8191
    CHECK(w::encode_tune_request(request, MutableByteView{out.data(), out.size()}).error() ==
          Error::INVALID_ARGUMENT);
    request.pids.value.items[1] = 4U;
    request.lnb_voltage.value = 3U;
    CHECK(w::encode_tune_request(request, MutableByteView{out.data(), out.size()}).error() ==
          Error::INVALID_ARGUMENT);

    w::TuneResult result;
    result.epoch = make_uuid(0x10U);
    result.tune_revision = 6U;
    result.requested_hz = 1049480000ULL;
    result.programmed_hz.present = true;
    result.programmed_hz.value = 1049480000ULL;
    result.verification = w::Verification::lock;
    result.lock.state = w::MeasureState::valid;
    result.lock.value = true;
    result.lock.sampled_at_ns = 123U;
    result.selected_tsid.present = true;
    result.selected_tsid.value = 0x1234U;
    const auto result_written =
        w::encode_tune_result(result, MutableByteView{out.data(), out.size()});
    CHECK(result_written.has_value());
    const auto result_decoded = w::decode_tune_result(ByteView{out.data(), result_written.value()});
    CHECK(result_decoded.has_value() && result_decoded.value().tune_revision == 6U &&
          result_decoded.value().lock.value && result_decoded.value().selected_tsid.value == 0x1234U);
    // tune_revision 0 is never a successful tune.
    result.tune_revision = 0U;
    CHECK(w::encode_tune_result(result, MutableByteView{out.data(), out.size()}).error() ==
          Error::INVALID_ARGUMENT);
}

void test_measurements_and_counters()
{
    w::ReceiverDescriptor descriptor = minimal_receiver(0U);
    descriptor.capabilities.measurement_mask = w::kMetricMaskLock | w::kMetricMaskCnrMdb;

    w::Measurements measurements;
    measurements.entries.count = 2U;
    measurements.entries.items[0].id = w::kMetricIdLock;
    measurements.entries.items[0].boolean.state = w::MeasureState::valid;
    measurements.entries.items[0].boolean.value = true;
    measurements.entries.items[0].boolean.sampled_at_ns = 10U;
    measurements.entries.items[1].id = w::kMetricIdCnrMdb;
    measurements.entries.items[1].integer.state = w::MeasureState::unknown;
    measurements.entries.items[1].integer.reason = w::UnknownReason::not_sampled;
    CHECK(w::validate_measurements(measurements, descriptor.capabilities.measurement_mask) ==
          Error::OK);

    // A declared metric with no record, and an undeclared record, are both wrong.
    CHECK(w::validate_measurements(measurements, w::kMetricMaskAll) == Error::INVALID_ARGUMENT);
    measurements.entries.items[1].id = w::kMetricIdSignalMdbm;
    CHECK(w::validate_measurements(measurements, descriptor.capabilities.measurement_mask) ==
          Error::INVALID_ARGUMENT);
    // unsupported is not a valid member of a declared set.
    measurements.entries.items[1].id = w::kMetricIdCnrMdb;
    measurements.entries.items[1].integer.state = w::MeasureState::unsupported;
    CHECK(w::validate_measurements(measurements, descriptor.capabilities.measurement_mask) ==
          Error::INVALID_ARGUMENT);

    // Roundtrip preserves unknown versus valid and the sample time.
    std::array<std::uint8_t, 64> out{};
    measurements.entries.items[1].integer.state = w::MeasureState::valid;
    measurements.entries.items[1].integer.value = -5;
    measurements.entries.items[1].integer.sampled_at_ns = 20U;
    const auto written = w::encode_measurements(measurements, mutable_view(out));
    CHECK(written.has_value());
    const auto decoded = w::decode_measurements(ByteView{out.data(), written.value()});
    CHECK(decoded.has_value() && decoded.value().entries.items[1].integer.value == -5 &&
          decoded.value().entries.items[1].integer.sampled_at_ns == 20U);
    CHECK(w::validate_measurements(decoded.value(), descriptor.capabilities.measurement_mask) ==
          Error::OK);

    w::StreamCounters stream;
    stream.entries.count = 2U;
    stream.entries.items[0].id = w::kCounterIdTransmittedPackets;
    stream.entries.items[0].counter = valid_counter(0xffffffffffffffffULL, 5U, true);
    stream.entries.items[1].id = w::kCounterIdQueueDroppedPackets;
    stream.entries.items[1].counter = unknown_counter(w::UnknownReason::stale);
    CHECK(w::validate_stream_counters(stream) == Error::OK);
    // overflow=true requires the saturated value.
    stream.entries.items[0].counter.measurement.value = 100U;
    CHECK(w::validate_stream_counters(stream) == Error::INVALID_ARGUMENT);
    stream.entries.items[0].counter.measurement.value = 0xffffffffffffffffULL;
    stream.entries.count = 1U;
    CHECK(w::validate_stream_counters(stream) == Error::INVALID_ARGUMENT);
    stream.entries.count = 2U;
    stream.entries.items[1].id = w::kCounterIdCapturedPackets;
    CHECK(w::validate_stream_counters(stream) == Error::INVALID_ARGUMENT);
    stream.entries.items[1].id = w::kCounterIdQueueDroppedPackets;
    stream.entries.items[1].counter.measurement.state = w::MeasureState::unsupported;
    CHECK(w::validate_stream_counters(stream) == Error::INVALID_ARGUMENT);

    w::ReceiverCounters receiver_counters;
    receiver_counters.entries.count = 2U;
    receiver_counters.entries.items[0].id = w::kCounterIdCapturedPackets;
    receiver_counters.entries.items[0].counter = valid_counter(1U, 2U, false);
    receiver_counters.entries.items[1].id = w::kCounterIdFramingErrors;
    receiver_counters.entries.items[1].counter = unknown_counter(w::UnknownReason::read_failed);
    CHECK(w::validate_receiver_counters(receiver_counters) == Error::OK);
    receiver_counters.entries.items[1].id = w::kCounterIdTransmittedPackets;
    CHECK(w::validate_receiver_counters(receiver_counters) == Error::INVALID_ARGUMENT);
    receiver_counters.entries.items[0].id = 0U;
    CHECK(w::validate_receiver_counters(receiver_counters) == Error::INVALID_ARGUMENT);

    // Excess counts must be rejected before any element is read, including for a
    // direct typed caller that never went through decode.
    w::Measurements too_many_metrics;
    too_many_metrics.entries.count = 0xffffU;
    CHECK(w::validate_measurements(too_many_metrics, 0U) == Error::INVALID_ARGUMENT);
    w::ReceiverCounters too_many_counters;
    too_many_counters.entries.count = 0xffffU;
    CHECK(w::validate_receiver_counters(too_many_counters) == Error::INVALID_ARGUMENT);
    w::StreamCounters wrong_stream_count;
    wrong_stream_count.entries.count = 0xffffU;
    CHECK(w::validate_stream_counters(wrong_stream_count) == Error::INVALID_ARGUMENT);
    // The codecs reject an over-cap count before touching element storage.
    std::array<std::uint8_t, 64> count_out{};
    w::Measurements count_metrics;
    count_metrics.entries.count = 4U;
    CHECK(w::encode_measurements(count_metrics, mutable_view(count_out)).error() ==
          Error::INVALID_ARGUMENT);
}

void test_capabilities()
{
    // Minimal capabilities: four bytes, no LNA/LNB bodies and no presence tags.
    w::Capabilities minimal;
    std::array<std::uint8_t, 8> out{};
    const auto minimal_written = w::encode_capabilities(minimal, mutable_view(out));
    CHECK(minimal_written.has_value() && minimal_written.value() == 4U);
    const std::array<std::uint8_t, 4> minimal_bytes = {0x00, 0x00, 0x00, 0x00};
    CHECK(equals(out.data(), 4U, minimal_bytes));

    w::Capabilities caps;
    caps.controls = w::kControlBitLna;
    caps.lna.entries.count = 1U;
    caps.lna.entries.items[0].system = w::System::isdb_s;
    caps.lna.entries.items[0].default_value.present = true;
    caps.lna.entries.items[0].default_value.value = true;
    // LNB body present while its control bit is clear is inconsistent.
    caps.lnb.values.count = 1U;
    caps.lnb.values.items[0] = 0U;
    CHECK(w::validate_capabilities(caps) == Error::INVALID_ARGUMENT);
    CHECK(w::encode_capabilities(caps, mutable_view(out)).error() == Error::INVALID_ARGUMENT);
    caps.lnb.values.count = 0U;
    CHECK(w::validate_capabilities(caps) == Error::OK);

    // LNA body with the control bit set adds its five body bytes and no tag:
    // four base bytes plus count(2) + system(1) + default tag(1) + value(1).
    std::array<std::uint8_t, 16> lna_out{};
    const auto lna_written = w::encode_capabilities(caps, mutable_view(lna_out));
    CHECK(lna_written.has_value() && lna_written.value() == 9U);

    caps.controls = static_cast<std::uint8_t>(w::kControlBitLna | 0x04U);
    CHECK(w::validate_capabilities(caps) == Error::INVALID_ARGUMENT);

    caps.controls = w::kControlBitLna;
    caps.measurement_mask = 0x08U;
    CHECK(w::validate_capabilities(caps) == Error::INVALID_ARGUMENT);
    caps.measurement_mask = w::kMetricMaskLock;

    // LNA entries must be one per supported system, ascending, no duplicate.
    caps.lna.entries.count = 2U;
    caps.lna.entries.items[0].system = w::System::isdb_s;
    caps.lna.entries.items[1].system = w::System::isdb_s;
    CHECK(w::validate_capabilities(caps) == Error::INVALID_ARGUMENT);
    caps.lna.entries.items[0].system = w::System::isdb_t;
    caps.lna.entries.items[1].system = w::System::isdb_s;
    CHECK(w::validate_capabilities(caps) == Error::OK);
    // A set control bit requires at least one LNA entry.
    caps.lna.entries.count = 0U;
    CHECK(w::validate_capabilities(caps) == Error::INVALID_ARGUMENT);
    caps.lna.entries.count = 1U;

    caps.controls = static_cast<std::uint8_t>(w::kControlBitLna | w::kControlBitLnb);
    caps.lnb.values.count = 2U;
    caps.lnb.values.items[0] = 0U;
    caps.lnb.values.items[1] = 15U;
    CHECK(w::validate_capabilities(caps) == Error::OK);
    caps.lnb.values.items[1] = 10U;
    CHECK(w::validate_capabilities(caps) == Error::INVALID_ARGUMENT);
    caps.lnb.values.items[1] = 0U;
    CHECK(w::validate_capabilities(caps) == Error::INVALID_ARGUMENT);

    caps.controls = w::kControlBitLna;
    caps.lnb.values.count = 0U;
    caps.max_pids.present = true;
    caps.max_pids.value = 8192U;
    CHECK(w::validate_capabilities(caps) == Error::OK);
    caps.max_pids.value = 0U;
    CHECK(w::validate_capabilities(caps) == Error::INVALID_ARGUMENT);

    // Reserved bit in controls and measurement_mask is rejected by the codec.
    w::ReceiverDescriptor descriptor = minimal_receiver(1U);
    descriptor.capabilities.controls = 0x80U;
    std::array<std::uint8_t, 16> out2{};
    CHECK(w::encode_receiver_descriptor(descriptor, mutable_view(out2)).error() ==
          Error::INVALID_ARGUMENT);
    descriptor.capabilities.controls = 0U;
    descriptor.capabilities.measurement_mask = 0x80U;
    CHECK(w::encode_receiver_descriptor(descriptor, mutable_view(out2)).error() ==
          Error::INVALID_ARGUMENT);
    descriptor.capabilities.measurement_mask = 0U;
    descriptor.systems = make_systems(false, false);
    CHECK(w::encode_receiver_descriptor(descriptor, mutable_view(out2)).error() ==
          Error::INVALID_ARGUMENT);
    descriptor.systems = make_systems(false, true);
    CHECK(w::encode_receiver_descriptor(descriptor, mutable_view(out2)).has_value());
}

void test_scope_and_card_validation()
{
    w::Scope scope;
    scope.kind = w::ScopeKind::enclosure;
    scope.id = 0U;
    CHECK(w::validate_scope(scope) == Error::OK);
    scope.id = 1U;
    CHECK(w::validate_scope(scope) == Error::INVALID_ARGUMENT);
    scope.kind = w::ScopeKind::receiver;
    scope.id = 0xffffU;
    CHECK(w::validate_scope(scope) == Error::OK);
    scope.id = 0x10000U;
    CHECK(w::validate_scope(scope) == Error::INVALID_ARGUMENT);
    scope.kind = w::ScopeKind::stream;
    scope.id = 0xffffffffffffffffULL;
    CHECK(w::validate_scope(scope) == Error::OK);

    std::array<std::uint8_t, 16> out{};
    scope.kind = w::ScopeKind::enclosure;
    scope.id = 1U;
    CHECK(w::encode_scope(scope, mutable_view(out)).error() == Error::INVALID_ARGUMENT);

    // Card observation presence rules.
    w::CardObservation card;
    card.state = w::CardState::present;
    card.sampled_at_ns.present = true;
    card.sampled_at_ns.value = 5U;
    CHECK(w::validate_card_observation(card) == Error::OK);
    card.sampled_at_ns.present = false;
    CHECK(w::validate_card_observation(card) == Error::INVALID_ARGUMENT);
    card.state = w::CardState::unknown;
    card.reason = static_cast<std::uint8_t>(w::UnknownReason::not_sampled);
    CHECK(w::validate_card_observation(card) == Error::OK);
    card.sampled_at_ns.present = true;
    card.sampled_at_ns.value = 1U;
    CHECK(w::validate_card_observation(card) == Error::INVALID_ARGUMENT);
    card.sampled_at_ns.present = false;
    card.state = w::CardState::unavailable;
    card.reason = static_cast<std::uint8_t>(w::UnknownReason::read_failed);
    CHECK(w::validate_card_observation(card) == Error::OK);
    card.reason = 0U;
    CHECK(w::validate_card_observation(card) == Error::INVALID_ARGUMENT);

    // Stale handle: usable must not claim a mismatch between generations.
    w::CardSessionResult result;
    result.epoch = make_uuid(0x10U);
    result.reader_id = 1U;
    result.card_handle = 1U;
    result.bound_generation = 4U;
    result.current_generation = 5U;
    result.present = true;
    result.initialized = true;
    result.usable = true;
    result.protocol.present = true;
    result.protocol.value = w::CardProtocol::t1;
    result.atr.present = true;
    result.atr.value.length = 1U;
    result.atr.value.bytes[0] = 0x3bU;
    CHECK(w::validate_card_session_result(result) == Error::INVALID_ARGUMENT);
    result.usable = false;
    CHECK(w::validate_card_session_result(result) == Error::INVALID_ARGUMENT);  // protocol kept
    result.protocol.present = false;
    result.atr.present = false;
    CHECK(w::validate_card_session_result(result) == Error::OK);
    result.usable = true;
    result.bound_generation = 5U;
    result.protocol.present = true;
    result.atr.present = true;
    CHECK(w::validate_card_session_result(result) == Error::OK);
}

void test_error_codes()
{
    std::array<std::uint8_t, 64> out{};
    w::ErrorRecord record;
    record.code = 0U;
    CHECK(w::encode_error_record(record, mutable_view(out)).error() == Error::INVALID_ARGUMENT);
    // An unknown nonzero code keeps its numeric value.
    record.code = 4095U;
    record.operation = make_str<32>("op");
    const auto written = w::encode_error_record(record, mutable_view(out));
    CHECK(written.has_value());
    const auto decoded = w::decode_error_record(ByteView{out.data(), written.value()});
    CHECK(decoded.has_value() && decoded.value().code == 4095U);
    std::array<std::uint8_t, 8> zero_code{};
    CHECK(w::decode_error_record(view(zero_code)).error() == Error::INVALID_ARGUMENT);
}

void test_strings_and_malformed()
{
    std::array<std::uint8_t, 64> out{};
    w::ListRequest request;
    request.epoch = make_uuid(0x00U);
    const auto written = w::encode_list_request(request, mutable_view(out));
    CHECK(written.has_value() && written.value() == 16U);
    // Trailing byte.
    std::array<std::uint8_t, 17> longer{};
    longer[16] = 0U;
    CHECK(w::decode_list_request(view(longer)).error() == Error::PROTOCOL_ERROR);
    CHECK(w::decode_list_request(ByteView{out.data(), 15U}).error() == Error::PROTOCOL_ERROR);
    CHECK(w::decode_list_request(ByteView{nullptr, 16U}).error() == Error::INVALID_ARGUMENT);

    // Strings: over cap, embedded NUL and invalid UTF-8 are rejected.
    w::Model model;
    model.id.length = 65U;  // cap is 64
    CHECK(w::encode_model(model, mutable_view(out)).error() == Error::INVALID_ARGUMENT);
    model.id.length = 3U;
    model.id.bytes[0] = 'a';
    model.id.bytes[1] = '\0';
    model.id.bytes[2] = 'b';
    CHECK(w::encode_model(model, mutable_view(out)).error() == Error::INVALID_ARGUMENT);
    model.id.bytes[0] = static_cast<char>(0xc3);
    model.id.bytes[1] = static_cast<char>(0x28);
    model.id.bytes[2] = 'b';
    CHECK(w::encode_model(model, mutable_view(out)).error() == Error::INVALID_ARGUMENT);
    // A valid two-byte UTF-8 sequence (U+3042) is accepted.
    model.id.length = 3U;
    model.id.bytes[0] = static_cast<char>(0xe3);
    model.id.bytes[1] = static_cast<char>(0x81);
    model.id.bytes[2] = static_cast<char>(0x82);
    model.name = make_str<128>("name");
    const auto model_written = w::encode_model(model, mutable_view(out));
    CHECK(model_written.has_value());
    const auto model_decoded = w::decode_model(ByteView{out.data(), model_written.value()});
    CHECK(model_decoded.has_value() && model_decoded.value().id.length == 3U);

    // Truncated embedded string inside a record.
    std::array<std::uint8_t, 32> truncated{};
    truncated[0] = 0x10U;  // declared id length 16 with only a few bytes left
    CHECK(w::decode_model(view(truncated)).error() == Error::PROTOCOL_ERROR);
}

void test_arrays_and_caps()
{
    // The functions array cap is 8; a ninth element is rejected.
    w::ListResponse list;
    list.epoch = make_uuid(0x00U);
    list.producer.product = make_str<32>("px4");
    list.producer.version = make_str<64>("1");
    list.topology.state = w::TopologyState::complete;
    list.functions.count = 8U;
    for (std::size_t i = 0U; i < 8U; ++i) {
        list.functions.items[i].id = static_cast<std::uint16_t>(i + 1U);
        list.functions.items[i].identification_reason =
            w::IdentificationReason::descriptor_unreadable;
    }
    std::vector<std::uint8_t> out(70000);
    const auto written = w::encode_list_response(list, MutableByteView{out.data(), out.size()});
    CHECK(written.has_value() && written.value() <= w::kMaxControlPayload);
    list.functions.count = 9U;
    CHECK(w::encode_list_response(list, MutableByteView{out.data(), out.size()}).error() ==
          Error::INVALID_ARGUMENT);

    // A full STATUS with 32 minimal receivers still fits the control cap.
    w::StatusResponse status;
    status.producer.product = make_str<32>("px4");
    status.producer.version = make_str<64>("1");
    status.context.epoch = make_uuid(0x00U);
    status.context.state = w::ContextState::ready;
    status.scope.kind = w::ScopeKind::enclosure;
    status.topology.state = w::TopologyState::complete;
    status.functions.present = true;
    status.functions.value.count = 8U;
    for (std::size_t i = 0U; i < 8U; ++i) {
        status.functions.value.items[i].id = static_cast<std::uint16_t>(i + 1U);
        status.functions.value.items[i].identification_reason =
            w::IdentificationReason::descriptor_unreadable;
    }
    status.receivers.present = true;
    status.receivers.value.count = 32U;
    for (std::size_t i = 0U; i < 32U; ++i) {
        status.receivers.value.items[i].descriptor = minimal_receiver(static_cast<std::uint16_t>(i));
    }
    status.readers.present = true;
    status.readers.value.count = 0U;
    const auto status_written =
        w::encode_status_response(status, MutableByteView{out.data(), out.size()});
    CHECK(status_written.has_value() && status_written.value() <= w::kMaxControlPayload);
    const auto status_decoded =
        w::decode_status_response(ByteView{out.data(), status_written.value()});
    CHECK(status_decoded.has_value() && status_decoded.value().receivers.value.count == 32U);
    CHECK(w::validate_status_response_scope(status_decoded.value()) == Error::OK);

    // metrics cap is 3.
    w::Measurements metrics;
    metrics.entries.count = 4U;
    CHECK(w::encode_measurements(metrics, MutableByteView{out.data(), out.size()}).error() ==
          Error::INVALID_ARGUMENT);
}

void test_status_scope_rules()
{
    w::StatusResponse status;
    status.producer.product = make_str<32>("px4");
    status.producer.version = make_str<64>("1");
    status.context.epoch = make_uuid(0x00U);
    status.context.state = w::ContextState::ready;
    status.scope.kind = w::ScopeKind::enclosure;
    status.topology.state = w::TopologyState::complete;
    status.functions.present = true;
    status.functions.value.count = 1U;
    status.functions.value.items[0].id = 1U;
    status.functions.value.items[0].identification_reason =
        w::IdentificationReason::descriptor_unreadable;
    status.receivers.present = true;
    status.receivers.value.count = 1U;
    status.receivers.value.items[0].descriptor = minimal_receiver(4U);
    status.readers.present = true;
    status.readers.value.count = 0U;
    CHECK(w::validate_status_response_scope(status) == Error::OK);

    // Unknown topology must omit both collections.
    status.topology.state = w::TopologyState::unknown;
    CHECK(w::validate_status_response_scope(status) == Error::INVALID_ARGUMENT);
    status.receivers.present = false;
    status.readers.present = false;
    CHECK(w::validate_status_response_scope(status) == Error::OK);

    // Receiver scope returns exactly the matching receiver and no functions.
    status.scope.kind = w::ScopeKind::receiver;
    status.scope.id = 4U;
    CHECK(w::validate_status_response_scope(status) == Error::INVALID_ARGUMENT);  // functions kept
    status.functions.present = false;
    status.receivers.present = true;
    status.receivers.value.count = 1U;
    CHECK(w::validate_status_response_scope(status) == Error::OK);
    status.receivers.value.items[0].descriptor.id = 5U;
    CHECK(w::validate_status_response_scope(status) == Error::INVALID_ARGUMENT);

    // Reader scope.
    status.scope.kind = w::ScopeKind::reader;
    status.scope.id = 2U;
    status.receivers.present = false;
    status.readers.present = true;
    status.readers.value.count = 1U;
    status.readers.value.items[0].descriptor = minimal_reader(2U);
    CHECK(w::validate_status_response_scope(status) == Error::OK);
    status.readers.value.items[0].descriptor = minimal_reader(3U);
    CHECK(w::validate_status_response_scope(status) == Error::INVALID_ARGUMENT);

    // Stream scope matches current or last stream.
    status.scope.kind = w::ScopeKind::stream;
    status.scope.id = 0x1122334455667788ULL;
    status.receivers.present = true;
    status.receivers.value.count = 1U;
    status.receivers.value.items[0].descriptor = minimal_receiver(4U);
    status.readers.present = false;
    status.receivers.value.items[0].stream.present = true;
    status.receivers.value.items[0].stream.value.id = 0x1122334455667788ULL;
    status.receivers.value.items[0].stream.value.tune_revision = 1U;
    CHECK(w::validate_status_response_scope(status) == Error::OK);
    status.receivers.value.items[0].stream.value.id = 1U;
    CHECK(w::validate_status_response_scope(status) == Error::INVALID_ARGUMENT);
}

void test_absent_vs_empty_and_sparse()
{
    // Unknown topology omits both collections; a known topology returns both,
    // with a known-empty reader set as a present array of count 0. The two
    // encodings differ by exactly the four bytes of the two empty array bodies.
    w::ListResponse list;
    list.epoch = make_uuid(0x00U);
    list.producer.product = make_str<32>("px4");
    list.producer.version = make_str<64>("1");
    list.topology.state = w::TopologyState::unknown;
    std::vector<std::uint8_t> absent(4096);
    std::vector<std::uint8_t> present(4096);
    const auto absent_written =
        w::encode_list_response(list, MutableByteView{absent.data(), absent.size()});
    CHECK(absent_written.has_value());
    CHECK(w::validate_list_response(list) == Error::OK);
    list.topology.state = w::TopologyState::complete;
    // Known topology with an absent collection is invalid.
    CHECK(w::validate_list_response(list) == Error::INVALID_ARGUMENT);
    list.receivers.present = true;
    list.receivers.value.count = 0U;
    list.readers.present = true;
    list.readers.value.count = 0U;
    const auto present_written =
        w::encode_list_response(list, MutableByteView{present.data(), present.size()});
    CHECK(present_written.has_value());
    CHECK(present_written.value() == absent_written.value() + 4U);
    const auto absent_decoded =
        w::decode_list_response(ByteView{absent.data(), absent_written.value()});
    CHECK(absent_decoded.has_value() && !absent_decoded.value().receivers.present &&
          !absent_decoded.value().readers.present);
    const auto present_decoded =
        w::decode_list_response(ByteView{present.data(), present_written.value()});
    CHECK(present_decoded.has_value() && present_decoded.value().receivers.present &&
          present_decoded.value().receivers.value.count == 0U &&
          present_decoded.value().readers.present &&
          present_decoded.value().readers.value.count == 0U);

    // Sparse public IDs survive in order and no slot is invented. A known
    // reader set is present (count 0), not absent.
    w::ListResponse sparse;
    sparse.epoch = make_uuid(0x00U);
    sparse.producer.product = make_str<32>("px4");
    sparse.producer.version = make_str<64>("1");
    sparse.topology.state = w::TopologyState::complete;
    sparse.functions.count = 2U;
    sparse.functions.items[0].id = 2U;
    sparse.functions.items[0].identification_reason =
        w::IdentificationReason::descriptor_unreadable;
    sparse.functions.items[1].id = 6U;
    sparse.functions.items[1].identification_reason =
        w::IdentificationReason::descriptor_unreadable;
    sparse.receivers.present = true;
    sparse.receivers.value.count = 3U;
    sparse.receivers.value.items[0] = minimal_receiver(0U);
    sparse.receivers.value.items[1].id = 7U;
    sparse.receivers.value.items[1].systems = make_systems(false, true);
    sparse.receivers.value.items[2].id = 65535U;
    sparse.receivers.value.items[2].systems = make_systems(true, true);
    sparse.readers.present = true;
    sparse.readers.value.count = 0U;
    const auto sparse_written =
        w::encode_list_response(sparse, MutableByteView{present.data(), present.size()});
    CHECK(sparse_written.has_value());
    const auto sparse_decoded =
        w::decode_list_response(ByteView{present.data(), sparse_written.value()});
    CHECK(sparse_decoded.has_value() && sparse_decoded.value().receivers.value.count == 3U &&
          sparse_decoded.value().receivers.value.items[0].id == 0U &&
          sparse_decoded.value().receivers.value.items[1].id == 7U &&
          sparse_decoded.value().receivers.value.items[2].id == 65535U);
    CHECK(w::validate_list_response(sparse_decoded.value()) == Error::OK);
    // Duplicate receiver IDs are rejected.
    sparse.receivers.value.items[1].id = 0U;
    CHECK(w::validate_list_response(sparse) == Error::INVALID_ARGUMENT);
    sparse.receivers.value.items[1].id = 7U;
    // A binding that references a function not in the snapshot is rejected.
    sparse.receivers.value.items[1].binding.present = true;
    sparse.receivers.value.items[1].binding.value.function_id = 99U;
    CHECK(w::validate_list_response(sparse) == Error::INVALID_ARGUMENT);
    sparse.receivers.value.items[1].binding.value.function_id = 6U;
    CHECK(w::validate_list_response(sparse) == Error::OK);

    // Topology and limit caps.
    w::Topology topology;
    topology.state = w::TopologyState::complete;
    topology.expected_function_count.present = true;
    topology.expected_function_count.value = 8U;
    CHECK(w::validate_topology(topology) == Error::OK);
    topology.expected_function_count.value = 9U;
    CHECK(w::validate_topology(topology) == Error::INVALID_ARGUMENT);
    topology.expected_function_count.value = 0U;
    CHECK(w::validate_topology(topology) == Error::INVALID_ARGUMENT);
    w::Limits limits;
    limits.max_concurrent_streams.present = true;
    limits.max_concurrent_streams.value = 2U;
    CHECK(w::validate_limits(limits, 2U) == Error::OK);
    CHECK(w::validate_limits(limits, 1U) == Error::INVALID_ARGUMENT);
    limits.max_concurrent_streams.value = 32U;
    CHECK(w::validate_limits(limits, 32U) == Error::OK);
    limits.max_concurrent_streams.value = 33U;
    CHECK(w::validate_limits(limits, 40U) == Error::INVALID_ARGUMENT);
}

void test_monitoring_has_no_owner_secrets()
{
    // A monitoring ReceiverStatus has no lease/attach-token/card-handle field by
    // construction; prove the encoded bytes do not carry a lease id that the
    // owner path uses, and that ReaderStatus carries no ATR.
    w::ReceiverStatus receiver;
    receiver.descriptor = minimal_receiver(1U);
    receiver.tune_revision = 0x0102030405060708ULL;
    receiver.receiver_statistics.entries.count = 1U;
    receiver.receiver_statistics.entries.items[0].id = w::kCounterIdCapturedPackets;
    receiver.receiver_statistics.entries.items[0].counter = valid_counter(1U, 1U, false);
    std::vector<std::uint8_t> out(4096);
    const auto written =
        w::encode_receiver_status(receiver, MutableByteView{out.data(), out.size()});
    CHECK(written.has_value());
    const std::uint8_t lease_pattern[8] = {0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff, 0x00, 0x11};
    CHECK(!bytes_contain(out.data(), written.value(), lease_pattern, sizeof(lease_pattern)));

    w::ReaderStatus reader;
    reader.descriptor = minimal_reader(1U);
    const auto reader_written =
        w::encode_reader_status(reader, MutableByteView{out.data(), out.size()});
    CHECK(reader_written.has_value());
    const std::uint8_t atr_pattern[2] = {0x3b, 0x00};
    CHECK(!bytes_contain(out.data(), reader_written.value(), atr_pattern, sizeof(atr_pattern)));
}

void test_fragmented_frame()
{
    w::AttachResponse response;
    response.epoch = make_uuid(0x00U);
    response.stream_id = 0x0102030405060708ULL;
    std::array<std::uint8_t, 24> payload{};
    const auto payload_written = w::encode_attach_response(response, mutable_view(payload));
    CHECK(payload_written.has_value());
    std::array<std::uint8_t, 44> frame{};
    const auto frame_written = w::encode_frame(
        w::Header{w::Kind::response, w::kOpcodeAttachStream, 3U, 24U}, view(payload),
        mutable_view(frame));
    CHECK(frame_written.has_value() && frame_written.value() == 44U);

    class Handler final : public w::FrameHandler {
    public:
        dtv::userland::Result<void> on_frame(const w::Frame& incoming) noexcept override
        {
            const auto decoded = w::decode_attach_response(incoming.payload);
            if (!decoded) {
                return dtv::userland::Result<void>::failure(dtv::userland::Error::PROTOCOL_ERROR);
            }
            stream_id = decoded.value().stream_id;
            ++count;
            return dtv::userland::Result<void>::success();
        }
        std::uint64_t stream_id = 0U;
        std::size_t count = 0U;
    };

    std::array<std::uint8_t, 24> storage{};
    w::FrameReceiver receiver(mutable_view(storage));
    Handler handler;
    std::size_t delivered = 0U;
    for (std::size_t i = 0U; i < frame.size(); ++i) {
        const auto fed = receiver.feed(ByteView{frame.data() + i, 1U}, handler);
        CHECK(fed.has_value());
        delivered += fed.has_value() ? fed.value() : 0U;
    }
    CHECK(delivered == 1U && handler.count == 1U && handler.stream_id == 0x0102030405060708ULL);
}

w::UsbFunction minimal_usb_function(std::uint16_t id)
{
    w::UsbFunction function;
    function.id = id;
    function.vid = 0x1234U;
    function.pid = 0x5678U;
    function.mode = w::Mode::runtime;
    function.serial_state = w::SerialState::absent;
    function.identification_reason = w::IdentificationReason::descriptor_unreadable;
    function.present = true;
    return function;
}

w::StatusResponse base_status()
{
    w::StatusResponse status;
    status.producer.product = make_str<32>("px4");
    status.producer.version = make_str<64>("1");
    status.context.epoch = make_uuid(0x00U);
    status.context.state = w::ContextState::ready;
    status.snapshot.sequence = 1U;
    status.snapshot.observed_at_ns = 5U;
    status.scope.kind = w::ScopeKind::enclosure;
    status.topology.state = w::TopologyState::complete;
    status.topology.expected_function_count.present = true;
    status.topology.expected_function_count.value = 1U;
    status.functions.present = true;
    status.functions.value.count = 1U;
    status.functions.value.items[0] = minimal_usb_function(1U);
    status.receivers.present = true;
    status.receivers.value.count = 1U;
    status.receivers.value.items[0].descriptor = minimal_receiver(0U);
    status.readers.present = true;
    status.readers.value.count = 0U;
    return status;
}

void test_capabilities_golden()
{
    // Independent vectors: the controls bits carry the LNA/LNB bodies with no
    // optional presence tag, so minimal capabilities are four zero bytes.
    const std::array<std::uint8_t, 4> none = {0x00, 0x00, 0x00, 0x00};
    const std::array<std::uint8_t, 9> lna = {0x00, 0x00, 0x01, 0x01, 0x00,
                                             0x02, 0x01, 0x01, 0x00};
    const std::array<std::uint8_t, 8> lnb = {0x00, 0x00, 0x02, 0x02, 0x00, 0x00, 0x0f, 0x00};
    const std::array<std::uint8_t, 13> both = {0x00, 0x00, 0x03, 0x01, 0x00, 0x02, 0x01,
                                               0x01, 0x02, 0x00, 0x00, 0x0f, 0x00};

    w::Capabilities caps;
    std::array<std::uint8_t, 16> out{};
    CHECK(w::encode_capabilities(caps, mutable_view(out)).value() == none.size());
    CHECK(equals(out.data(), none.size(), none));
    CHECK(w::decode_capabilities(view(none)).has_value());

    caps.controls = w::kControlBitLna;
    caps.lna.entries.count = 1U;
    caps.lna.entries.items[0].system = w::System::isdb_s;
    caps.lna.entries.items[0].default_value.present = true;
    caps.lna.entries.items[0].default_value.value = true;
    out.fill(0U);
    CHECK(w::encode_capabilities(caps, mutable_view(out)).value() == lna.size());
    CHECK(equals(out.data(), lna.size(), lna));
    const auto lna_decoded = w::decode_capabilities(view(lna));
    CHECK(lna_decoded.has_value() && lna_decoded.value().lna.entries.count == 1U &&
          lna_decoded.value().lna.entries.items[0].default_value.present &&
          lna_decoded.value().lna.entries.items[0].default_value.value);

    w::Capabilities lnb_caps;
    lnb_caps.controls = w::kControlBitLnb;
    lnb_caps.lnb.values.count = 2U;
    lnb_caps.lnb.values.items[0] = 0U;
    lnb_caps.lnb.values.items[1] = 15U;
    out.fill(0U);
    CHECK(w::encode_capabilities(lnb_caps, mutable_view(out)).value() == lnb.size());
    CHECK(equals(out.data(), lnb.size(), lnb));

    w::Capabilities both_caps = caps;
    both_caps.controls = static_cast<std::uint8_t>(w::kControlBitLna | w::kControlBitLnb);
    both_caps.lnb.values.count = 2U;
    both_caps.lnb.values.items[0] = 0U;
    both_caps.lnb.values.items[1] = 15U;
    out.fill(0U);
    CHECK(w::encode_capabilities(both_caps, mutable_view(out)).value() == both.size());
    CHECK(equals(out.data(), both.size(), both));
    CHECK(w::validate_capabilities(w::decode_capabilities(view(both)).value()) == Error::OK);

    // The absent body LNA vector is exactly what a tag-carrying encoder would not
    // produce, so a decode of the four-byte minimal value leaves both empty.
    const auto minimal_decoded = w::decode_capabilities(view(none)).value();
    CHECK(minimal_decoded.lna.entries.count == 0U && minimal_decoded.lnb.values.count == 0U);
}

void test_measurement_and_counter_golden()
{
    const std::array<std::uint8_t, 16> measurements = {
        0x02, 0x00,                                    // count 2
        0x01, 0x02, 0x01, 0x0a, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // lock valid
        0x02, 0x01, 0x01};                             // cnr unknown not_sampled
    const auto decoded = w::decode_measurements(view(measurements));
    CHECK(decoded.has_value() && decoded.value().entries.count == 2U);
    CHECK(decoded.value().entries.items[0].id == w::kMetricIdLock &&
          decoded.value().entries.items[0].boolean.state == w::MeasureState::valid &&
          decoded.value().entries.items[0].boolean.value &&
          decoded.value().entries.items[0].boolean.sampled_at_ns == 10U);
    CHECK(decoded.value().entries.items[1].id == w::kMetricIdCnrMdb &&
          decoded.value().entries.items[1].integer.state == w::MeasureState::unknown &&
          decoded.value().entries.items[1].integer.reason == w::UnknownReason::not_sampled);
    CHECK(w::validate_measurements(decoded.value(), w::kMetricMaskLock | w::kMetricMaskCnrMdb) ==
          Error::OK);

    const std::array<std::uint8_t, 24> stream_counters = {
        0x02, 0x00,                                    // count 2
        0x05, 0x02, 0x0a, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // id5 valid
        0x64, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,        // sampled 100, overflow 0
        0x06, 0x01, 0x02};                             // id6 unknown stale
    const auto counters = w::decode_stream_counters(view(stream_counters));
    CHECK(counters.has_value() && counters.value().entries.count == 2U);
    CHECK(counters.value().entries.items[0].id == w::kCounterIdTransmittedPackets &&
          counters.value().entries.items[0].counter.measurement.value == 10U &&
          counters.value().entries.items[0].counter.measurement.sampled_at_ns == 100U &&
          !counters.value().entries.items[0].counter.overflow);
    CHECK(counters.value().entries.items[1].id == w::kCounterIdQueueDroppedPackets &&
          counters.value().entries.items[1].counter.measurement.state == w::MeasureState::unknown);
    CHECK(w::validate_stream_counters(counters.value()) == Error::OK);

    const std::array<std::uint8_t, 21> receiver_counters = {
        0x01, 0x00,                                    // count 1
        0x01, 0x02, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // id1 valid value 1
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};        // sampled 0, overflow 0
    const auto rc = w::decode_receiver_counters(view(receiver_counters));
    CHECK(rc.has_value() && rc.value().entries.count == 1U &&
          rc.value().entries.items[0].id == w::kCounterIdCapturedPackets &&
          rc.value().entries.items[0].counter.measurement.value == 1U);
    CHECK(w::validate_receiver_counters(rc.value()) == Error::OK);
}

void test_tune_golden()
{
    const std::array<std::uint8_t, 43> request_bytes = {
        0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d,
        0x1e, 0x1f, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x06, 0x05, 0x04,
        0x03, 0x02, 0x01, 0x00, 0x00, 0x02, 0x03, 0x00, 0x00, 0x00, 0x00, 0x30, 0x75, 0x00,
        0x00};
    const auto request = w::decode_tune_request(view(request_bytes));
    CHECK(request.has_value());
    CHECK(request.value().lease_id == 1U);
    CHECK(request.value().system == w::System::isdb_s);
    CHECK(request.value().frequency_hz == 0x0000010203040506ULL);
    CHECK(request.value().selection == w::Selection::slot && request.value().selection_value == 3U);
    CHECK(!request.value().lnb_voltage.present && !request.value().lna.present &&
          !request.value().pids.present);
    CHECK(request.value().timeout_ms == 30000U);
    w::TuneRequest built = request.value();
    std::array<std::uint8_t, 64> out{};
    const auto request_written = w::encode_tune_request(built, mutable_view(out));
    CHECK(request_written.has_value() && request_written.value() == request_bytes.size());
    CHECK(equals(out.data(), request_bytes.size(), request_bytes));

    const std::array<std::uint8_t, 48> result_bytes = {
        0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28, 0x29, 0x2a, 0x2b, 0x2c, 0x2d,
        0x2e, 0x2f, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x06, 0x05, 0x04, 0x03,
        0x02, 0x01, 0x00, 0x00, 0x00, 0x01, 0x02, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x01, 0x34, 0x12, 0x00};
    const auto result = w::decode_tune_result(view(result_bytes));
    CHECK(result.has_value() && result.value().tune_revision == 1U &&
          result.value().requested_hz == 0x0000010203040506ULL &&
          !result.value().programmed_hz.present &&
          result.value().verification == w::Verification::lock &&
          result.value().lock.state == w::MeasureState::valid && result.value().lock.value &&
          result.value().selected_tsid.present && result.value().selected_tsid.value == 0x1234U &&
          !result.value().selected_slot.present);
}

template <std::size_t N>
w::WString<N> fill_str(char c)
{
    w::WString<N> value;
    value.length = static_cast<std::uint16_t>(N);
    for (std::size_t i = 0U; i < N; ++i) {
        value.bytes[i] = c;
    }
    return value;
}

// Canonical same-epoch LIST matching base_status(): one function, receiver 0
// with no binding, no readers.
w::ListResponse canonical_list()
{
    w::ListResponse list;
    list.epoch = make_uuid(0x00U);
    list.producer.product = make_str<32>("px4");
    list.producer.version = make_str<64>("1");
    list.topology.state = w::TopologyState::complete;
    list.topology.expected_function_count.present = true;
    list.topology.expected_function_count.value = 1U;
    list.functions.count = 1U;
    list.functions.items[0] = minimal_usb_function(1U);
    list.receivers.present = true;
    list.receivers.value.count = 1U;
    list.receivers.value.items[0] = minimal_receiver(0U);
    list.readers.present = true;
    list.readers.value.count = 0U;
    return list;
}

w::ReceiverDescriptor max_receiver_descriptor(std::uint16_t id, std::uint16_t function_id)
{
    w::ReceiverDescriptor descriptor;
    descriptor.id = id;
    descriptor.systems = make_systems(true, true);
    descriptor.binding.present = true;
    descriptor.binding.value.function_id = function_id;
    descriptor.binding.value.local_id = id;
    descriptor.capabilities.ts_scope = w::TsScope::pid_subset;
    descriptor.capabilities.max_pids.present = true;
    descriptor.capabilities.max_pids.value = 8192U;
    descriptor.capabilities.controls =
        static_cast<std::uint8_t>(w::kControlBitLna | w::kControlBitLnb);
    descriptor.capabilities.lna.entries.count = 2U;
    descriptor.capabilities.lna.entries.items[0].system = w::System::isdb_t;
    descriptor.capabilities.lna.entries.items[0].default_value.present = true;
    descriptor.capabilities.lna.entries.items[0].default_value.value = false;
    descriptor.capabilities.lna.entries.items[1].system = w::System::isdb_s;
    descriptor.capabilities.lna.entries.items[1].default_value.present = true;
    descriptor.capabilities.lna.entries.items[1].default_value.value = true;
    descriptor.capabilities.lnb.values.count = 2U;
    descriptor.capabilities.lnb.values.items[0] = 0U;
    descriptor.capabilities.lnb.values.items[1] = 15U;
    descriptor.capabilities.measurement_mask = w::kMetricMaskAll;
    return descriptor;
}

w::ReaderDescriptor max_reader_descriptor(std::uint16_t id, std::uint16_t function_id)
{
    w::ReaderDescriptor descriptor;
    descriptor.id = id;
    descriptor.binding.present = true;
    descriptor.binding.value.function_id = function_id;
    descriptor.binding.value.local_id = id;
    return descriptor;
}

w::UsbFunction max_usb_function(std::uint16_t id)
{
    w::UsbFunction function;
    function.id = id;
    function.vid = 0x1234U;
    function.pid = 0x5678U;
    function.mode = w::Mode::runtime;
    function.location.usb_path.present = true;
    function.location.usb_path.value = fill_str<64>('p');
    function.location.bus.present = true;
    function.location.bus.value = 1U;
    function.location.address.present = true;
    function.location.address.value = 2U;
    function.serial_state = w::SerialState::present;
    function.serial.present = true;
    function.serial.value = fill_str<512>('s');
    function.model.present = true;
    function.model.value.id = fill_str<64>('m');
    function.model.value.name = fill_str<128>('n');
    function.identification_reason = w::IdentificationReason::none;
    function.present = true;
    function.last_error.present = true;
    function.last_error.value.code = 7U;
    function.last_error.value.operation = fill_str<32>('o');
    function.last_error.value.detail = fill_str<256>('d');
    return function;
}

void test_status_golden_and_aggregate()
{
    // Independent literal vector for a minimal global STATUS.
    const std::array<std::uint8_t, 113> expected = {
        // producer
        0x03, 0x00, 'p', 'x', '4', 0x01, 0x00, '1', 0x00,
        // context
        0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d,
        0x0e, 0x0f, 0x00, 0x01, 0x00,
        // snapshot
        0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00,
        // scope: enclosure, id 0
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        // enclosure absent
        0x00,
        // topology complete, expected 1
        0x02, 0x01, 0x01, 0x00,
        // functions present, count 1
        0x01, 0x01, 0x00, 0x01, 0x00, 0x34, 0x12, 0x78, 0x56, 0x02, 0x00, 0x00, 0x00, 0x02,
        0x00, 0x00, 0x02, 0x01, 0x00,
        // receivers present, count 1
        0x01, 0x01, 0x00,
        // descriptor: id 0, systems T, binding absent, capabilities
        0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00,
        // present, state, operation, leased
        0x00, 0x00, 0x00, 0x00,
        // tune_revision 0
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        // current, last_successful, stream, last_stream absent
        0x00, 0x00, 0x00, 0x00,
        // measurements count 0, receiver_statistics count 0
        0x00, 0x00, 0x00, 0x00,
        // stream_statistics absent, last_error absent
        0x00, 0x00,
        // readers present, count 0
        0x01, 0x00, 0x00};
    const w::StatusResponse status = base_status();
    std::vector<std::uint8_t> out(70000);
    const auto written = w::encode_status_response(status, MutableByteView{out.data(), out.size()});
    CHECK(written.has_value() && written.value() == expected.size());
    CHECK(equals(out.data(), expected.size(), expected));
    const auto decoded = w::decode_status_response(view(expected));
    CHECK(decoded.has_value());
    CHECK(w::validate_status_response_scope(decoded.value()) == Error::OK);
    const w::ListResponse canonical = canonical_list();
    CHECK(w::validate_list_response(canonical) == Error::OK);
    w::StatusContext context;
    context.list = &canonical;
    CHECK(w::validate_status_response(decoded.value(), context) == Error::OK);
    // A caller comment is not enough: without a canonical context this fails.
    const w::StatusContext none;
    CHECK(w::validate_status_response(decoded.value(), none) == Error::INVALID_ARGUMENT);

    // A response from another epoch than the canonical list.
    w::StatusResponse other_epoch = base_status();
    other_epoch.context.epoch = make_uuid(0x10U);
    CHECK(w::validate_status_response(other_epoch, context) == Error::INVALID_ARGUMENT);

    // Membership: an ID outside the canonical receiver set.
    w::StatusResponse member = base_status();
    member.receivers.value.items[0].descriptor.id = 999U;
    CHECK(w::validate_status_response(member, context) == Error::INVALID_ARGUMENT);

    // Descriptor identity: altered systems, binding and capabilities.
    w::StatusResponse systems = base_status();
    systems.receivers.value.items[0].descriptor.systems = make_systems(false, true);
    CHECK(w::validate_status_response(systems, context) == Error::INVALID_ARGUMENT);
    w::StatusResponse caps = base_status();
    caps.receivers.value.items[0].descriptor.capabilities.measurement_mask = w::kMetricMaskLock;
    CHECK(w::validate_status_response(caps, context) == Error::INVALID_ARGUMENT);

    // Duplicate IDs.
    w::StatusResponse dup = base_status();
    dup.receivers.value.count = 2U;
    dup.receivers.value.items[1].descriptor = minimal_receiver(0U);
    CHECK(w::validate_status_response(dup, context) == Error::INVALID_ARGUMENT);

    // Limits versus the canonical public receiver count.
    w::StatusResponse bad_limits = base_status();
    bad_limits.enclosure.present = true;
    bad_limits.enclosure.value.limits.max_concurrent_streams.present = true;
    bad_limits.enclosure.value.limits.max_concurrent_streams.value = 2U;
    CHECK(w::validate_status_response(bad_limits, context) == Error::INVALID_ARGUMENT);

    // Sample time after the snapshot, with a canonical descriptor that matches.
    w::ListResponse timed = canonical_list();
    timed.receivers.value.items[0].capabilities.measurement_mask = w::kMetricMaskLock;
    w::StatusContext timed_context;
    timed_context.list = &timed;
    w::StatusResponse stale_measure = base_status();
    stale_measure.receivers.value.items[0].descriptor.capabilities.measurement_mask =
        w::kMetricMaskLock;
    stale_measure.receivers.value.items[0].measurements.entries.count = 1U;
    stale_measure.receivers.value.items[0].measurements.entries.items[0].id = w::kMetricIdLock;
    stale_measure.receivers.value.items[0].measurements.entries.items[0].boolean.state =
        w::MeasureState::valid;
    stale_measure.receivers.value.items[0].measurements.entries.items[0].boolean.sampled_at_ns = 999U;
    CHECK(w::validate_status_response(stale_measure, timed_context) == Error::INVALID_ARGUMENT);
    stale_measure.receivers.value.items[0].measurements.entries.items[0].boolean.sampled_at_ns = 5U;
    CHECK(w::validate_status_response(stale_measure, timed_context) == Error::OK);

    // An unbound partial record still needs membership proof.
    w::ListResponse bound = canonical_list();
    bound.receivers.value.items[0].binding.present = true;
    bound.receivers.value.items[0].binding.value.function_id = 1U;
    w::StatusContext bound_context;
    bound_context.list = &bound;
    w::StatusResponse partial = base_status();
    partial.functions.present = false;
    partial.readers.present = false;
    partial.scope.kind = w::ScopeKind::receiver;
    partial.scope.id = 0U;
    partial.receivers.value.items[0].descriptor = bound.receivers.value.items[0];
    CHECK(w::validate_status_response(partial, bound_context) == Error::OK);
    partial.receivers.value.items[0].descriptor.binding.value.function_id = 2U;
    CHECK(w::validate_status_response(partial, bound_context) == Error::INVALID_ARGUMENT);
    partial.receivers.value.items[0].descriptor = bound.receivers.value.items[0];
    partial.receivers.value.items[0].descriptor.id = 3U;
    CHECK(w::validate_status_response(partial, bound_context) == Error::INVALID_ARGUMENT);

    // A reader missing from the canonical list is rejected.
    w::ListResponse with_reader = canonical_list();
    with_reader.readers.value.count = 1U;
    with_reader.readers.value.items[0].id = 2U;
    w::StatusContext reader_context;
    reader_context.list = &with_reader;
    w::StatusResponse reader_scope = base_status();
    reader_scope.functions.present = false;
    reader_scope.receivers.present = false;
    reader_scope.scope.kind = w::ScopeKind::reader;
    reader_scope.scope.id = 2U;
    reader_scope.readers.present = true;
    reader_scope.readers.value.count = 1U;
    reader_scope.readers.value.items[0].descriptor.id = 2U;
    reader_scope.readers.value.items[0].card.reason =
        static_cast<std::uint8_t>(w::UnknownReason::not_sampled);
    CHECK(w::validate_status_response(reader_scope, reader_context) == Error::OK);
    reader_scope.readers.value.items[0].descriptor.id = 3U;
    reader_scope.scope.id = 3U;
    CHECK(w::validate_status_response(reader_scope, reader_context) == Error::INVALID_ARGUMENT);

    // A retained stream may only be queried while its receiver is still leased.
    w::StatusResponse retained = base_status();
    retained.functions.present = false;
    retained.readers.present = false;
    retained.scope.kind = w::ScopeKind::stream;
    retained.scope.id = 0x1122334455667788ULL;
    retained.receivers.value.items[0].last_stream.present = true;
    retained.receivers.value.items[0].last_stream.value.id = 0x1122334455667788ULL;
    retained.receivers.value.items[0].last_stream.value.tune_revision = 1U;
    retained.receivers.value.items[0].last_stream.value.reason = w::StreamEndReason::stopped;
    retained.receivers.value.items[0].last_stream.value.statistics.entries.count = 2U;
    retained.receivers.value.items[0].last_stream.value.statistics.entries.items[0].id =
        w::kCounterIdTransmittedPackets;
    retained.receivers.value.items[0].last_stream.value.statistics.entries.items[0].counter =
        unknown_counter(w::UnknownReason::stale);
    retained.receivers.value.items[0].last_stream.value.statistics.entries.items[1].id =
        w::kCounterIdQueueDroppedPackets;
    retained.receivers.value.items[0].last_stream.value.statistics.entries.items[1].counter =
        unknown_counter(w::UnknownReason::stale);
    CHECK(w::validate_status_response(retained, context) == Error::INVALID_ARGUMENT);
    retained.receivers.value.items[0].leased = true;
    CHECK(w::validate_status_response(retained, context) == Error::OK);

    // A global snapshot may not hide a known canonical topology by claiming
    // unknown and omitting both collections.
    w::StatusResponse masked = base_status();
    masked.topology.state = w::TopologyState::unknown;
    masked.receivers.present = false;
    masked.readers.present = false;
    CHECK(w::validate_status_response(masked, context) == Error::INVALID_ARGUMENT);
    // A genuinely unknown canonical topology requires exactly that omission.
    w::ListResponse unknown_list = canonical_list();
    unknown_list.topology.state = w::TopologyState::unknown;
    unknown_list.receivers.present = false;
    unknown_list.readers.present = false;
    CHECK(w::validate_list_response(unknown_list) == Error::OK);
    w::StatusContext unknown_context;
    unknown_context.list = &unknown_list;
    w::StatusResponse unknown_status = base_status();
    unknown_status.topology.state = w::TopologyState::unknown;
    unknown_status.receivers.present = false;
    unknown_status.readers.present = false;
    CHECK(w::validate_status_response(unknown_status, unknown_context) == Error::OK);
    // A known canonical topology with an empty reader set (present, count 0) is
    // already the base case above; omitting the reader array is rejected.
    w::StatusResponse no_readers = base_status();
    no_readers.readers.present = false;
    CHECK(w::validate_status_response(no_readers, context) == Error::INVALID_ARGUMENT);

    // Complete 3x4 STATUS identity matrix: uniqueness is unknown while the
    // serial state is present or unknown, and not_applicable only on confirmed
    // absence. unique_in_scan and duplicate are never valid for STATUS.
    const w::IdentitySerialState identity_states[3] = {
        w::IdentitySerialState::unknown,
        w::IdentitySerialState::present,
        w::IdentitySerialState::absent,
    };
    const w::Uniqueness uniqueness_values[4] = {
        w::Uniqueness::unknown,
        w::Uniqueness::unique_in_scan,
        w::Uniqueness::duplicate,
        w::Uniqueness::not_applicable,
    };
    for (const auto state : identity_states) {
        for (const auto uniqueness : uniqueness_values) {
            w::StatusResponse identity_status = base_status();
            identity_status.enclosure.present = true;
            identity_status.enclosure.value.identity.serial_state = state;
            if (state == w::IdentitySerialState::present) {
                identity_status.enclosure.value.identity.serial.present = true;
                identity_status.enclosure.value.identity.serial.value = make_str<512>("sn");
            } else {
                identity_status.enclosure.value.identity.serial.present = false;
            }
            identity_status.enclosure.value.identity.uniqueness = uniqueness;
            const bool expected =
                (state != w::IdentitySerialState::absent && uniqueness == w::Uniqueness::unknown) ||
                (state == w::IdentitySerialState::absent &&
                 uniqueness == w::Uniqueness::not_applicable);
            CHECK(w::validate_status_response(identity_status, context) ==
                  (expected ? Error::OK : Error::INVALID_ARGUMENT));
        }
    }

    // A serial-state/value mismatch is rejected by the local codec check.
    w::StatusResponse mismatch = base_status();
    mismatch.enclosure.present = true;
    mismatch.enclosure.value.identity.serial_state = w::IdentitySerialState::present;
    mismatch.enclosure.value.identity.serial.present = false;
    mismatch.enclosure.value.identity.uniqueness = w::Uniqueness::unknown;
    CHECK(w::validate_status_response(mismatch, context) == Error::INVALID_ARGUMENT);
    mismatch.enclosure.value.identity.serial_state = w::IdentitySerialState::unknown;
    mismatch.enclosure.value.identity.serial.present = true;
    mismatch.enclosure.value.identity.serial.value = make_str<512>("sn");
    CHECK(w::validate_status_response(mismatch, context) == Error::INVALID_ARGUMENT);
    mismatch.enclosure.value.identity.serial_state = w::IdentitySerialState::absent;
    mismatch.enclosure.value.identity.serial.present = true;
    CHECK(w::validate_status_response(mismatch, context) == Error::INVALID_ARGUMENT);
}

void test_max_populated_status()
{
    // Every bounded field at its legal maximum in one record. No two maxima are
    // byte-exclusive here: a receiver may hold both a current and a last
    // successful tune, both a current and a last stream, and a full measurement
    // and counter set at once, so the size is the plain sum of the field maxima.
    w::StatusResponse status = base_status();
    status.producer.product = fill_str<32>('p');
    status.producer.version = fill_str<64>('v');
    status.producer.core_version.present = true;
    status.producer.core_version.value = fill_str<64>('c');
    status.context.instance.present = true;
    status.context.instance.value = fill_str<64>('i');
    status.context.last_error.present = true;
    status.context.last_error.value.code = 9U;
    status.context.last_error.value.operation = fill_str<32>('o');
    status.context.last_error.value.detail = fill_str<256>('d');
    status.snapshot.observed_at_ns = 1000U;
    status.scope.kind = w::ScopeKind::enclosure;
    status.topology.state = w::TopologyState::complete;
    status.topology.expected_function_count.present = true;
    status.topology.expected_function_count.value = 8U;
    status.enclosure.present = true;
    status.enclosure.value.id = 0U;
    status.enclosure.value.model.present = true;
    status.enclosure.value.model.value.id = fill_str<64>('m');
    status.enclosure.value.model.value.name = fill_str<128>('n');
    status.enclosure.value.identity.serial_state = w::IdentitySerialState::present;
    status.enclosure.value.identity.serial.present = true;
    status.enclosure.value.identity.serial.value = fill_str<512>('s');
    status.enclosure.value.identity.uniqueness = w::Uniqueness::unknown;
    status.enclosure.value.limits.max_concurrent_streams.present = true;
    status.enclosure.value.limits.max_concurrent_streams.value = 32U;

    status.functions.present = true;
    status.functions.value.count = 8U;
    for (std::size_t i = 0U; i < 8U; ++i) {
        status.functions.value.items[i] = max_usb_function(static_cast<std::uint16_t>(i + 1U));
    }

    status.receivers.present = true;
    status.receivers.value.count = 32U;
    for (std::size_t i = 0U; i < 32U; ++i) {
        w::ReceiverStatus& receiver = status.receivers.value.items[i];
        receiver.descriptor = max_receiver_descriptor(static_cast<std::uint16_t>(i), 1U);
        receiver.present = true;
        receiver.state = w::ReceiverState::streaming;
        receiver.operation = w::Operation::idle;
        receiver.leased = true;
        receiver.tune_revision = 3U;
        receiver.current_tune.present = true;
        receiver.current_tune.value.revision = 3U;
        receiver.current_tune.value.system = w::System::isdb_s;
        receiver.current_tune.value.frequency_hz = 1049480000ULL;
        receiver.current_tune.value.programmed_hz.present = true;
        receiver.current_tune.value.programmed_hz.value = 1049480000ULL;
        receiver.current_tune.value.selection = w::Selection::tsid;
        receiver.current_tune.value.selection_value = 0x1234U;
        receiver.current_tune.value.verification = w::Verification::lock;
        receiver.last_successful_tune = receiver.current_tune;
        receiver.stream.present = true;
        receiver.stream.value.id = 100U + i;
        receiver.stream.value.tune_revision = 3U;
        receiver.stream.value.phase = w::StreamPhase::active;
        receiver.last_stream.present = true;
        receiver.last_stream.value.id = 200U + i;
        receiver.last_stream.value.tune_revision = 3U;
        receiver.last_stream.value.reason = w::StreamEndReason::stopped;
        receiver.last_stream.value.error.present = true;
        receiver.last_stream.value.error.value.code = 9U;
        receiver.last_stream.value.error.value.operation = fill_str<32>('o');
        receiver.last_stream.value.error.value.detail = fill_str<256>('d');
        receiver.last_stream.value.statistics.entries.count = 2U;
        receiver.last_stream.value.statistics.entries.items[0].id =
            w::kCounterIdTransmittedPackets;
        receiver.last_stream.value.statistics.entries.items[0].counter =
            valid_counter(100U, 5U, false);
        receiver.last_stream.value.statistics.entries.items[1].id =
            w::kCounterIdQueueDroppedPackets;
        receiver.last_stream.value.statistics.entries.items[1].counter =
            valid_counter(0U, 5U, false);
        receiver.measurements.entries.count = 3U;
        receiver.measurements.entries.items[0].id = w::kMetricIdLock;
        receiver.measurements.entries.items[0].boolean.state = w::MeasureState::valid;
        receiver.measurements.entries.items[0].boolean.value = true;
        receiver.measurements.entries.items[0].boolean.sampled_at_ns = 5U;
        receiver.measurements.entries.items[1].id = w::kMetricIdCnrMdb;
        receiver.measurements.entries.items[1].integer.state = w::MeasureState::valid;
        receiver.measurements.entries.items[1].integer.value = 12345;
        receiver.measurements.entries.items[1].integer.sampled_at_ns = 5U;
        receiver.measurements.entries.items[2].id = w::kMetricIdSignalMdbm;
        receiver.measurements.entries.items[2].integer.state = w::MeasureState::valid;
        receiver.measurements.entries.items[2].integer.value = -60;
        receiver.measurements.entries.items[2].integer.sampled_at_ns = 5U;
        receiver.receiver_statistics.entries.count = 4U;
        for (std::size_t c = 0U; c < 4U; ++c) {
            receiver.receiver_statistics.entries.items[c].id =
                static_cast<std::uint8_t>(w::kCounterIdCapturedPackets + c);
            receiver.receiver_statistics.entries.items[c].counter =
                valid_counter(1000U + c, 5U, false);
        }
        receiver.stream_statistics.present = true;
        receiver.stream_statistics.value.entries.count = 2U;
        receiver.stream_statistics.value.entries.items[0].id = w::kCounterIdTransmittedPackets;
        receiver.stream_statistics.value.entries.items[0].counter =
            valid_counter(0xffffffffffffffffULL, 5U, true);
        receiver.stream_statistics.value.entries.items[1].id = w::kCounterIdQueueDroppedPackets;
        receiver.stream_statistics.value.entries.items[1].counter = valid_counter(7U, 5U, false);
        receiver.last_error.present = true;
        receiver.last_error.value.code = 7U;
        receiver.last_error.value.operation = fill_str<32>('o');
        receiver.last_error.value.detail = fill_str<256>('d');
    }

    status.readers.present = true;
    status.readers.value.count = 8U;
    for (std::size_t i = 0U; i < 8U; ++i) {
        w::ReaderStatus& reader = status.readers.value.items[i];
        reader.descriptor = max_reader_descriptor(static_cast<std::uint16_t>(i), 1U);
        reader.present = true;
        reader.card.state = w::CardState::present;
        reader.card.sampled_at_ns.present = true;
        reader.card.sampled_at_ns.value = 5U;
        reader.operation = w::CardOperation::idle;
        reader.handles_open = 64U;
        reader.transaction_active = true;
        reader.last_error.present = true;
        reader.last_error.value.code = 12U;
        reader.last_error.value.operation = fill_str<32>('o');
        reader.last_error.value.detail = fill_str<256>('d');
    }

    // Independent size computation: the plain sum of the legal field maxima.
    // producer 167 + context 381 + snapshot 16 + scope 9 + enclosure 720 +
    // topology 4 + functions (3 + 8*1090) + receivers (3 + 32*930) +
    // readers (3 + 8*320) = 42346. Each receiver carries three valid long
    // measurements (11 + 14 + 14) and long counters, so the signal metric is the
    // valid form, not the two-byte unknown form.
    const std::size_t expected_size = 42346U;
    CHECK(expected_size <= w::kMaxControlPayload);

    std::vector<std::uint8_t> out(70000);
    const auto written = w::encode_status_response(status, MutableByteView{out.data(), out.size()});
    CHECK(written.has_value() && written.value() == expected_size);
    CHECK(written.value() <= w::kMaxControlPayload);

    // A buffer one byte short is left untouched and reports a short caller
    // buffer, which is distinct from RESOURCE_LIMIT.
    std::vector<std::uint8_t> short_buffer(expected_size - 1U, 0xaaU);
    const auto short_written = w::encode_status_response(
        status, MutableByteView{short_buffer.data(), short_buffer.size()});
    CHECK(short_written.error() == Error::BUFFER_TOO_SMALL);
    bool untouched = true;
    for (std::size_t i = 0U; i < short_buffer.size(); ++i) {
        if (short_buffer[i] != 0xaaU) {
            untouched = false;
        }
    }
    CHECK(untouched);

    // The canonical same-epoch LIST mirrors the published descriptors.
    w::ListResponse canonical;
    canonical.epoch = make_uuid(0x00U);
    canonical.producer.product = make_str<32>("px4");
    canonical.producer.version = make_str<64>("1");
    canonical.topology.state = w::TopologyState::complete;
    canonical.topology.expected_function_count.present = true;
    canonical.topology.expected_function_count.value = 8U;
    canonical.functions.count = 8U;
    for (std::size_t i = 0U; i < 8U; ++i) {
        canonical.functions.items[i] = max_usb_function(static_cast<std::uint16_t>(i + 1U));
    }
    canonical.receivers.present = true;
    canonical.receivers.value.count = 32U;
    for (std::size_t i = 0U; i < 32U; ++i) {
        canonical.receivers.value.items[i] =
            max_receiver_descriptor(static_cast<std::uint16_t>(i), 1U);
    }
    canonical.readers.present = true;
    canonical.readers.value.count = 8U;
    for (std::size_t i = 0U; i < 8U; ++i) {
        canonical.readers.value.items[i] = max_reader_descriptor(static_cast<std::uint16_t>(i), 1U);
    }
    CHECK(w::validate_list_response(canonical) == Error::OK);
    const auto decoded = w::decode_status_response(ByteView{out.data(), written.value()});
    CHECK(decoded.has_value() && decoded.value().receivers.value.count == 32U);
    w::StatusContext context;
    context.list = &canonical;
    CHECK(w::validate_status_response(decoded.value(), context) == Error::OK);
}

void test_scalar_and_completion_negatives()
{
    std::array<std::uint8_t, 64> out{};
    std::array<std::uint8_t, 24> zero_request{};

    w::StartRequest start;
    start.epoch = make_uuid(0x00U);
    start.tune_revision = 1U;
    CHECK(w::encode_start_request(start, mutable_view(out)).error() == Error::INVALID_ARGUMENT);
    start.lease_id = 1U;
    CHECK(w::encode_start_request(start, mutable_view(out)).has_value());
    CHECK(w::decode_start_request(view(zero_request)).error() == Error::INVALID_ARGUMENT);

    w::AttachRequest attach;
    attach.epoch = make_uuid(0x00U);
    attach.lease_id = 0U;
    attach.stream_id = 2U;
    CHECK(w::encode_attach_request(attach, mutable_view(out)).error() == Error::INVALID_ARGUMENT);
    attach.lease_id = 1U;
    attach.stream_id = 0U;
    CHECK(w::encode_attach_request(attach, mutable_view(out)).error() == Error::INVALID_ARGUMENT);
    attach.stream_id = 2U;
    CHECK(w::encode_attach_request(attach, mutable_view(out)).has_value());

    w::StopRequest stop;
    stop.epoch = make_uuid(0x00U);
    stop.stream_id = 2U;
    CHECK(w::encode_stop_request(stop, mutable_view(out)).error() == Error::INVALID_ARGUMENT);
    stop.lease_id = 1U;
    stop.stream_id = 0U;
    CHECK(w::encode_stop_request(stop, mutable_view(out)).error() == Error::INVALID_ARGUMENT);

    w::ReleaseRequest release;
    release.epoch = make_uuid(0x00U);
    CHECK(w::encode_release_request(release, mutable_view(out)).error() == Error::INVALID_ARGUMENT);
    release.lease_id = 1U;
    CHECK(w::encode_release_request(release, mutable_view(out)).has_value());

    // STOP quiesced=false requires the final error.
    w::StreamFinal final;
    final.id = 1U;
    final.tune_revision = 1U;
    final.reason = w::StreamEndReason::stopped;
    final.statistics.entries.count = 2U;
    final.statistics.entries.items[0].id = w::kCounterIdTransmittedPackets;
    final.statistics.entries.items[0].counter = unknown_counter(w::UnknownReason::stale);
    final.statistics.entries.items[1].id = w::kCounterIdQueueDroppedPackets;
    final.statistics.entries.items[1].counter = unknown_counter(w::UnknownReason::stale);
    w::StopResponse stop_response;
    stop_response.epoch = make_uuid(0x00U);
    stop_response.final = final;
    stop_response.quiesced = false;
    CHECK(w::encode_stop_response(stop_response, mutable_view(out)).error() ==
          Error::INVALID_ARGUMENT);
    stop_response.final.error.present = true;
    stop_response.final.error.value.code = 8U;
    CHECK(w::encode_stop_response(stop_response, mutable_view(out)).has_value());
    stop_response.final.id = 0U;
    CHECK(w::encode_stop_response(stop_response, mutable_view(out)).error() ==
          Error::INVALID_ARGUMENT);
    stop_response.final.id = 1U;
    stop_response.final.tune_revision = 0U;
    CHECK(w::encode_stop_response(stop_response, mutable_view(out)).error() ==
          Error::INVALID_ARGUMENT);

    // Card handle keys and issued generations are nonzero.
    w::CardHandleKey key;
    key.epoch = make_uuid(0x00U);
    key.bound_generation = 0U;
    key.card_handle = 1U;
    CHECK(w::encode_card_handle_key(key, mutable_view(out)).error() == Error::INVALID_ARGUMENT);
    key.bound_generation = 1U;
    key.card_handle = 0U;
    CHECK(w::encode_card_handle_key(key, mutable_view(out)).error() == Error::INVALID_ARGUMENT);
    key.card_handle = 1U;
    CHECK(w::encode_card_handle_key(key, mutable_view(out)).has_value());

    w::CardStatusResponse card_status;
    card_status.key.epoch = make_uuid(0x00U);
    card_status.current_generation = 0U;
    CHECK(w::encode_card_status_response(card_status, mutable_view(out)).error() ==
          Error::INVALID_ARGUMENT);
    card_status.current_generation = 1U;
    CHECK(w::encode_card_status_response(card_status, mutable_view(out)).has_value());

    w::CardTransmitRequest transmit;
    transmit.key.epoch = make_uuid(0x00U);
    transmit.key.bound_generation = 1U;
    transmit.key.card_handle = 1U;
    transmit.protocol = w::CardProtocol::t1;
    CHECK(w::encode_card_transmit_request(transmit, mutable_view(out)).error() ==
          Error::INVALID_ARGUMENT);
    transmit.command.length = 1U;
    transmit.command.bytes[0] = 0x00U;
    CHECK(w::encode_card_transmit_request(transmit, mutable_view(out)).has_value());

    // ISDB-T cannot select TSID or slot.
    w::TuneRequest tune;
    tune.epoch = make_uuid(0x00U);
    tune.lease_id = 1U;
    tune.system = w::System::isdb_t;
    tune.frequency_hz = 473142857ULL;
    tune.selection = w::Selection::tsid;
    tune.selection_value = 1U;
    CHECK(w::encode_tune_request(tune, mutable_view(out)).error() == Error::INVALID_ARGUMENT);
    tune.selection = w::Selection::automatic;
    tune.selection_value = 0U;
    CHECK(w::encode_tune_request(tune, mutable_view(out)).has_value());

    // An absent model must not claim identification reason none.
    w::UsbFunction function;
    CHECK(w::encode_usb_function(function, mutable_view(out)).error() == Error::INVALID_ARGUMENT);
    function.identification_reason = w::IdentificationReason::cold_model;
    CHECK(w::encode_usb_function(function, mutable_view(out)).has_value());

    // A saturated counter must carry the maximum value in the codec itself.
    w::StreamCounters saturating;
    saturating.entries.count = 2U;
    saturating.entries.items[0].id = w::kCounterIdTransmittedPackets;
    saturating.entries.items[0].counter = valid_counter(5U, 1U, true);
    saturating.entries.items[1].id = w::kCounterIdQueueDroppedPackets;
    saturating.entries.items[1].counter = unknown_counter(w::UnknownReason::stale);
    CHECK(w::encode_stream_counters(saturating, mutable_view(out)).error() ==
          Error::INVALID_ARGUMENT);
    saturating.entries.items[0].counter.measurement.value = 0xffffffffffffffffULL;
    CHECK(w::encode_stream_counters(saturating, mutable_view(out)).has_value());

    // The status Tune record also refuses an ISDB-T TSID/slot selection.
    w::Tune tuned;
    tuned.revision = 1U;
    tuned.system = w::System::isdb_t;
    tuned.frequency_hz = 473142857ULL;
    tuned.selection = w::Selection::tsid;
    tuned.selection_value = 1U;
    CHECK(w::encode_tune(tuned, mutable_view(out)).error() == Error::INVALID_ARGUMENT);
    tuned.selection = w::Selection::automatic;
    tuned.selection_value = 0U;
    CHECK(w::encode_tune(tuned, mutable_view(out)).has_value());
}

void test_frequency_and_lna_mutations()
{
    std::array<std::uint8_t, 64> out{};

    // programmed_hz is a frequency: present values must be 1..2^53-1.
    w::Tune tune;
    tune.revision = 1U;
    tune.system = w::System::isdb_s;
    tune.frequency_hz = 1U;
    tune.selection = w::Selection::automatic;
    tune.selection_value = 0U;
    tune.programmed_hz.present = true;
    tune.programmed_hz.value = 0U;
    CHECK(w::encode_tune(tune, mutable_view(out)).error() == Error::INVALID_ARGUMENT);
    tune.programmed_hz.value = 9007199254740992ULL;  // 2^53
    CHECK(w::encode_tune(tune, mutable_view(out)).error() == Error::INVALID_ARGUMENT);
    tune.programmed_hz.value = 1U;
    CHECK(w::encode_tune(tune, mutable_view(out)).has_value());
    tune.programmed_hz.value = 9007199254740991ULL;  // 2^53-1
    CHECK(w::encode_tune(tune, mutable_view(out)).has_value());

    w::TuneResult result;
    result.epoch = make_uuid(0x00U);
    result.tune_revision = 1U;
    result.requested_hz = 1U;
    result.programmed_hz.present = true;
    result.programmed_hz.value = 0U;
    CHECK(w::encode_tune_result(result, mutable_view(out)).error() == Error::INVALID_ARGUMENT);
    result.programmed_hz.value = 9007199254740992ULL;
    CHECK(w::encode_tune_result(result, mutable_view(out)).error() == Error::INVALID_ARGUMENT);
    result.programmed_hz.value = 1U;
    const auto result_written = w::encode_tune_result(result, mutable_view(out));
    CHECK(result_written.has_value());
    // The decoded bytes with a zero programmed value are rejected too.
    std::array<std::uint8_t, 64> decode_bytes{};
    for (std::size_t i = 0U; i < result_written.value(); ++i) {
        decode_bytes[i] = out[i];
    }
    for (std::size_t i = 33U; i < 41U; ++i) {
        decode_bytes[i] = 0U;
    }
    CHECK(w::decode_tune_result(ByteView{decode_bytes.data(), result_written.value()}).error() ==
          Error::INVALID_ARGUMENT);

    // The complete aggregate validator rejects it as well.
    w::ReceiverStatus receiver;
    receiver.descriptor = minimal_receiver(0U);
    receiver.current_tune.present = true;
    receiver.current_tune.value.revision = 1U;
    receiver.current_tune.value.system = w::System::isdb_t;
    receiver.current_tune.value.frequency_hz = 1U;
    receiver.current_tune.value.programmed_hz.present = true;
    receiver.current_tune.value.programmed_hz.value = 0U;
    CHECK(w::validate_receiver_status(receiver) == Error::INVALID_ARGUMENT);

    // LNA defaults must exactly cover the descriptor's supported systems.
    w::ReceiverDescriptor descriptor = minimal_receiver(1U);  // isdb-t only
    descriptor.capabilities.controls = w::kControlBitLna;
    descriptor.capabilities.lna.entries.count = 1U;
    descriptor.capabilities.lna.entries.items[0].system = w::System::isdb_s;
    CHECK(w::validate_receiver_descriptor(descriptor) == Error::INVALID_ARGUMENT);
    descriptor.systems = make_systems(true, true);
    descriptor.capabilities.lna.entries.count = 1U;
    descriptor.capabilities.lna.entries.items[0].system = w::System::isdb_t;
    CHECK(w::validate_receiver_descriptor(descriptor) == Error::INVALID_ARGUMENT);
    descriptor.capabilities.lna.entries.count = 2U;
    descriptor.capabilities.lna.entries.items[0].system = w::System::isdb_t;
    descriptor.capabilities.lna.entries.items[1].system = w::System::isdb_s;
    CHECK(w::validate_receiver_descriptor(descriptor) == Error::OK);
    // Extra/duplicate defaults.
    descriptor.systems = make_systems(true, false);
    CHECK(w::validate_receiver_descriptor(descriptor) == Error::INVALID_ARGUMENT);
    descriptor.systems = make_systems(true, true);
    descriptor.capabilities.lna.entries.items[1].system = w::System::isdb_t;
    CHECK(w::validate_receiver_descriptor(descriptor) == Error::INVALID_ARGUMENT);

    // The canonical LIST validator applies the same rule.
    w::ListResponse list = canonical_list();
    list.receivers.value.items[0] = minimal_receiver(0U);
    list.receivers.value.items[0].capabilities.controls = w::kControlBitLna;
    list.receivers.value.items[0].capabilities.lna.entries.count = 1U;
    list.receivers.value.items[0].capabilities.lna.entries.items[0].system = w::System::isdb_s;
    CHECK(w::validate_list_response(list) == Error::INVALID_ARGUMENT);
    list.receivers.value.items[0].systems = make_systems(true, true);
    list.receivers.value.items[0].capabilities.lna.entries.count = 2U;
    list.receivers.value.items[0].capabilities.lna.entries.items[0].system = w::System::isdb_t;
    list.receivers.value.items[0].capabilities.lna.entries.items[1].system = w::System::isdb_s;
    CHECK(w::validate_list_response(list) == Error::OK);
}

void test_direct_invalid_typed_values()
{
    // A direct typed caller cannot bypass the local syntax/value rules with a
    // static_cast that the codec would reject.
    w::Measurements measurements;
    measurements.entries.count = 1U;
    measurements.entries.items[0].id = w::kMetricIdLock;
    measurements.entries.items[0].boolean.state = static_cast<w::MeasureState>(3);
    CHECK(w::validate_measurements(measurements, w::kMetricMaskLock) == Error::INVALID_ARGUMENT);
    measurements.entries.items[0].boolean.state = w::MeasureState::unknown;
    measurements.entries.items[0].boolean.reason = static_cast<w::UnknownReason>(0);
    CHECK(w::validate_measurements(measurements, w::kMetricMaskLock) == Error::INVALID_ARGUMENT);
    measurements.entries.items[0].boolean.reason = w::UnknownReason::stale;
    CHECK(w::validate_measurements(measurements, w::kMetricMaskLock) == Error::OK);
    measurements.entries.items[0].id = 9U;
    CHECK(w::validate_measurements(measurements, w::kMetricMaskLock) == Error::INVALID_ARGUMENT);

    w::StreamCounters saturating;
    saturating.entries.count = 2U;
    saturating.entries.items[0].id = w::kCounterIdTransmittedPackets;
    saturating.entries.items[0].counter = valid_counter(5U, 1U, true);
    saturating.entries.items[1].id = w::kCounterIdQueueDroppedPackets;
    saturating.entries.items[1].counter = unknown_counter(w::UnknownReason::stale);
    CHECK(w::validate_stream_counters(saturating) == Error::INVALID_ARGUMENT);

    w::Capabilities capabilities;
    capabilities.ts_scope = static_cast<w::TsScope>(5);
    CHECK(w::validate_capabilities(capabilities) == Error::INVALID_ARGUMENT);

    w::ReceiverStatus receiver;
    receiver.descriptor = minimal_receiver(0U);
    receiver.state = static_cast<w::ReceiverState>(9);
    CHECK(w::validate_receiver_status(receiver) == Error::INVALID_ARGUMENT);

    // The same invalid values fail in decoded bytes.
    const std::array<std::uint8_t, 4> bad_state = {0x01, 0x00, 0x01, 0x03};
    CHECK(w::decode_measurements(view(bad_state)).error() == Error::INVALID_ARGUMENT);
    const std::array<std::uint8_t, 5> bad_reason = {0x01, 0x00, 0x01, 0x01, 0x00};
    CHECK(w::decode_measurements(view(bad_reason)).error() == Error::INVALID_ARGUMENT);

    // The new additive error value is present and preserves existing values.
    CHECK(std::string(dtv::userland::error_string(Error::RESOURCE_LIMIT)) == "RESOURCE_LIMIT");
    CHECK(static_cast<int>(Error::RESOURCE_LIMIT) == 17);
    CHECK(static_cast<int>(Error::INTERNAL) == 255);
    CHECK(static_cast<int>(Error::SLOW_CONSUMER) == 15);
}

}  // namespace

int main()
{
    test_constants();
    test_hello_response_golden();
    test_error_response_golden();
    test_ts_data_golden();
    test_state_event_golden();
    test_card_session_golden();
    test_stream_lifecycle_golden();
    test_tune();
    test_measurements_and_counters();
    test_capabilities();
    test_capabilities_golden();
    test_measurement_and_counter_golden();
    test_tune_golden();
    test_scope_and_card_validation();
    test_error_codes();
    test_strings_and_malformed();
    test_arrays_and_caps();
    test_status_golden_and_aggregate();
    test_max_populated_status();
    test_frequency_and_lna_mutations();
    test_direct_invalid_typed_values();
    test_scalar_and_completion_negatives();
    test_status_scope_rules();
    test_absent_vs_empty_and_sparse();
    test_monitoring_has_no_owner_secrets();
    test_fragmented_frame();
    if (g_failures != 0) {
        std::fprintf(stderr, "records_tests: %d of %d checks failed\n", g_failures, g_checks);
        return 1;
    }
    std::printf("records_tests: %d checks passed\n", g_checks);
    return 0;
}
