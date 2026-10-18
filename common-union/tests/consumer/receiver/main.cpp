#include <dtv/userland/detail/receiver_engine.h>

#include <array>

namespace {

using namespace dtv::userland;
using namespace dtv::userland::detail;

class Backend final : public ReceiverBackend {
public:
    bool supports(ReceiverId, ReceiverSystem system) const noexcept override
    {
        return system == ReceiverSystem::terrestrial;
    }
    bool selects_satellite_stream_before_tune() const noexcept override { return false; }
    std::uint32_t lock_settle_ms(ReceiverId, ReceiverSystem) const noexcept override
    {
        return 0U;
    }
    Result<void> open(ReceiverId) noexcept override { return Result<void>::success(); }
    Result<void> tune(ReceiverId, ReceiverSystem, std::uint32_t, std::uint32_t) noexcept override
    {
        return Result<void>::success();
    }
    Result<bool> is_locked(ReceiverId, ReceiverSystem) noexcept override
    {
        return Result<bool>::success(true);
    }
    Result<void> select_satellite_slot(ReceiverId, std::uint8_t, std::uint32_t) noexcept override
    {
        return Result<void>::failure(Error::UNSUPPORTED);
    }
    Result<void> select_satellite_tsid(ReceiverId, std::uint16_t, std::uint32_t) noexcept override
    {
        return Result<void>::failure(Error::UNSUPPORTED);
    }
    Result<void> close(ReceiverId) noexcept override { return Result<void>::success(); }
    Result<void> begin_tune_power(ReceiverId, ReceiverSystem, std::uint8_t) noexcept override
    {
        return Result<void>::success();
    }
    Result<void> commit_tune_power(ReceiverId) noexcept override
    {
        return Result<void>::success();
    }
    Result<void> rollback_tune_power(ReceiverId) noexcept override
    {
        return Result<void>::success();
    }
    void mark_disconnected(ReceiverId) noexcept override {}
    Result<void> shutdown() noexcept override { return Result<void>::success(); }
    Result<void> start_capture(ReceiverId, ReceiverSystem) noexcept override
    {
        return Result<void>::success();
    }
    Result<void> stop_capture(ReceiverId, ReceiverSystem) noexcept override
    {
        return Result<void>::success();
    }
};

class Tokens final : public ReceiverTokenSource {
public:
    Result<ReceiverToken> generate() noexcept override
    {
        return Result<ReceiverToken>::success(ReceiverToken{1U});
    }
};

class Clock final : public ReceiverClock {
public:
    std::uint64_t monotonic_ms() noexcept override { return 0U; }
    void sleep_ms(std::uint32_t) noexcept override {}
};

}  // namespace

int main()
{
    Backend backend;
    Tokens tokens;
    Clock clock;
    const std::array<ReceiverId, 1> receivers{513U};
    ReceiverEngine engine(backend, receivers.data(), receivers.size(), tokens, clock);
    const auto lease = engine.acquire(1U, 513U);
    TuneRequest request{};
    request.lease_id = lease ? lease.value().lease_id : 0U;
    request.frequency_khz = 557000U;
    request.timeout_ms = 1000U;
    return lease && engine.tune(1U, request) && engine.release(1U, request.lease_id) ? 0 : 1;
}
