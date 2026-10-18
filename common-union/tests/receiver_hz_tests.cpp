// SPDX-License-Identifier: MIT
//
// R3 strict Hz tests for the same ReceiverEngine admission/ticket/helper.
// They are additive: the legacy direct entrance and the R2 strict kHz
// admission keep their behavior (receiver_engine_tests.cpp and
// receiver_r2_tests.cpp).  Every backend here is a bounded fake; no product
// frontend arithmetic is used.
#include "dtv/userland/detail/receiver_engine.h"

#include <cstdint>
#include <cstdio>
#include <functional>
#include <limits>
#include <string>
#include <vector>

namespace {

using namespace dtv::userland;
using namespace dtv::userland::detail;

#define HZ_CHECK(condition)                                                           \
    do {                                                                               \
        if (!(condition)) {                                                            \
            std::fprintf(stderr, "receiver Hz check failed at line %d: %s\n",        \
                         __LINE__, #condition);                                        \
            return false;                                                              \
        }                                                                              \
    } while (false)

constexpr ReceiverId kTerrestrial = 17U;
constexpr ReceiverId kSatellite = 513U;
constexpr std::uint64_t kT13 = 473142857U;

class Clock final : public ReceiverClock {
public:
    std::uint64_t monotonic_ms() noexcept override
    {
        ++calls;
        return now;
    }
    void sleep_ms(std::uint32_t milliseconds) noexcept override { now += milliseconds; }
    std::uint64_t now = 1000U;
    std::uint64_t calls = 0U;
};

class Tokens final : public ReceiverTokenSource {
public:
    Result<ReceiverToken> generate() noexcept override
    {
        ReceiverToken token{};
        token[0U] = ++next;
        return Result<ReceiverToken>::success(token);
    }
    std::uint8_t next = 0U;
};

std::string num(std::uint64_t value) { return std::to_string(value); }

// Legacy fake: implements only the established callbacks.  It deliberately
// does not override program_hz, so the default strict callback applies.
class LegacyBackend : public ReceiverBackend {
public:
    explicit LegacyBackend(Clock& clock) noexcept : clock_(clock) {}

    bool supports(ReceiverId receiver, ReceiverSystem system) const noexcept override
    {
        return receiver == kSatellite ? system == ReceiverSystem::satellite
                                      : system == ReceiverSystem::terrestrial;
    }
    bool selects_satellite_stream_before_tune() const noexcept override { return false; }
    std::uint32_t lock_settle_ms(ReceiverId, ReceiverSystem) const noexcept override
    {
        return 0U;
    }
    Result<void> open(ReceiverId receiver) noexcept override
    {
        calls.push_back("open " + num(receiver));
        return Result<void>::success();
    }
    Result<void> tune(ReceiverId receiver, ReceiverSystem, std::uint32_t frequency_khz,
                      std::uint32_t) noexcept override
    {
        calls.push_back("tune_khz " + num(receiver) + " " + num(frequency_khz));
        return Result<void>::success();
    }
    Result<bool> is_locked(ReceiverId receiver, ReceiverSystem) noexcept override
    {
        calls.push_back("lock " + num(receiver));
        if (lock_advance_ms != 0U) clock_.now += lock_advance_ms;
        return lock_error == Error::OK ? Result<bool>::success(locked)
                                       : Result<bool>::failure(lock_error);
    }
    Result<void> select_satellite_slot(ReceiverId receiver, std::uint8_t slot,
                                       std::uint32_t) noexcept override
    {
        calls.push_back("slot " + num(receiver) + " " + num(slot));
        return Result<void>::success();
    }
    Result<void> select_satellite_tsid(ReceiverId receiver, std::uint16_t tsid,
                                       std::uint32_t) noexcept override
    {
        calls.push_back("tsid " + num(receiver) + " " + num(tsid));
        return Result<void>::success();
    }
    Result<void> close(ReceiverId receiver) noexcept override
    {
        calls.push_back("close " + num(receiver));
        return Result<void>::success();
    }
    Result<void> begin_tune_power(ReceiverId receiver, ReceiverSystem,
                                  std::uint8_t) noexcept override
    {
        calls.push_back("power " + num(receiver));
        pending_power = true;
        return Result<void>::success();
    }
    Result<void> commit_tune_power(ReceiverId receiver) noexcept override
    {
        calls.push_back("commit " + num(receiver));
        pending_power = false;
        if (commit_advance_ms != 0U) clock_.now += commit_advance_ms;
        return Result<void>::success();
    }
    Result<void> rollback_tune_power(ReceiverId receiver) noexcept override
    {
        calls.push_back("rollback " + num(receiver));
        return pending_power ? Result<void>::success()
                             : Result<void>::failure(Error::INVALID_ARGUMENT);
    }
    void mark_disconnected(ReceiverId receiver) noexcept override
    {
        calls.push_back("lost " + num(receiver));
    }
    Result<void> shutdown() noexcept override { return Result<void>::success(); }
    Result<void> start_capture(ReceiverId, ReceiverSystem) noexcept override
    {
        return Result<void>::success();
    }
    Result<void> stop_capture(ReceiverId, ReceiverSystem) noexcept override
    {
        return Result<void>::success();
    }

    int count(const std::string& prefix) const
    {
        int total = 0;
        for (const std::string& call : calls) {
            if (call.rfind(prefix, 0U) == 0U) ++total;
        }
        return total;
    }

    std::vector<std::string> calls;
    bool locked = true;
    Error lock_error = Error::OK;
    std::uint64_t lock_advance_ms = 0U;
    std::uint64_t commit_advance_ms = 0U;
    bool pending_power = false;

protected:
    Clock& clock_;
};

// Strict fake: also implements program_hz with a scripted outcome.
class HzBackend final : public LegacyBackend {
public:
    explicit HzBackend(Clock& clock) noexcept : LegacyBackend(clock) {}

    Result<TuneProgramming> program_hz(ReceiverId receiver, ReceiverSystem,
                                       std::uint64_t requested_hz,
                                       std::uint32_t timeout_ms) noexcept override
    {
        calls.push_back("tune_hz " + num(receiver) + " " + num(requested_hz));
        last_hz = requested_hz;
        last_timeout_ms = timeout_ms;
        if (program_advance_ms != 0U) clock_.now += program_advance_ms;
        if (during_program) during_program();
        if (program_error != Error::OK) return Result<TuneProgramming>::failure(program_error);
        return Result<TuneProgramming>::success(programming);
    }

    TuneProgramming programming{};
    Error program_error = Error::OK;
    std::uint64_t program_advance_ms = 0U;
    std::uint64_t last_hz = 0U;
    std::uint32_t last_timeout_ms = 0U;
    std::function<void()> during_program;
};

struct Fixture final {
    Clock clock;
    Tokens tokens;
    std::array<ReceiverId, 2> ids{{kTerrestrial, kSatellite}};
};

bool configure(ReceiverEngine& engine, bool native_hz,
               TuneReadiness readiness = TuneReadiness::lock_poll,
               bool lock_measurement = true)
{
    std::array<ReceiverCapability, 2> capabilities{};
    capabilities[0].receiver = kTerrestrial;
    capabilities[0].supports_terrestrial = true;
    capabilities[1].receiver = kSatellite;
    capabilities[1].supports_satellite = true;
    for (ReceiverCapability& capability : capabilities) {
        capability.native_hz = native_hz;
        capability.readiness = readiness;
        capability.lock_measurement = lock_measurement;
    }
    return static_cast<bool>(
        engine.configure_strict_capabilities(capabilities.data(), capabilities.size()));
}

HzTuneRequest hz_request(std::uint64_t lease_id, std::uint64_t hz,
                         Error validation = Error::OK)
{
    HzTuneRequest request{};
    request.lease_id = lease_id;
    request.prepared_receiver = kTerrestrial;
    request.system = ReceiverSystem::terrestrial;
    request.requested_hz = hz;
    request.timeout_ms = 1000U;
    request.validation = validation;
    return request;
}

bool failed(const Result<AdmittedTune>& result, Error error)
{
    return !result && result.error() == error;
}

bool failed(const Result<void>& result, Error error)
{
    return !result && result.error() == error;
}

TuneMetadata metadata(const ReceiverEngine& engine, std::size_t slot)
{
    return engine.status().value().tune_metadata[slot];
}

// Exact u64 requested Hz survives admission, execution, current and history
// without truncation; programmed Hz and lock evidence travel separately.
bool test_exact_hz_survives()
{
    for (const std::uint64_t hz : {kT13, kMaxRequestedHz, std::uint64_t{1U}}) {
        Fixture f;
        HzBackend backend(f.clock);
        ReceiverEngine engine(backend, f.ids.data(), f.ids.size(), f.tokens, f.clock);
        HZ_CHECK(configure(engine, true));
        const auto lease = engine.acquire(1U, kTerrestrial);
        HZ_CHECK(lease);
        backend.programming.programmed_hz = wire::Optional<std::uint64_t>{true, 473143000U};
        const auto admitted =
            engine.try_admit_tune(1U, hz_request(lease.value().lease_id, hz), f.clock.now);
        HZ_CHECK(admitted);
        HZ_CHECK(admitted.value().request.requested_hz == hz);
        HZ_CHECK(admitted.value().request.entrance == TuneEntrance::strict_hz);
        HZ_CHECK(engine.execute_admitted_tune(admitted.value()));
        HZ_CHECK(backend.last_hz == hz);
        HZ_CHECK(backend.count("tune_khz") == 0);
        const TuneMetadata m = metadata(engine, 0U);
        HZ_CHECK(m.current_valid && m.last_successful_valid);
        HZ_CHECK(m.current.requested_hz == hz && m.last_successful.requested_hz == hz);
        HZ_CHECK(m.current.entrance == TuneEntrance::strict_hz);
        HZ_CHECK(m.current.programmed_hz.present &&
                 m.current.programmed_hz.value == 473143000U);
        HZ_CHECK(m.current.verification == wire::Verification::lock);
        HZ_CHECK(m.current.lock.state == wire::MeasureState::valid && m.current.lock.value);
        // The derived kHz projection is floor(Hz/1000) or 0 when too wide.
        const std::uint64_t khz = hz / 1000U;
        HZ_CHECK(m.current.frequency_khz ==
                 (khz <= std::numeric_limits<std::uint32_t>::max()
                      ? static_cast<std::uint32_t>(khz)
                      : 0U));
    }
    return true;
}

// The legacy kHz entrance still calls the original kHz callback with the
// original value; its canonical Hz is that value times 1000.
bool test_legacy_entrance_unchanged()
{
    Fixture f;
    HzBackend backend(f.clock);
    ReceiverEngine engine(backend, f.ids.data(), f.ids.size(), f.tokens, f.clock);
    HZ_CHECK(configure(engine, true));
    const auto lease = engine.acquire(1U, kTerrestrial);
    HZ_CHECK(lease);
    TuneRequest legacy{};
    legacy.lease_id = lease.value().lease_id;
    legacy.system = ReceiverSystem::terrestrial;
    legacy.frequency_khz = 557142U;
    legacy.timeout_ms = 1000U;
    backend.calls.clear();
    const auto admitted = engine.try_admit_tune(1U, legacy, f.clock.now);
    HZ_CHECK(admitted);
    HZ_CHECK(admitted.value().request.entrance == TuneEntrance::legacy_khz);
    HZ_CHECK(admitted.value().request.requested_hz == 557142000U);
    HZ_CHECK(engine.execute_admitted_tune(admitted.value()));
    HZ_CHECK(backend.count("tune_khz 17 557142") == 1 && backend.count("tune_hz") == 0);
    const TuneMetadata m = metadata(engine, 0U);
    HZ_CHECK(m.current.entrance == TuneEntrance::legacy_khz);
    HZ_CHECK(m.current.requested_hz == 557142000U && m.current.frequency_khz == 557142U);
    HZ_CHECK(!m.current.programmed_hz.present);
    // The legacy direct entrance also keeps its kHz callback.
    backend.calls.clear();
    HZ_CHECK(engine.tune(1U, legacy));
    HZ_CHECK(backend.count("tune_khz 17 557142") == 1 && backend.count("tune_hz") == 0);
    return true;
}

// A backend without a strict callback reports UNSUPPORTED from the default;
// nothing falls back to the kHz callback and no state is published.
bool test_default_strict_callback_unsupported()
{
    Fixture f;
    LegacyBackend backend(f.clock);
    ReceiverEngine engine(backend, f.ids.data(), f.ids.size(), f.tokens, f.clock);
    HZ_CHECK(configure(engine, true));
    const auto lease = engine.acquire(1U, kTerrestrial);
    HZ_CHECK(lease);
    backend.calls.clear();
    const auto admitted =
        engine.try_admit_tune(1U, hz_request(lease.value().lease_id, 557142000U), f.clock.now);
    HZ_CHECK(admitted);
    HZ_CHECK(failed(engine.execute_admitted_tune(admitted.value()), Error::UNSUPPORTED));
    HZ_CHECK(backend.count("tune_khz") == 0 && backend.count("lock") == 0);
    HZ_CHECK(backend.count("power") == 1 && backend.count("rollback") == 1 &&
             backend.count("commit") == 0);
    const TuneMetadata m = metadata(engine, 0U);
    HZ_CHECK(!m.current_valid && !m.last_successful_valid && m.revision == 1U);
    return true;
}

// Admission precedence and callback-free rejection without revision change.
bool test_admission_precedence()
{
    Fixture f;
    HzBackend backend(f.clock);
    ReceiverEngine engine(backend, f.ids.data(), f.ids.size(), f.tokens, f.clock);
    // Not configured: NOT_READY first.
    HZ_CHECK(failed(engine.try_admit_tune(1U, hz_request(1U, kT13), 0U), Error::NOT_READY));
    HZ_CHECK(configure(engine, true));
    // Unknown lease.
    HZ_CHECK(failed(engine.try_admit_tune(1U, hz_request(99U, kT13), 0U), Error::NOT_FOUND));
    const auto lease = engine.acquire(1U, kTerrestrial);
    HZ_CHECK(lease);
    const std::uint64_t id = lease.value().lease_id;
    backend.calls.clear();
    const std::uint64_t clock_calls = f.clock.calls;
    // System support before Hz checks and before the prepared decision.
    HzTuneRequest satellite = hz_request(id, 0U, Error::UNSUPPORTED);
    satellite.system = ReceiverSystem::satellite;
    HZ_CHECK(failed(engine.try_admit_tune(1U, satellite, 0U), Error::INVALID_ARGUMENT));
    HzTuneRequest unknown = hz_request(id, kT13);
    unknown.system = static_cast<ReceiverSystem>(9U);
    HZ_CHECK(failed(engine.try_admit_tune(1U, unknown, 0U), Error::INVALID_ARGUMENT));
    // Common scalar before the prepared profile decision.
    HZ_CHECK(failed(engine.try_admit_tune(1U, hz_request(id, 0U, Error::UNSUPPORTED), 0U),
                    Error::INVALID_ARGUMENT));
    HZ_CHECK(failed(engine.try_admit_tune(1U, hz_request(id, kMaxRequestedHz + 1U), 0U),
                    Error::INVALID_ARGUMENT));
    HZ_CHECK(failed(engine.try_admit_tune(
                        1U, hz_request(id, std::numeric_limits<std::uint64_t>::max()), 0U),
                    Error::INVALID_ARGUMENT));
    // An outcome prepared for another receiver's profile is not consumed.
    HzTuneRequest elsewhere = hz_request(id, kT13);
    elsewhere.prepared_receiver = kSatellite;
    HZ_CHECK(failed(engine.try_admit_tune(1U, elsewhere, 0U), Error::INVALID_ARGUMENT));
    // The prepared profile outcome is returned at the validation position.
    HZ_CHECK(failed(engine.try_admit_tune(1U, hz_request(id, kT13, Error::UNSUPPORTED), 0U),
                    Error::UNSUPPORTED));
    HZ_CHECK(failed(engine.try_admit_tune(1U, hz_request(id, kT13, Error::INVALID_ARGUMENT), 0U),
                    Error::INVALID_ARGUMENT));
    // No backend or clock callback, no revision spent, nothing published.
    HZ_CHECK(backend.calls.empty() && f.clock.calls == clock_calls);
    TuneMetadata m = metadata(engine, 0U);
    HZ_CHECK(m.revision == 0U && !m.current_valid);
    HZ_CHECK(engine.status().value().operations[0] == ReceiverTuneOperation::idle);

    // A receiver without native Hz programming: strict Hz is UNSUPPORTED after
    // the support check and before scalar/validation; legacy kHz still admits.
    Fixture g;
    HzBackend other(g.clock);
    ReceiverEngine legacy_only(other, g.ids.data(), g.ids.size(), g.tokens, g.clock);
    HZ_CHECK(configure(legacy_only, false));
    const auto second = legacy_only.acquire(2U, kTerrestrial);
    HZ_CHECK(second);
    HZ_CHECK(failed(legacy_only.try_admit_tune(2U, hz_request(second.value().lease_id, 0U), 0U),
                    Error::UNSUPPORTED));
    HZ_CHECK(metadata(legacy_only, 0U).revision == 0U);
    TuneRequest khz{};
    khz.lease_id = second.value().lease_id;
    khz.frequency_khz = 557142U;
    khz.timeout_ms = 1000U;
    HZ_CHECK(legacy_only.try_admit_tune(2U, khz, 0U));
    return true;
}

// programmed_hz present / absent / divergent and lock evidence timestamps.
bool test_programmed_and_lock_evidence()
{
    struct Case final {
        bool present;
        std::uint64_t programmed;
    };
    for (const Case c : {Case{false, 0U}, Case{true, kT13}, Case{true, 473143000U}}) {
        Fixture f;
        HzBackend backend(f.clock);
        ReceiverEngine engine(backend, f.ids.data(), f.ids.size(), f.tokens, f.clock);
        HZ_CHECK(configure(engine, true));
        const auto lease = engine.acquire(1U, kTerrestrial);
        HZ_CHECK(lease);
        backend.programming.programmed_hz = wire::Optional<std::uint64_t>{c.present, c.programmed};
        f.clock.now = 5000U;
        const auto admitted = engine.try_admit_tune(1U, hz_request(lease.value().lease_id, kT13),
                                                    f.clock.now);
        HZ_CHECK(admitted);
        backend.program_advance_ms = 7U;
        HZ_CHECK(engine.execute_admitted_tune(admitted.value()));
        const TuneMetadata m = metadata(engine, 0U);
        HZ_CHECK(m.current.requested_hz == kT13);
        HZ_CHECK(m.current.programmed_hz.present == c.present);
        HZ_CHECK(!c.present || m.current.programmed_hz.value == c.programmed);
        // The successful poll sampled the engine clock at 5007 ms.
        HZ_CHECK(m.current.lock.state == wire::MeasureState::valid && m.current.lock.value &&
                 m.current.lock.sampled_at_ns == 5007000000ULL);
    }
    // A clock value whose nanoseconds do not fit is unknown, never valid at 0.
    Fixture f;
    HzBackend backend(f.clock);
    ReceiverEngine engine(backend, f.ids.data(), f.ids.size(), f.tokens, f.clock);
    HZ_CHECK(configure(engine, true));
    const auto lease = engine.acquire(1U, kTerrestrial);
    HZ_CHECK(lease);
    f.clock.now = std::numeric_limits<std::uint64_t>::max() / 1000000U + 1U;
    const auto admitted =
        engine.try_admit_tune(1U, hz_request(lease.value().lease_id, kT13), f.clock.now);
    HZ_CHECK(admitted && engine.execute_admitted_tune(admitted.value()));
    const TuneMetadata m = metadata(engine, 0U);
    HZ_CHECK(m.current_valid && m.current.lock.state == wire::MeasureState::unknown &&
             m.current.lock.sampled_at_ns == 0U && !m.current.lock.value);
    return true;
}

bool test_readiness_policies()
{
    // terminal_lock: no is_locked call; valid true succeeds, valid false is a
    // timeout and unknown is a backend contract failure.  Nothing is faked.
    {
        Fixture f;
        HzBackend backend(f.clock);
        ReceiverEngine engine(backend, f.ids.data(), f.ids.size(), f.tokens, f.clock);
        HZ_CHECK(configure(engine, true, TuneReadiness::terminal_lock));
        const auto lease = engine.acquire(1U, kTerrestrial);
        HZ_CHECK(lease);
        const std::uint64_t id = lease.value().lease_id;
        backend.programming.lock =
            wire::Measurement<bool>{wire::MeasureState::valid, wire::UnknownReason::not_sampled,
                                    true, 42U};
        auto admitted = engine.try_admit_tune(1U, hz_request(id, kT13), f.clock.now);
        HZ_CHECK(admitted && engine.execute_admitted_tune(admitted.value()));
        HZ_CHECK(backend.count("lock") == 0);
        TuneMetadata m = metadata(engine, 0U);
        HZ_CHECK(m.current.verification == wire::Verification::lock &&
                 m.current.lock.state == wire::MeasureState::valid &&
                 m.current.lock.sampled_at_ns == 42U);

        backend.programming.lock.value = false;
        admitted = engine.try_admit_tune(1U, hz_request(id, kT13 + 1U), f.clock.now);
        HZ_CHECK(admitted);
        HZ_CHECK(failed(engine.execute_admitted_tune(admitted.value()), Error::TIMEOUT));
        m = metadata(engine, 0U);
        HZ_CHECK(!m.current_valid && m.last_successful.requested_hz == kT13);

        backend.programming.lock =
            wire::Measurement<bool>{wire::MeasureState::unknown, wire::UnknownReason::not_sampled,
                                    false, 0U};
        admitted = engine.try_admit_tune(1U, hz_request(id, kT13 + 2U), f.clock.now);
        HZ_CHECK(admitted);
        HZ_CHECK(failed(engine.execute_admitted_tune(admitted.value()), Error::PROTOCOL_ERROR));
        HZ_CHECK(!metadata(engine, 0U).current_valid && backend.count("lock") == 0);
    }
    // commands: completion only, never a lock claim and no is_locked call.
    for (const bool measured : {true, false}) {
        Fixture f;
        HzBackend backend(f.clock);
        ReceiverEngine engine(backend, f.ids.data(), f.ids.size(), f.tokens, f.clock);
        HZ_CHECK(configure(engine, true, TuneReadiness::commands, measured));
        const auto lease = engine.acquire(1U, kTerrestrial);
        HZ_CHECK(lease);
        // Even a backend-reported lock is not promoted under commands.
        backend.programming.lock.state = wire::MeasureState::valid;
        backend.programming.lock.value = true;
        const auto admitted =
            engine.try_admit_tune(1U, hz_request(lease.value().lease_id, kT13), f.clock.now);
        HZ_CHECK(admitted && engine.execute_admitted_tune(admitted.value()));
        HZ_CHECK(backend.count("lock") == 0);
        const TuneMetadata m = metadata(engine, 0U);
        HZ_CHECK(m.current.verification == wire::Verification::commands);
        HZ_CHECK(m.current.lock.state ==
                 (measured ? wire::MeasureState::unknown : wire::MeasureState::unsupported));
        HZ_CHECK(!m.current.lock.value);
    }
    // ts_sync: requires actual synchronized-TS evidence from the invocation.
    {
        Fixture f;
        HzBackend backend(f.clock);
        ReceiverEngine engine(backend, f.ids.data(), f.ids.size(), f.tokens, f.clock);
        HZ_CHECK(configure(engine, true, TuneReadiness::ts_sync));
        const auto lease = engine.acquire(1U, kTerrestrial);
        HZ_CHECK(lease);
        const std::uint64_t id = lease.value().lease_id;
        auto admitted = engine.try_admit_tune(1U, hz_request(id, kT13), f.clock.now);
        HZ_CHECK(admitted);
        HZ_CHECK(failed(engine.execute_admitted_tune(admitted.value()), Error::TIMEOUT));
        HZ_CHECK(!metadata(engine, 0U).current_valid);
        backend.programming.ts_synchronized = true;
        admitted = engine.try_admit_tune(1U, hz_request(id, kT13), f.clock.now);
        HZ_CHECK(admitted && engine.execute_admitted_tune(admitted.value()));
        const TuneMetadata m = metadata(engine, 0U);
        HZ_CHECK(m.current_valid && m.current.verification == wire::Verification::ts_sync);
        HZ_CHECK(backend.count("lock") == 0);
    }
    // lock_poll: a poll error or false result never becomes lock evidence.
    {
        Fixture f;
        HzBackend backend(f.clock);
        ReceiverEngine engine(backend, f.ids.data(), f.ids.size(), f.tokens, f.clock);
        HZ_CHECK(configure(engine, true));
        const auto lease = engine.acquire(1U, kTerrestrial);
        HZ_CHECK(lease);
        backend.lock_error = Error::USB_IO;
        auto admitted =
            engine.try_admit_tune(1U, hz_request(lease.value().lease_id, kT13), f.clock.now);
        HZ_CHECK(admitted);
        HZ_CHECK(failed(engine.execute_admitted_tune(admitted.value()), Error::USB_IO));
        HZ_CHECK(!metadata(engine, 0U).current_valid &&
                 !metadata(engine, 0U).last_successful_valid);
    }
    return true;
}

// A programming error publishes nothing and rolls back the pending power.
bool test_programming_error()
{
    Fixture f;
    HzBackend backend(f.clock);
    ReceiverEngine engine(backend, f.ids.data(), f.ids.size(), f.tokens, f.clock);
    HZ_CHECK(configure(engine, true));
    const auto lease = engine.acquire(1U, kTerrestrial);
    HZ_CHECK(lease);
    backend.program_error = Error::INVALID_ARGUMENT;
    backend.calls.clear();
    const auto admitted =
        engine.try_admit_tune(1U, hz_request(lease.value().lease_id, kT13), f.clock.now);
    HZ_CHECK(admitted);
    HZ_CHECK(failed(engine.execute_admitted_tune(admitted.value()), Error::INVALID_ARGUMENT));
    HZ_CHECK(backend.count("lock") == 0 && backend.count("rollback") == 1 &&
             backend.count("commit") == 0);
    const TuneMetadata m = metadata(engine, 0U);
    HZ_CHECK(!m.current_valid && !m.last_successful_valid);
    HZ_CHECK(engine.status().value().states[0] != ReceiverState::error);
    return true;
}

// The strict callback receives the remaining admitted budget, and a callback
// that returns after the deadline cannot populate current state.
bool test_late_and_cancelled_programming()
{
    {
        Fixture f;
        HzBackend backend(f.clock);
        ReceiverEngine engine(backend, f.ids.data(), f.ids.size(), f.tokens, f.clock);
        HZ_CHECK(configure(engine, true));
        const auto lease = engine.acquire(1U, kTerrestrial);
        HZ_CHECK(lease);
        const std::uint64_t id = lease.value().lease_id;
        auto admitted = engine.try_admit_tune(1U, hz_request(id, kT13), f.clock.now);
        HZ_CHECK(admitted && engine.execute_admitted_tune(admitted.value()));
        // Next request accepted at 1000; queue wait consumed 250 ms.
        const std::uint64_t accepted = f.clock.now;
        admitted = engine.try_admit_tune(1U, hz_request(id, 557142857U), accepted);
        HZ_CHECK(admitted);
        f.clock.now = accepted + 250U;
        backend.program_advance_ms = 900U;
        backend.programming.programmed_hz = wire::Optional<std::uint64_t>{true, 557143000U};
        backend.calls.clear();
        HZ_CHECK(failed(engine.execute_admitted_tune(admitted.value()), Error::TIMEOUT));
        HZ_CHECK(backend.last_timeout_ms == 750U);
        HZ_CHECK(backend.count("lock") == 0 && backend.count("commit") == 0 &&
                 backend.count("rollback") == 1);
        const TuneMetadata m = metadata(engine, 0U);
        HZ_CHECK(!m.current_valid);
        HZ_CHECK(m.last_successful_valid && m.last_successful.requested_hz == kT13 &&
                 m.last_successful_revision == 1U && m.revision == 2U);
    }
    {
        // Owner revocation while the strict callback runs.
        Fixture f;
        HzBackend backend(f.clock);
        ReceiverEngine engine(backend, f.ids.data(), f.ids.size(), f.tokens, f.clock);
        HZ_CHECK(configure(engine, true));
        const auto lease = engine.acquire(1U, kTerrestrial);
        HZ_CHECK(lease);
        const std::uint64_t id = lease.value().lease_id;
        const auto admitted = engine.try_admit_tune(1U, hz_request(id, kT13), f.clock.now);
        HZ_CHECK(admitted);
        backend.during_program = [&]() { (void)engine.revoke_tune_ticket(1U, id); };
        HZ_CHECK(failed(engine.execute_admitted_tune(admitted.value()), Error::NOT_FOUND));
        HZ_CHECK(backend.count("lock") == 0 && backend.count("commit") == 0);
        HZ_CHECK(!metadata(engine, 0U).current_valid &&
                 !metadata(engine, 0U).last_successful_valid);
    }
    return true;
}

// A deadline found at the final critical section after a consumed commit
// quarantines; there is no rollback of the consumed transaction and the last
// success stays distinct from the invalid current state.
bool test_postcommit_quarantine()
{
    Fixture f;
    HzBackend backend(f.clock);
    ReceiverEngine engine(backend, f.ids.data(), f.ids.size(), f.tokens, f.clock);
    HZ_CHECK(configure(engine, true));
    const auto lease = engine.acquire(1U, kTerrestrial);
    HZ_CHECK(lease);
    const std::uint64_t id = lease.value().lease_id;
    auto admitted = engine.try_admit_tune(1U, hz_request(id, kT13), f.clock.now);
    HZ_CHECK(admitted && engine.execute_admitted_tune(admitted.value()));
    admitted = engine.try_admit_tune(1U, hz_request(id, 767142857U), f.clock.now);
    HZ_CHECK(admitted);
    backend.commit_advance_ms = 5000U;
    backend.calls.clear();
    HZ_CHECK(failed(engine.execute_admitted_tune(admitted.value()), Error::TIMEOUT));
    HZ_CHECK(backend.count("commit") == 1 && backend.count("rollback") == 0);
    const auto status = engine.status().value();
    HZ_CHECK(status.states[0] == ReceiverState::error);
    HZ_CHECK(!status.tune_metadata[0].current_valid);
    HZ_CHECK(status.tune_metadata[0].last_successful.requested_hz == kT13 &&
             status.tune_metadata[0].last_successful_revision == 1U);
    // Existing exact cleanup closes the quarantined receiver.
    HZ_CHECK(engine.release(1U, id));
    HZ_CHECK(engine.status().value().states[0] == ReceiverState::free);
    return true;
}

// Satellite selection placement is shared with the legacy algorithm.
bool test_satellite_selection_after_programming()
{
    Fixture f;
    HzBackend backend(f.clock);
    ReceiverEngine engine(backend, f.ids.data(), f.ids.size(), f.tokens, f.clock);
    HZ_CHECK(configure(engine, true));
    const auto lease = engine.acquire(1U, kSatellite);
    HZ_CHECK(lease);
    HzTuneRequest request = hz_request(lease.value().lease_id, 1049480000U);
    request.prepared_receiver = kSatellite;
    request.system = ReceiverSystem::satellite;
    request.slot = 3U;
    backend.calls.clear();
    const auto admitted = engine.try_admit_tune(1U, request, f.clock.now);
    HZ_CHECK(admitted && engine.execute_admitted_tune(admitted.value()));
    const std::vector<std::string> expected{"power 513", "tune_hz 513 1049480000", "lock 513",
                                            "slot 513 3", "commit 513"};
    HZ_CHECK(backend.calls == expected);
    const TuneMetadata m = metadata(engine, 1U);
    HZ_CHECK(m.current.requested_hz == 1049480000U && m.current.slot == 3U);
    return true;
}

}  // namespace

int main()
{
    const bool ok = test_exact_hz_survives() && test_legacy_entrance_unchanged() &&
                    test_default_strict_callback_unsupported() &&
                    test_admission_precedence() && test_programmed_and_lock_evidence() &&
                    test_readiness_policies() && test_programming_error() &&
                    test_late_and_cancelled_programming() && test_postcommit_quarantine() &&
                    test_satellite_selection_after_programming();
    if (!ok) return 1;
    std::printf("receiver Hz tests passed\n");
    return 0;
}
