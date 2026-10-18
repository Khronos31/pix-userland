// SPDX-License-Identifier: MIT
#include "dtv/userland/error.h"
#include "dtv/userland/logging.h"
#include "dtv/userland/mock_transport.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string_view>

namespace {

using namespace dtv::userland;

// 共通側は装置値を既定値として持たないため、試験側で明示的に与える。
// この値はPX4の既存既定に対応する。
constexpr MockTransportPolicy kPolicy{65535U, 1048576U, 0x84U};
constexpr std::uint8_t kCommandInEndpoint = 0x81U;
constexpr std::uint8_t kCommandOutEndpoint = 0x02U;

#define CHECK(condition)                                                                        \
    do {                                                                                        \
        if (!(condition)) {                                                                     \
            std::fprintf(stderr, "check failed at %s:%d: %s\n", __FILE__, __LINE__, #condition); \
            return false;                                                                       \
        }                                                                                       \
    } while (false)

bool test_error_contract()
{
    static_assert(static_cast<std::uint8_t>(Error::OK) == 0U);
    static_assert(static_cast<std::uint8_t>(Error::INVALID_ARGUMENT) == 1U);
    static_assert(static_cast<std::uint8_t>(Error::VERSION_MISMATCH) == 2U);
    static_assert(static_cast<std::uint8_t>(Error::NOT_FOUND) == 3U);
    static_assert(static_cast<std::uint8_t>(Error::BUSY) == 4U);
    static_assert(static_cast<std::uint8_t>(Error::NOT_READY) == 5U);
    static_assert(static_cast<std::uint8_t>(Error::TIMEOUT) == 6U);
    static_assert(static_cast<std::uint8_t>(Error::USB_IO) == 7U);
    static_assert(static_cast<std::uint8_t>(Error::DISCONNECTED) == 8U);
    static_assert(static_cast<std::uint8_t>(Error::PROTOCOL_ERROR) == 9U);
    static_assert(static_cast<std::uint8_t>(Error::FIRMWARE_REJECTED) == 10U);
    static_assert(static_cast<std::uint8_t>(Error::UNSUPPORTED) == 11U);
    static_assert(static_cast<std::uint8_t>(Error::NO_CARD) == 12U);
    static_assert(static_cast<std::uint8_t>(Error::CARD_REMOVED) == 13U);
    static_assert(static_cast<std::uint8_t>(Error::BUFFER_TOO_SMALL) == 14U);
    static_assert(static_cast<std::uint8_t>(Error::SLOW_CONSUMER) == 15U);
    static_assert(static_cast<std::uint8_t>(Error::INTERNAL) == 255U);
    static_assert(sizeof(Error) == sizeof(std::uint8_t));

    constexpr std::array<const char*, 16> names{
        "OK", "INVALID_ARGUMENT", "VERSION_MISMATCH", "NOT_FOUND", "BUSY", "NOT_READY", "TIMEOUT",
        "USB_IO", "DISCONNECTED", "PROTOCOL_ERROR", "FIRMWARE_REJECTED", "UNSUPPORTED", "NO_CARD",
        "CARD_REMOVED", "BUFFER_TOO_SMALL", "SLOW_CONSUMER"};
    for (std::size_t index = 0U; index < names.size(); ++index) {
        CHECK(std::strcmp(error_string(static_cast<Error>(index)), names[index]) == 0);
    }
    CHECK(std::strcmp(error_string(Error::INTERNAL), "INTERNAL") == 0);
    CHECK(std::strcmp(error_string(static_cast<Error>(254U)), "UNKNOWN") == 0);

    const auto success = Result<std::uint32_t>::success(7U);
    CHECK(success.has_value());
    CHECK(static_cast<bool>(success));
    CHECK(success.value() == 7U);
    const auto failure = Result<std::uint32_t>::failure(Error::BUSY);
    CHECK(!failure.has_value());
    CHECK(!static_cast<bool>(failure));
    CHECK(failure.error() == Error::BUSY);
    CHECK(failure.value() == 0U);
    CHECK(static_cast<bool>(Result<void>::success()));
    CHECK(Result<void>::failure(Error::TIMEOUT).error() == Error::TIMEOUT);
    return true;
}

struct SinkCapture final {
    std::array<LogLevel, 4> levels{};
    std::array<std::string_view, 4> messages{};
    std::size_t count = 0U;
};

void capture_log(const LogRecord& record, void* context) noexcept
{
    auto* capture = static_cast<SinkCapture*>(context);
    if (capture->count < capture->levels.size()) {
        capture->levels[capture->count] = record.level;
        capture->messages[capture->count] = record.message;
    }
    ++capture->count;
}

bool test_logging()
{
    SinkCapture capture;
    Logger logger(LogLevel::warn, capture_log, &capture);
    CHECK(!logger.enabled(LogLevel::info));
    CHECK(logger.enabled(LogLevel::warn));
    logger.log(LogLevel::info, "ignored");
    logger.log(LogLevel::warn, "warning");
    logger.log(LogLevel::error, "failure");
    CHECK(capture.count == 2U);
    CHECK(capture.levels[0] == LogLevel::warn);
    CHECK(capture.messages[0] == "warning");
    CHECK(capture.levels[1] == LogLevel::error);
    CHECK(capture.messages[1] == "failure");
    CHECK(std::strcmp(log_level_string(LogLevel::trace), "TRACE") == 0);
    CHECK(std::strcmp(log_level_string(LogLevel::debug), "DEBUG") == 0);
    CHECK(std::strcmp(log_level_string(LogLevel::info), "INFO") == 0);
    CHECK(std::strcmp(log_level_string(LogLevel::warn), "WARN") == 0);
    CHECK(std::strcmp(log_level_string(LogLevel::error), "ERROR") == 0);
    CHECK(std::strcmp(log_level_string(static_cast<LogLevel>(200U)), "UNKNOWN") == 0);
    logger.set_minimum_level(LogLevel::trace);
    logger.log(LogLevel::trace, "no clock text");
    CHECK(capture.count == 3U);
    CHECK(capture.messages[2] == "no clock text");
    logger.set_sink(nullptr, nullptr);
    logger.log(LogLevel::error, "dropped");
    CHECK(capture.count == 3U);
    return true;
}

bool test_mock_transport()
{
    constexpr std::array<std::uint8_t, 3U> read_response{1U, 2U, 3U};
    constexpr std::array<std::uint8_t, 2U> write_request{9U, 8U};
    constexpr std::array<std::uint8_t, 4U> stream_data{0x47U, 0x01U, 0x02U, 0x03U};
    const StreamConfig stream_config{0x84U, 188U, 2U};

    MockTransport transport(kPolicy);
    transport.expect_bulk_read(kCommandInEndpoint,
                               ByteView{read_response.data(), read_response.size()});
    transport.expect_bulk_write(kCommandOutEndpoint,
                                ByteView{write_request.data(), write_request.size()});
    transport.expect_stream_start(stream_config);
    transport.expect_stream_event(ByteView{stream_data.data(), stream_data.size()},
                                  MockOutcome::short_transfer, 3U);
    transport.expect_stream_cancel();

    std::array<std::uint8_t, 8U> output{};
    auto read = transport.bulk_read(kCommandInEndpoint,
                                    MutableByteView{output.data(), output.size()}, Timeout{100U});
    CHECK(read);
    CHECK(read.value() == read_response.size());
    CHECK(std::memcmp(output.data(), read_response.data(), read_response.size()) == 0);
    auto write = transport.bulk_write(kCommandOutEndpoint,
                                      ByteView{write_request.data(), write_request.size()}, Timeout{100U});
    CHECK(write);
    CHECK(write.value() == write_request.size());
    CHECK(transport.start_stream(stream_config));
    CHECK(transport.stream_active());
    auto event = transport.wait_stream(Timeout{100U});
    CHECK(event);
    CHECK(event.value().kind == StreamEventKind::short_transfer);
    CHECK(event.value().size == 3U);
    CHECK(event.value().data[0] == 0x47U);
    CHECK(transport.cancel_stream());
    CHECK(!transport.stream_active());
    CHECK(transport.cancel_stream());
    CHECK(transport.stop_stream());
    CHECK(transport.remaining_expectations() == 0U);

    const std::array<MockOperation, 5U> expected_operations{
        MockOperation::bulk_read, MockOperation::bulk_write, MockOperation::stream_start,
        MockOperation::stream_wait, MockOperation::stream_cancel};
    CHECK(transport.operations().size() == expected_operations.size());
    for (std::size_t index = 0U; index < expected_operations.size(); ++index) {
        CHECK(transport.operations()[index] == expected_operations[index]);
    }
    return true;
}

bool test_mock_failures_and_bounds()
{
    std::array<std::uint8_t, 1U> byte{};
    constexpr std::array<std::uint8_t, 3U> short_response{0x21U, 0x22U, 0x23U};
    MockTransport short_transfer(kPolicy);
    short_transfer.expect_bulk_read(kCommandInEndpoint,
                                    ByteView{short_response.data(), short_response.size()},
                                    MockOutcome::short_transfer);
    std::array<std::uint8_t, 3U> short_output{};
    BulkReadObservation short_observation{Error::USB_IO, 99U};
    auto short_read = short_transfer.bulk_read(
        kCommandInEndpoint, MutableByteView{short_output.data(), short_output.size()}, Timeout{1U},
        &short_observation);
    CHECK(short_read);
    CHECK(short_read.value() == 2U);
    CHECK(short_observation.completion_error == Error::OK);
    CHECK(short_observation.transferred == 2U);
    CHECK(short_output[0] == 0x21U && short_output[1] == 0x22U);

    MockTransport partial_timeout(kPolicy);
    std::array<std::uint8_t, 512U> timeout_response{};
    partial_timeout.expect_bulk_read(kCommandInEndpoint,
                                     ByteView{timeout_response.data(), timeout_response.size()},
                                     MockOutcome::timeout, timeout_response.size());
    BulkReadObservation partial_timeout_observation{Error::OK, 99U};
    auto partial_timeout_read = partial_timeout.bulk_read(
        kCommandInEndpoint,
        MutableByteView{timeout_response.data(), timeout_response.size()}, Timeout{1U},
        &partial_timeout_observation);
    CHECK(partial_timeout_read && partial_timeout_read.value() == timeout_response.size());
    CHECK(partial_timeout_observation.completion_error == Error::TIMEOUT);
    CHECK(partial_timeout_observation.transferred == timeout_response.size());

    MockTransport zero_timeout(kPolicy);
    zero_timeout.expect_bulk_read(kCommandInEndpoint, ByteView{nullptr, 0U},
                                  MockOutcome::timeout);
    BulkReadObservation zero_timeout_observation{Error::OK, 99U};
    auto zero_timeout_read = zero_timeout.bulk_read(
        kCommandInEndpoint, MutableByteView{byte.data(), byte.size()}, Timeout{1U},
        &zero_timeout_observation);
    CHECK(!zero_timeout_read && zero_timeout_read.error() == Error::TIMEOUT);
    CHECK(zero_timeout_observation.completion_error == Error::TIMEOUT);
    CHECK(zero_timeout_observation.transferred == 0U);

    MockTransport invalid_transfer(kPolicy);
    invalid_transfer.expect_bulk_read(kCommandInEndpoint, ByteView{short_response.data(), 1U},
                                      MockOutcome::success, 2U);
    BulkReadObservation invalid_transfer_observation{Error::OK, 99U};
    auto invalid_transfer_read = invalid_transfer.bulk_read(
        kCommandInEndpoint, MutableByteView{byte.data(), byte.size()}, Timeout{1U},
        &invalid_transfer_observation);
    CHECK(!invalid_transfer_read && invalid_transfer_read.error() == Error::INTERNAL);
    CHECK(invalid_transfer_observation.completion_error == Error::INTERNAL);
    CHECK(invalid_transfer_observation.transferred == 0U);

    MockTransport bounds(kPolicy);
    constexpr std::size_t max_command_size = 65535U;
    std::array<std::uint8_t, max_command_size> max_command_data{};
    bounds.expect_bulk_read(kCommandInEndpoint,
                            ByteView{max_command_data.data(), max_command_data.size()});
    auto max_read = bounds.bulk_read(kCommandInEndpoint,
                                     MutableByteView{max_command_data.data(), max_command_data.size()},
                                     Timeout{1U});
    CHECK(max_read);
    CHECK(max_read.value() == max_command_size);
    auto too_large_read = bounds.bulk_read(kCommandInEndpoint,
                                           MutableByteView{byte.data(), max_command_size + 1U},
                                           Timeout{1U});
    CHECK(!too_large_read);
    CHECK(too_large_read.error() == Error::BUFFER_TOO_SMALL);
    bounds.expect_bulk_write(kCommandOutEndpoint,
                             ByteView{max_command_data.data(), max_command_data.size()});
    auto max_write = bounds.bulk_write(kCommandOutEndpoint,
                                       ByteView{max_command_data.data(), max_command_data.size()},
                                       Timeout{1U});
    CHECK(max_write);
    CHECK(max_write.value() == max_command_size);
    auto too_large_write = bounds.bulk_write(kCommandOutEndpoint,
                                             ByteView{byte.data(), max_command_size + 1U}, Timeout{1U});
    CHECK(!too_large_write);
    CHECK(too_large_write.error() == Error::BUFFER_TOO_SMALL);

    MockTransport null_pointer(kPolicy);
    auto null_read = null_pointer.bulk_read(kCommandInEndpoint,
                                            MutableByteView{nullptr, 1U}, Timeout{1U});
    CHECK(!null_read);
    CHECK(null_read.error() == Error::INVALID_ARGUMENT);
    auto null_write = null_pointer.bulk_write(kCommandOutEndpoint, ByteView{nullptr, 1U}, Timeout{1U});
    CHECK(!null_write);
    CHECK(null_write.error() == Error::INVALID_ARGUMENT);
    CHECK(null_pointer.operations().empty());

    MockTransport stream_bounds(kPolicy);
    const StreamConfig too_large_stream{0x84U, 1048576U + 1U, 1U};
    auto too_large_stream_result = stream_bounds.start_stream(too_large_stream);
    CHECK(!too_large_stream_result);
    CHECK(too_large_stream_result.error() == Error::INVALID_ARGUMENT);
    const StreamConfig wrong_endpoint{kCommandInEndpoint, 188U, 1U};
    CHECK(stream_bounds.start_stream(wrong_endpoint).error() == Error::INVALID_ARGUMENT);

    MockTransport ordering(kPolicy);
    constexpr std::array<std::uint8_t, 1U> expected{0x11U};
    ordering.expect_bulk_read(kCommandInEndpoint, ByteView{expected.data(), expected.size()});
    auto wrong_order = ordering.bulk_write(kCommandOutEndpoint,
                                           ByteView{expected.data(), expected.size()}, Timeout{1U});
    CHECK(!wrong_order);
    CHECK(wrong_order.error() == Error::PROTOCOL_ERROR);
    CHECK(ordering.remaining_expectations() == 1U);

    const MockOutcome outcomes[] = {MockOutcome::timeout, MockOutcome::disconnect,
                                    MockOutcome::protocol_error, MockOutcome::usb_io};
    const Error errors[] = {Error::TIMEOUT, Error::DISCONNECTED, Error::PROTOCOL_ERROR,
                            Error::USB_IO};
    for (std::size_t index = 0U; index < 4U; ++index) {
        MockTransport failing(kPolicy);
        failing.expect_bulk_read(kCommandInEndpoint, ByteView{nullptr, 0U}, outcomes[index]);
        auto result = failing.bulk_read(kCommandInEndpoint,
                                        MutableByteView{byte.data(), byte.size()}, Timeout{1U});
        CHECK(!result);
        CHECK(result.error() == errors[index]);
    }

    MockTransport stream_failure(kPolicy);
    const StreamConfig config{0x84U, 188U, 1U};
    stream_failure.expect_stream_start(config);
    stream_failure.expect_stream_event(ByteView{nullptr, 0U}, MockOutcome::timeout);
    stream_failure.expect_stream_stop();
    CHECK(stream_failure.start_stream(config));
    auto timed_out = stream_failure.wait_stream(Timeout{1U});
    CHECK(!timed_out);
    CHECK(timed_out.error() == Error::TIMEOUT);
    CHECK(stream_failure.stream_active());
    CHECK(stream_failure.stop_stream());
    CHECK(stream_failure.stop_stream());
    CHECK(stream_failure.remaining_expectations() == 0U);

    MockTransport disconnect_stream(kPolicy);
    const StreamConfig disconnect_config{0x84U, 188U, 1U};
    disconnect_stream.expect_stream_start(disconnect_config);
    disconnect_stream.expect_stream_event(ByteView{nullptr, 0U}, MockOutcome::disconnect);
    CHECK(disconnect_stream.start_stream(disconnect_config));
    auto disconnected = disconnect_stream.wait_stream(Timeout{1U});
    CHECK(!disconnected);
    CHECK(disconnected.error() == Error::DISCONNECTED);
    CHECK(!disconnect_stream.stream_active());
    CHECK(disconnect_stream.stop_stream());
    CHECK(disconnect_stream.remaining_expectations() == 0U);

    MockTransport not_ready(kPolicy);
    auto not_ready_event = not_ready.wait_stream(Timeout{1U});
    CHECK(!not_ready_event && not_ready_event.error() == Error::NOT_READY);
    return true;
}

bool test_mock_policy()
{
    // 共通側は製品値を固定しない。呼出側policyが上限とendpointを決める。
    constexpr MockTransportPolicy tight_policy{64U, 256U, 0x88U};
    MockTransport transport(tight_policy);

    constexpr std::array<std::uint8_t, 1U> byte{0x5aU};
    auto too_large_write = transport.bulk_write(0x02U, ByteView{byte.data(), 65U}, Timeout{1U});
    CHECK(!too_large_write);
    CHECK(too_large_write.error() == Error::BUFFER_TOO_SMALL);

    auto wrong_stream_endpoint = transport.start_stream(StreamConfig{0x84U, 188U, 1U});
    CHECK(!wrong_stream_endpoint);
    CHECK(wrong_stream_endpoint.error() == Error::INVALID_ARGUMENT);

    auto too_large_stream = transport.start_stream(StreamConfig{0x88U, 257U, 1U});
    CHECK(!too_large_stream);
    CHECK(too_large_stream.error() == Error::INVALID_ARGUMENT);

    const StreamConfig policy_stream{0x88U, 256U, 1U};
    transport.expect_stream_start(policy_stream);
    transport.expect_stream_stop();
    CHECK(transport.start_stream(policy_stream));
    CHECK(transport.stream_active());
    CHECK(transport.stop_stream());

    constexpr std::array<std::uint8_t, 2U> read_response{0xaaU, 0xbbU};
    transport.expect_bulk_read(0x88U, ByteView{read_response.data(), read_response.size()});
    std::array<std::uint8_t, 2U> output{};
    auto read = transport.bulk_read(0x88U, MutableByteView{output.data(), output.size()}, Timeout{1U});
    CHECK(read && read.value() == 2U && output[0] == 0xaaU && output[1] == 0xbbU);

    // 下位方向ビットの無いendpointは読み出しで拒否し、上位ビット付きは書き込みで拒否する。
    CHECK(transport.bulk_read(0x02U, MutableByteView{output.data(), output.size()}, Timeout{1U})
              .error() == Error::INVALID_ARGUMENT);
    CHECK(transport.bulk_write(0x88U, ByteView{byte.data(), byte.size()}, Timeout{1U})
              .error() == Error::INVALID_ARGUMENT);
    CHECK(transport.bulk_read(0x00U, MutableByteView{output.data(), output.size()}, Timeout{1U})
              .error() == Error::INVALID_ARGUMENT);
    CHECK(transport.bulk_write(0x00U, ByteView{byte.data(), byte.size()}, Timeout{1U})
              .error() == Error::INVALID_ARGUMENT);
    return true;
}

}  // namespace

int main()
{
    const struct {
        const char* name;
        bool (*run)();
    } tests[] = {
        {"error_contract", test_error_contract},
        {"logging", test_logging},
        {"mock_transport", test_mock_transport},
        {"mock_failures_and_bounds", test_mock_failures_and_bounds},
        {"mock_policy", test_mock_policy},
    };

    int failures = 0;
    for (const auto& test : tests) {
        if (test.run()) {
            std::printf("ok %s\n", test.name);
        } else {
            ++failures;
            std::printf("FAILED %s\n", test.name);
        }
    }

    if (failures != 0) {
        std::fprintf(stderr, "%d foundation test(s) failed\n", failures);
        return 1;
    }
    std::printf("foundation tests passed\n");
    return 0;
}
