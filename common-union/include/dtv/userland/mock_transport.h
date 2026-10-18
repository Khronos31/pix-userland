// SPDX-License-Identifier: MIT
#ifndef DTV_USERLAND_MOCK_TRANSPORT_H
#define DTV_USERLAND_MOCK_TRANSPORT_H

#include "dtv/userland/transport.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace dtv::userland {

enum class MockOperation : std::uint8_t {
    bulk_read,
    bulk_write,
    stream_start,
    stream_wait,
    stream_cancel,
    stream_stop,
};

enum class MockOutcome : std::uint8_t {
    success,
    short_transfer,
    timeout,
    disconnect,
    protocol_error,
    usb_io,
};

// 呼出側が装置固有の上限・endpointを必ず指定する。共通側に製品値を既定値として持たない。
struct MockTransportPolicy final {
    std::size_t max_command_transfer;
    std::size_t max_stream_transfer;
    std::uint8_t stream_endpoint;
};

class MockTransport : public Transport {
public:
    explicit MockTransport(MockTransportPolicy policy) noexcept;
    ~MockTransport() noexcept override = default;

    void expect_bulk_read(std::uint8_t endpoint, ByteView response,
                          MockOutcome outcome = MockOutcome::success,
                          std::size_t transferred = kUsePayloadSize);
    void expect_bulk_write(std::uint8_t endpoint, ByteView request,
                           MockOutcome outcome = MockOutcome::success,
                           std::size_t transferred = kUsePayloadSize);
    void expect_stream_start(const StreamConfig& config,
                             MockOutcome outcome = MockOutcome::success);
    void expect_stream_event(ByteView response,
                             MockOutcome outcome = MockOutcome::success,
                             std::size_t transferred = kUsePayloadSize);
    void expect_stream_cancel(MockOutcome outcome = MockOutcome::success);
    void expect_stream_stop(MockOutcome outcome = MockOutcome::success);

    Result<std::size_t> bulk_read(std::uint8_t endpoint, MutableByteView output,
                                  Timeout timeout,
                                  BulkReadObservation* observation = nullptr) noexcept override;
    Result<std::size_t> bulk_write(std::uint8_t endpoint, ByteView input,
                                   Timeout timeout) noexcept override;
    Result<void> start_stream(const StreamConfig& config) noexcept override;
    Result<StreamEvent> wait_stream(Timeout timeout) noexcept override;
    Result<void> cancel_stream() noexcept override;
    Result<void> stop_stream() noexcept override;
    bool stream_active() const noexcept override { return stream_active_; }

    const std::vector<MockOperation>& operations() const noexcept { return operations_; }
    const std::vector<Timeout>& timeouts() const noexcept { return timeouts_; }
    const std::vector<std::size_t>& bulk_read_sizes() const noexcept { return bulk_read_sizes_; }
    std::size_t remaining_expectations() const noexcept;

    static constexpr std::size_t kUsePayloadSize = static_cast<std::size_t>(-1);

private:
    struct Expectation final {
        MockOperation operation;
        MockOutcome outcome;
        std::uint8_t endpoint;
        std::vector<std::uint8_t> payload;
        std::size_t transferred;
        StreamConfig stream_config;
    };

    void add_expectation(MockOperation operation, std::uint8_t endpoint,
                         MockOutcome outcome, ByteView payload,
                         std::size_t transferred, const StreamConfig& config);
    Expectation* consume(MockOperation operation) noexcept;
    static Error outcome_error(MockOutcome outcome) noexcept;
    static std::size_t default_transfer(MockOutcome outcome, std::size_t payload_size) noexcept;
    bool valid_bulk_size(std::size_t size) const noexcept;
    bool valid_stream_config(const StreamConfig& config) const noexcept;

    MockTransportPolicy policy_;
    std::vector<Expectation> expectations_;
    std::vector<MockOperation> operations_;
    std::vector<Timeout> timeouts_;
    std::vector<std::size_t> bulk_read_sizes_;
    std::vector<std::uint8_t> last_stream_data_;
    StreamConfig active_stream_config_{};
    std::size_t next_expectation_ = 0U;
    bool stream_active_ = false;
};

}  // namespace dtv::userland

#endif  // DTV_USERLAND_MOCK_TRANSPORT_H
