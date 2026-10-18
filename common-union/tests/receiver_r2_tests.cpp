// SPDX-License-Identifier: MIT
//
// R2 strict admission, ticket, deadline and cancellation tests.  These are
// additive: the legacy direct entrance keeps its old behavior and is covered
// by receiver_engine_tests.cpp.
#include "dtv/userland/detail/receiver_engine.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#if defined(DTV_R2_LOCK_OBSERVER)
#include <pthread.h>

// Test-binary-only linker-wrapped observer of the real registry mutex.  It is
// compiled only for the Linux GCC/Clang R2 test target that links with
// -Wl,--wrap=pthread_mutex_lock.  Production source and headers are unchanged.
// Thread roles and the holder's last-acquired mutex are thread-local, so no
// plain identity is shared across threads; only the selected registry address
// and the phase flags use atomics.
constexpr auto kObserverBound = std::chrono::seconds(10);

static thread_local int g_r2_role = 0;  // 0 none, 1 holder, 2 worker
static thread_local pthread_mutex_t* g_r2_last_acquired = nullptr;

struct R2LockObserver final {
    std::atomic<bool> enabled{false};
    std::atomic<pthread_mutex_t*> target{nullptr};
    std::atomic<bool> observe_worker{false};
    std::atomic<bool> requested{false};
    std::atomic<bool> acquired{false};
    std::atomic<bool> pause_before_real{false};
    std::atomic<bool> observer_timed_out{false};

    void reset() noexcept
    {
        enabled.store(false, std::memory_order_release);
        target.store(nullptr, std::memory_order_release);
        observe_worker.store(false, std::memory_order_release);
        requested.store(false, std::memory_order_relaxed);
        acquired.store(false, std::memory_order_relaxed);
        pause_before_real.store(false, std::memory_order_release);
        observer_timed_out.store(false, std::memory_order_relaxed);
    }
};

R2LockObserver g_r2_lock_observer;

extern "C" int __real_pthread_mutex_lock(pthread_mutex_t*);
extern "C" int __wrap_pthread_mutex_lock(pthread_mutex_t* m)
{
    R2LockObserver& observer = g_r2_lock_observer;
    if (!observer.enabled.load(std::memory_order_relaxed))
        return __real_pthread_mutex_lock(m);
    const bool observing =
        observer.observe_worker.load(std::memory_order_acquire) &&
        g_r2_role == 2 &&
        m == observer.target.load(std::memory_order_acquire);
    if (observing) {
        observer.requested.store(true, std::memory_order_release);
        // Bounded pause; on timeout set the flag and proceed with the real
        // operation (never fabricate lock success or skip the real call).
        const auto deadline = std::chrono::steady_clock::now() + kObserverBound;
        while (observer.pause_before_real.load(std::memory_order_acquire)) {
            if (std::chrono::steady_clock::now() >= deadline) {
                observer.observer_timed_out.store(true, std::memory_order_release);
                break;
            }
            std::this_thread::yield();
        }
    }
    const int rc = __real_pthread_mutex_lock(m);
    if (rc == 0 && g_r2_role == 1) g_r2_last_acquired = m;
    if (observing) {
        observer.acquired.store(true, std::memory_order_release);
        observer.observe_worker.store(false, std::memory_order_release);
    }
    return rc;
}
#endif  // DTV_R2_LOCK_OBSERVER

namespace {

using namespace dtv::userland;
using namespace dtv::userland::detail;

#define R2_CHECK(condition)                                                           \
    do {                                                                               \
        if (!(condition)) {                                                            \
            std::fprintf(stderr, "receiver R2 check failed at line %d: %s\n",        \
                         __LINE__, #condition);                                        \
            return false;                                                              \
        }                                                                              \
    } while (false)

constexpr std::chrono::milliseconds kWaitBound{10000};
constexpr int kNoReceiver = -1;

constexpr ReceiverId kTerrestrial = 17U;
constexpr ReceiverId kSatellite = 513U;

std::string num(std::uint64_t value)
{
    return std::to_string(value);
}

std::string sys(ReceiverSystem system)
{
    return std::to_string(static_cast<unsigned>(system));
}

class Calls final {
public:
    void add(int receiver, std::string text)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        entries_.push_back(Entry{receiver, std::move(text)});
    }

    std::vector<std::string> take()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<std::string> texts;
        for (const Entry& entry : entries_) texts.push_back(entry.text);
        entries_.clear();
        return texts;
    }

    std::vector<std::string> take_for(int receiver)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<std::string> texts;
        for (const Entry& entry : entries_) {
            if (entry.receiver == receiver) texts.push_back(entry.text);
        }
        entries_.clear();
        return texts;
    }

    void clear()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        entries_.clear();
    }

private:
    struct Entry final {
        int receiver;
        std::string text;
    };

    std::mutex mutex_;
    std::vector<Entry> entries_;
};

bool same_calls(const char* scenario, const std::vector<std::string>& actual,
                const std::vector<std::string>& expected)
{
    if (actual == expected) return true;
    std::fprintf(stderr, "call mismatch in %s\n  expected:\n", scenario);
    for (const std::string& entry : expected) std::fprintf(stderr, "    %s\n", entry.c_str());
    std::fprintf(stderr, "  actual:\n");
    for (const std::string& entry : actual) std::fprintf(stderr, "    %s\n", entry.c_str());
    return false;
}

template <typename T>
bool failed(const Result<T>& result, Error expected)
{
    return !result && result.error() == expected;
}

// Runs a cleanup lambda on scope exit so a new threaded test always releases
// its gate and joins its worker before a failing assertion returns.
template <typename F>
struct ScopeExit final {
    F fn;
    ~ScopeExit() noexcept { fn(); }
};

template <typename F>
ScopeExit<F> on_scope_exit(F fn)
{
    return ScopeExit<F>{fn};
}

// One-shot rendezvous used to deterministically hold a callback that runs
// under the registry lock.  Every wait is bounded.
class Gate final {
public:
    void arm(int receiver)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        receiver_ = receiver;
        entered_ = false;
        open_ = false;
    }

    void pass(int receiver)
    {
        std::unique_lock<std::mutex> lock(mutex_);
        if (receiver_ != receiver) return;
        receiver_ = kNoReceiver;
        entered_ = true;
        changed_.notify_all();
        changed_.wait_for(lock, kWaitBound, [&]() noexcept { return open_; });
    }

    bool wait_entered()
    {
        std::unique_lock<std::mutex> lock(mutex_);
        return changed_.wait_for(lock, kWaitBound, [&]() noexcept { return entered_; });
    }

    void open()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        open_ = true;
        receiver_ = kNoReceiver;
        changed_.notify_all();
    }

private:
    std::mutex mutex_;
    std::condition_variable changed_;
    int receiver_ = kNoReceiver;
    bool entered_ = false;
    bool open_ = false;
};

class FakeClock final : public ReceiverClock {
public:
    explicit FakeClock(Calls& calls) noexcept : calls_(calls) {}

    std::uint64_t monotonic_ms() noexcept override
    {
        monotonic_calls.fetch_add(1U);
#if defined(DTV_R2_LOCK_OBSERVER)
        if (worker_known && pthread_equal(pthread_self(), worker_thread)) {
            const int sample = worker_samples.fetch_add(1U, std::memory_order_relaxed) + 1;
            if (stop_sample == sample && gate_sample_active) {
                gate_sample_active = false;
                sample_gate.pass(0);
            }
        }
#endif
        if (gate_clock.load()) {
            gate_clock.store(false);
            clock_gate.pass(0);
        }
        return now.load();
    }

    void sleep_ms(std::uint32_t milliseconds) noexcept override
    {
        calls_.add(kNoReceiver, "sleep " + num(milliseconds));
        if (gate_sleep.load()) {
            gate_sleep.store(false);
            sleep_gate.pass(0);
        }
        now.fetch_add(milliseconds + oversleep_extra);
    }

    std::atomic<std::uint64_t> now{0U};
    std::atomic<std::uint64_t> monotonic_calls{0U};
    std::uint64_t oversleep_extra = 0U;
    // One-shot gate for the first monotonic_ms call (the initial clock delay).
    Gate clock_gate;
    std::atomic<bool> gate_clock{false};
    // One-shot gate for an inter-poll sleep.
    Gate sleep_gate;
    std::atomic<bool> gate_sleep{false};
#if defined(DTV_R2_LOCK_OBSERVER)
    // Worker-thread-only monotonic sample gate for the registry proofs.
    pthread_t worker_thread{};
    bool worker_known = false;
    int stop_sample = -1;
    std::atomic<int> worker_samples{0};
    bool gate_sample_active = false;
    Gate sample_gate;
#endif

private:
    Calls& calls_;
};

class FakeBackend final : public ReceiverBackend {
public:
    FakeBackend(Calls& calls, FakeClock& clock) noexcept : calls_(calls), clock_(clock) {}

    bool supports(ReceiverId receiver, ReceiverSystem system) const noexcept override
    {
        calls_.add(receiver, "supports " + num(receiver) + " " + sys(system));
        mutable_mutex_.lock();
        mutable_mutex_.unlock();
        supports_gate.pass(receiver);
        return receiver == kSatellite ? system == ReceiverSystem::satellite
                                      : system == ReceiverSystem::terrestrial;
    }

    bool selects_satellite_stream_before_tune() const noexcept override { return false; }

    std::uint32_t lock_settle_ms(ReceiverId, ReceiverSystem system) const noexcept override
    {
        return system == ReceiverSystem::terrestrial ? settle_ms : 0U;
    }

    Result<void> open(ReceiverId receiver) noexcept override
    {
        calls_.add(receiver, "open " + num(receiver));
        return Result<void>::success();
    }

    Result<void> tune(ReceiverId receiver, ReceiverSystem system,
                      std::uint32_t frequency_khz, std::uint32_t timeout_ms) noexcept override
    {
        calls_.add(receiver, "tune " + num(receiver) + " " + sys(system) + " " +
                                 num(frequency_khz) + " " + num(timeout_ms));
        tune_gate.pass(receiver);
        return Result<void>::success();
    }

    Result<bool> is_locked(ReceiverId receiver, ReceiverSystem) noexcept override
    {
        calls_.add(receiver, "lock " + num(receiver));
        if (lock_advance_ms != 0U) clock_.now.fetch_add(lock_advance_ms);
        lock_gate.pass(receiver);
        return Result<bool>::success(lock_locked);
    }

    Result<void> select_satellite_slot(ReceiverId receiver, std::uint8_t slot,
                                       std::uint32_t timeout_ms) noexcept override
    {
        calls_.add(receiver, "slot " + num(receiver) + " " + num(slot) + " " +
                                 num(timeout_ms));
        if (select_advance_ms != 0U) clock_.now.fetch_add(select_advance_ms);
        select_gate.pass(receiver);
        return Result<void>::success();
    }

    Result<void> select_satellite_tsid(ReceiverId receiver, std::uint16_t tsid,
                                       std::uint32_t timeout_ms) noexcept override
    {
        calls_.add(receiver, "tsid " + num(receiver) + " " + num(tsid) + " " +
                                 num(timeout_ms));
        return Result<void>::success();
    }

    Result<void> close(ReceiverId receiver) noexcept override
    {
        calls_.add(receiver, "close " + num(receiver));
        close_gate.pass(receiver);
        return close_error == Error::OK ? Result<void>::success()
                                        : Result<void>::failure(close_error);
    }

    Result<void> begin_tune_power(ReceiverId receiver, ReceiverSystem,
                                  std::uint8_t lnb_voltage) noexcept override
    {
        calls_.add(receiver, "power " + num(receiver) + " " + num(lnb_voltage));
        pending_power = true;
        if (power_wait_ms != 0U) clock_.sleep_ms(power_wait_ms);
        power_gate.pass(receiver);
        return Result<void>::success();
    }

    Result<void> commit_tune_power(ReceiverId receiver) noexcept override
    {
        calls_.add(receiver, "commit " + num(receiver));
        // Model the production coordinators: a successful commit consumes the
        // pending power transaction, so a later rollback is invalid or a no-op.
        if (consume_power_transaction) pending_power = false;
        if (commit_wait_ms != 0U) clock_.sleep_ms(commit_wait_ms);
        commit_gate.pass(receiver);
        return Result<void>::success();
    }

    Result<void> rollback_tune_power(ReceiverId receiver) noexcept override
    {
        calls_.add(receiver, "rollback " + num(receiver));
        if (consume_power_transaction && !pending_power)
            return Result<void>::failure(Error::INVALID_ARGUMENT);
        return Result<void>::success();
    }

    void mark_disconnected(ReceiverId receiver) noexcept override
    {
        calls_.add(receiver, "lost " + num(receiver));
    }

    Result<void> shutdown() noexcept override
    {
        calls_.add(kNoReceiver, "shutdown");
        return Result<void>::success();
    }

    Result<void> start_capture(ReceiverId receiver, ReceiverSystem) noexcept override
    {
        calls_.add(receiver, "start " + num(receiver));
        return Result<void>::success();
    }

    Result<void> stop_capture(ReceiverId receiver, ReceiverSystem) noexcept override
    {
        calls_.add(receiver, "stop " + num(receiver));
        return Result<void>::success();
    }

    std::uint32_t power_wait_ms = 0U;
    std::uint32_t commit_wait_ms = 0U;
    std::uint32_t lock_advance_ms = 0U;
    std::uint32_t settle_ms = 0U;
    std::uint32_t select_advance_ms = 0U;
    bool lock_locked = true;
    Error close_error = Error::OK;
    bool consume_power_transaction = false;
    bool pending_power = false;
    mutable Gate supports_gate;
    Gate tune_gate;
    Gate commit_gate;
    Gate power_gate;
    Gate select_gate;
    Gate lock_gate;
    Gate close_gate;

private:
    Calls& calls_;
    FakeClock& clock_;
    mutable std::mutex mutable_mutex_;
};

class CountingTokens final : public ReceiverTokenSource {
public:
    Result<ReceiverToken> generate() noexcept override
    {
        ReceiverToken token{};
        token[0U] = next.fetch_add(1U);
        return Result<ReceiverToken>::success(token);
    }

    std::atomic<std::uint8_t> next{1U};
};

// Token source that parks inside generate(), which runs under the registry
// lock, so a strict admission sees registry contention.
class GatedTokens final : public ReceiverTokenSource {
public:
    Result<ReceiverToken> generate() noexcept override
    {
        gate.pass(0);
        ReceiverToken token{};
        token[0U] = next.fetch_add(1U);
        return Result<ReceiverToken>::success(token);
    }

    Gate gate;
    std::atomic<std::uint8_t> next{1U};
};

struct Fixture final {
    Calls calls;
    FakeClock clock{calls};
    FakeBackend backend{calls, clock};
    CountingTokens tokens;
    std::array<ReceiverId, 2> ids{kTerrestrial, kSatellite};
};

bool configure_strict(ReceiverEngine& engine)
{
    const std::array<ReceiverCapability, 2> capabilities{
        ReceiverCapability{kTerrestrial, true, false},
        ReceiverCapability{kSatellite, false, true}};
    return static_cast<bool>(
        engine.configure_strict_capabilities(capabilities.data(), capabilities.size()));
}

TuneRequest terrestrial(std::uint64_t lease_id, Error validation = Error::OK)
{
    TuneRequest request{};
    request.lease_id = lease_id;
    request.system = ReceiverSystem::terrestrial;
    request.frequency_khz = 557000U;
    request.timeout_ms = 1000U;
    request.validation = validation;
    return request;
}

TuneRequest satellite_slot(std::uint64_t lease_id, std::uint16_t slot)
{
    TuneRequest request{};
    request.lease_id = lease_id;
    request.system = ReceiverSystem::satellite;
    request.frequency_khz = 1049480U;
    request.slot = slot;
    request.timeout_ms = 1000U;
    return request;
}

int count_calls(const std::vector<std::string>& calls, const std::string& prefix)
{
    int total = 0;
    for (const std::string& entry : calls) {
        if (entry.rfind(prefix, 0U) == 0U) ++total;
    }
    return total;
}

bool test_strict_setup_validation()
{
    Fixture f;
    ReceiverEngine engine(f.backend, f.ids.data(), f.ids.size(), f.tokens, f.clock);
    R2_CHECK(!engine.strict_capabilities_configured());
    R2_CHECK(failed(engine.try_admit_tune(1U, terrestrial(1U), 0U), Error::NOT_READY));

    // Wrong count, duplicate receiver, missing receiver and an empty entry all
    // fail without enabling strict mode.
    const std::array<ReceiverCapability, 1> one{
        ReceiverCapability{kTerrestrial, true, false}};
    R2_CHECK(failed(engine.configure_strict_capabilities(one.data(), one.size()),
                    Error::INVALID_ARGUMENT));
    const std::array<ReceiverCapability, 2> duplicate{
        ReceiverCapability{kTerrestrial, true, false},
        ReceiverCapability{kTerrestrial, true, true}};
    R2_CHECK(failed(engine.configure_strict_capabilities(duplicate.data(),
                                                         duplicate.size()),
                    Error::INVALID_ARGUMENT));
    const std::array<ReceiverCapability, 2> missing{
        ReceiverCapability{kTerrestrial, true, true},
        ReceiverCapability{999U, true, true}};
    R2_CHECK(failed(engine.configure_strict_capabilities(missing.data(),
                                                         missing.size()),
                    Error::INVALID_ARGUMENT));
    const std::array<ReceiverCapability, 2> empty{
        ReceiverCapability{kTerrestrial, false, false},
        ReceiverCapability{kSatellite, false, true}};
    R2_CHECK(failed(engine.configure_strict_capabilities(empty.data(), empty.size()),
                    Error::INVALID_ARGUMENT));
    R2_CHECK(!engine.strict_capabilities_configured());
    R2_CHECK(failed(engine.configure_strict_capabilities(nullptr, 0U),
                    Error::INVALID_ARGUMENT));
    return true;
}

bool test_admission_is_callback_free()
{
    Fixture f;
    ReceiverEngine engine(f.backend, f.ids.data(), f.ids.size(), f.tokens, f.clock);
    R2_CHECK(configure_strict(engine) && engine.strict_capabilities_configured());
    R2_CHECK(engine.acquire(1U, kTerrestrial));
    f.calls.clear();
    const std::uint64_t clock_before = f.clock.monotonic_calls.load();

    const auto admitted = engine.try_admit_tune(1U, terrestrial(1U), 0U);
    R2_CHECK(admitted);
    R2_CHECK(admitted.value().tune_revision == 1U);
    R2_CHECK(admitted.value().accepted_at_ms == 0U);
    // Admission performs no backend, clock, or ID-source call.
    R2_CHECK(same_calls("admission", f.calls.take(), {}));
    R2_CHECK(f.clock.monotonic_calls.load() == clock_before);

    // A second admission while the first is pending is BUSY with no change.
    R2_CHECK(failed(engine.try_admit_tune(1U, terrestrial(1U), 0U), Error::BUSY));
    const auto status = engine.status();
    R2_CHECK(status && status.value().tune_metadata[0U].revision == 1U &&
             !status.value().tune_metadata[0U].current_valid &&
             status.value().operations[0U] == ReceiverTuneOperation::tuning);

    // Strict execution never consults the virtual supports predicate.
    R2_CHECK(engine.execute_admitted_tune(admitted.value()));
    const auto executed = f.calls.take();
    R2_CHECK(count_calls(executed, "supports") == 0);
    R2_CHECK(same_calls("admitted execution", executed,
                        {"power 17 0", "tune 17 1 557000 1000", "lock 17", "commit 17"}));
    const auto after = engine.status();
    R2_CHECK(after && after.value().tune_metadata[0U].current_valid &&
             after.value().tune_metadata[0U].current_revision == 1U &&
             after.value().tune_metadata[0U].last_successful_valid &&
             after.value().operations[0U] == ReceiverTuneOperation::idle);
    return true;
}

bool test_registry_contention_and_gated_callbacks()
{
    {
        Fixture f;
        ReceiverEngine engine(f.backend, f.ids.data(), f.ids.size(), f.tokens, f.clock);
        R2_CHECK(configure_strict(engine));
        R2_CHECK(engine.acquire(1U, kTerrestrial));
        f.calls.clear();

        // Park a legacy supports callback (which runs under the registry lock
        // while ACQUIRE commits) and show strict admission returns BUSY
        // instead of waiting.
        f.backend.supports_gate.arm(static_cast<int>(kSatellite));
        Result<AcquireResult> second = Result<AcquireResult>::failure(Error::INTERNAL);
        std::thread accepter([&]() { second = engine.acquire(2U, kSatellite); });
        R2_CHECK(f.backend.supports_gate.wait_entered());
        R2_CHECK(failed(engine.try_admit_tune(1U, terrestrial(1U), 0U), Error::BUSY));
        f.backend.supports_gate.open();
        accepter.join();
        R2_CHECK(second);
        // Admission contention changed no tune metadata.
        const auto status = engine.status();
        R2_CHECK(status && status.value().tune_metadata[0U].revision == 0U &&
                 !status.value().tune_metadata[0U].current_valid &&
                 status.value().operations[0U] == ReceiverTuneOperation::idle);
    }
    {
        // A gated token source (entropy) holds the registry lock during a
        // concurrent ACQUIRE on another receiver; admission must still return
        // promptly with BUSY rather than block behind it.
        Calls calls;
        FakeClock clock(calls);
        FakeBackend backend(calls, clock);
        GatedTokens tokens;
        const std::array<ReceiverId, 2> ids{kTerrestrial, kSatellite};
        ReceiverEngine engine(backend, ids.data(), ids.size(), tokens, clock);
        R2_CHECK(configure_strict(engine));
        R2_CHECK(engine.acquire(1U, kTerrestrial));

        tokens.gate.arm(0);
        Result<AcquireResult> other = Result<AcquireResult>::failure(Error::INTERNAL);
        std::thread accepter([&]() { other = engine.acquire(2U, kSatellite); });
        R2_CHECK(tokens.gate.wait_entered());
        R2_CHECK(failed(engine.try_admit_tune(1U, terrestrial(1U), 0U), Error::BUSY));
        tokens.gate.open();
        accepter.join();
        R2_CHECK(other);
    }
    return true;
}

bool test_expired_and_power_late()
{
    Fixture f;
    ReceiverEngine engine(f.backend, f.ids.data(), f.ids.size(), f.tokens, f.clock, nullptr,
                          nullptr);
    R2_CHECK(configure_strict(engine));
    R2_CHECK(engine.acquire(1U, kTerrestrial));
    f.calls.clear();

    // Queue waiting consumes the whole budget: no power or front-end I/O.
    auto expired = engine.try_admit_tune(1U, terrestrial(1U), 0U);
    R2_CHECK(expired);
    f.clock.now.store(1000U);
    R2_CHECK(failed(engine.execute_admitted_tune(expired.value()), Error::TIMEOUT));
    R2_CHECK(same_calls("expired", f.calls.take(), {}));
    const auto after_expired = engine.status();
    R2_CHECK(after_expired && !after_expired.value().tune_metadata[0U].current_valid &&
             after_expired.value().operations[0U] == ReceiverTuneOperation::idle);

    // Power preparation consumes the budget: rollback, no frontend.
    f.clock.now.store(0U);
    f.backend.power_wait_ms = 1000U;
    auto late_power = engine.try_admit_tune(1U, terrestrial(1U), 0U);
    R2_CHECK(late_power);
    R2_CHECK(failed(engine.execute_admitted_tune(late_power.value()), Error::TIMEOUT));
    R2_CHECK(same_calls("power late", f.calls.take(),
                        {"power 17 0", "sleep 1000", "rollback 17"}));
    R2_CHECK(!engine.status().value().tune_metadata[0U].current_valid);
    f.backend.power_wait_ms = 0U;
    return true;
}

bool test_budget_is_not_restarted_and_commit_is_gated()
{
    Fixture f;
    ReceiverEngine engine(f.backend, f.ids.data(), f.ids.size(), f.tokens, f.clock);
    R2_CHECK(configure_strict(engine));
    R2_CHECK(engine.acquire(1U, kTerrestrial));
    f.calls.clear();

    // Queue wait plus power consumption reduce the tune timeout argument; the
    // budget is never restarted at dequeue or at a backend stage.
    auto admitted = engine.try_admit_tune(1U, terrestrial(1U), 0U);
    R2_CHECK(admitted);
    f.clock.now.store(400U);
    f.backend.power_wait_ms = 300U;
    R2_CHECK(engine.execute_admitted_tune(admitted.value()));
    R2_CHECK(same_calls("budget", f.calls.take(),
                        {"power 17 0", "sleep 300", "tune 17 1 557000 300", "lock 17",
                         "commit 17"}));
    f.backend.power_wait_ms = 0U;

    // commit_tune_power consumes the remaining budget after the last
    // pre-callback check.  The power transaction is already committed, so the
    // engine must not call the invalid/no-op post-commit rollback: it
    // quarantines the receiver instead.
    f.clock.now.store(0U);
    f.calls.clear();
    f.backend.commit_wait_ms = 1000U;
    f.backend.consume_power_transaction = true;
    auto late_commit = engine.try_admit_tune(1U, terrestrial(1U), 0U);
    R2_CHECK(late_commit);
    R2_CHECK(failed(engine.execute_admitted_tune(late_commit.value()), Error::TIMEOUT));
    R2_CHECK(same_calls("commit late", f.calls.take(),
                        {"power 17 0", "tune 17 1 557000 1000", "lock 17", "commit 17",
                         "sleep 1000"}));
    const auto after = engine.status();
    R2_CHECK(after && !after.value().tune_metadata[0U].current_valid &&
             after.value().operations[0U] == ReceiverTuneOperation::idle &&
             after.value().states[0U] == ReceiverState::error);
    // Quarantined: not reusable until the exact RELEASE performs cleanup.
    R2_CHECK(failed(engine.start_stream(1U, 1U), Error::NOT_READY));
    R2_CHECK(failed(engine.try_admit_tune(1U, terrestrial(1U), 0U), Error::NOT_READY));
    // The quarantined lease is still held, so reacquire is BUSY, not ready.
    R2_CHECK(failed(engine.acquire(3U, kTerrestrial), Error::BUSY));
    f.calls.clear();
    R2_CHECK(engine.release(1U, 1U));
    R2_CHECK(same_calls("quarantine release", f.calls.take(), {"close 17"}));
    R2_CHECK(engine.acquire(3U, kTerrestrial));
    f.backend.commit_wait_ms = 0U;
    f.backend.consume_power_transaction = false;
    return true;
}

bool test_postcommit_cancellation_quarantines()
{
    Fixture f;
    ReceiverEngine engine(f.backend, f.ids.data(), f.ids.size(), f.tokens, f.clock);
    R2_CHECK(configure_strict(engine));
    R2_CHECK(engine.acquire(1U, kTerrestrial));
    f.calls.clear();

    auto admitted = engine.try_admit_tune(1U, terrestrial(1U), 0U);
    R2_CHECK(admitted);
    f.backend.consume_power_transaction = true;
    f.backend.commit_gate.arm(static_cast<int>(kTerrestrial));
    Error primary = Error::OK;
    {
        std::thread worker([&]() {
            const auto result = engine.execute_admitted_tune(admitted.value());
            primary = result ? Error::OK : result.error();
        });
        const auto guard = on_scope_exit([&]() noexcept {
            f.backend.commit_gate.open();
            if (worker.joinable()) worker.join();
        });
        R2_CHECK(f.backend.commit_gate.wait_entered());
        // The owner is revoked only while the commit is already in flight; the
        // rejected late success must not invoke post-commit rollback.
        R2_CHECK(engine.revoke_tune_ticket(1U, admitted.value().lease_id));
    }
    R2_CHECK(primary == Error::NOT_FOUND);
    R2_CHECK(same_calls("cancelled post-commit", f.calls.take(),
                        {"power 17 0", "tune 17 1 557000 1000", "lock 17", "commit 17"}));
    const auto status = engine.status();
    R2_CHECK(status && !status.value().tune_metadata[0U].current_valid &&
             status.value().states[0U] == ReceiverState::error);
    f.backend.consume_power_transaction = false;
    return true;
}

// Hold the worker at one returned stage, revoke the exact ticket, then assert
// the next stage never starts and the pre-commit rollback path runs.  The
// worker is always released and joined before any assertion can return.
bool revoked_at_stage(Fixture& f, ReceiverEngine& engine, Gate& gate,
                      int gate_receiver, std::uint64_t client,
                      const TuneRequest& request, Result<void>& executed)
{
    f.calls.clear();
    const auto admitted = engine.try_admit_tune(client, request, 0U);
    if (!admitted) return false;
    gate.arm(gate_receiver);
    executed = Result<void>::failure(Error::INTERNAL);
    {
        std::thread worker(
            [&]() { executed = engine.execute_admitted_tune(admitted.value()); });
        const auto guard = on_scope_exit([&]() noexcept {
            gate.open();
            if (worker.joinable()) worker.join();
        });
        if (!gate.wait_entered()) return false;
        if (!engine.revoke_tune_ticket(client, admitted.value().lease_id))
            return false;
    }
    return true;
}

bool test_cancellation_during_gated_tune()
{
    Fixture f;
    ReceiverEngine engine(f.backend, f.ids.data(), f.ids.size(), f.tokens, f.clock);
    R2_CHECK(configure_strict(engine));
    R2_CHECK(engine.acquire(1U, kTerrestrial));
    R2_CHECK(engine.acquire(1U, kSatellite));

    Result<void> executed = Result<void>::failure(Error::INTERNAL);
    // Revocation during power preparation stops the next stage, so no frontend
    // tune and no lock are issued.
    R2_CHECK(revoked_at_stage(f, engine, f.backend.power_gate,
                              static_cast<int>(kTerrestrial), 1U, terrestrial(1U),
                              executed));
    R2_CHECK(failed(executed, Error::NOT_FOUND));
    R2_CHECK(same_calls("cancelled power", f.calls.take(),
                        {"power 17 0", "rollback 17"}));

    // Revocation during the finite frontend tune stops the next stage: no lock
    // callback runs after the revocation is observed.  The power transaction
    // has not committed, so the ordinary pre-commit rollback applies.
    R2_CHECK(revoked_at_stage(f, engine, f.backend.tune_gate,
                              static_cast<int>(kTerrestrial), 1U, terrestrial(1U),
                              executed));
    R2_CHECK(failed(executed, Error::NOT_FOUND));
    R2_CHECK(same_calls("cancelled tune", f.calls.take(),
                        {"power 17 0", "tune 17 1 557000 1000", "rollback 17"}));

    // Revocation during the lock poll (successful lock) stops before commit.
    R2_CHECK(revoked_at_stage(f, engine, f.backend.lock_gate,
                              static_cast<int>(kTerrestrial), 1U, terrestrial(1U),
                              executed));
    R2_CHECK(failed(executed, Error::NOT_FOUND));
    R2_CHECK(same_calls("cancelled lock", f.calls.take(),
                        {"power 17 0", "tune 17 1 557000 1000", "lock 17",
                         "rollback 17"}));

    // Revocation during an UNLOCKED (false) lock result stops before another
    // poll/sleep: exactly one lock callback, then rollback.
    f.backend.lock_locked = false;
    R2_CHECK(revoked_at_stage(f, engine, f.backend.lock_gate,
                              static_cast<int>(kTerrestrial), 1U, terrestrial(1U),
                              executed));
    R2_CHECK(failed(executed, Error::NOT_FOUND));
    R2_CHECK(same_calls("cancelled unlocked", f.calls.take(),
                        {"power 17 0", "tune 17 1 557000 1000", "lock 17",
                         "rollback 17"}));
    f.backend.lock_locked = true;

    // Revocation during satellite selection stops before commit.
    R2_CHECK(revoked_at_stage(f, engine, f.backend.select_gate,
                              static_cast<int>(kSatellite), 1U, satellite_slot(2U, 0U),
                              executed));
    R2_CHECK(failed(executed, Error::NOT_FOUND));
    R2_CHECK(same_calls("cancelled select", f.calls.take(),
                        {"power 513 0", "tune 513 2 1049480 1000", "lock 513",
                         "slot 513 0 1000", "rollback 513"}));

    const auto status = engine.status();
    R2_CHECK(status && !status.value().tune_metadata[0U].current_valid &&
             !status.value().tune_metadata[1U].current_valid);
    return true;
}

bool test_stale_ticket_and_revision_monotonic()
{
    Fixture f;
    ReceiverEngine engine(f.backend, f.ids.data(), f.ids.size(), f.tokens, f.clock);
    R2_CHECK(configure_strict(engine));
    R2_CHECK(engine.acquire(1U, kTerrestrial));

    auto first = engine.try_admit_tune(1U, terrestrial(1U), 0U);
    R2_CHECK(first && first.value().tune_revision == 1U);
    // Abortion keeps the advanced revision and returns the receiver to idle.
    R2_CHECK(engine.abort_unstarted_tune(first.value(), Error::INTERNAL));
    const auto aborted = engine.status();
    R2_CHECK(aborted && aborted.value().tune_metadata[0U].revision == 1U &&
             aborted.value().operations[0U] == ReceiverTuneOperation::idle);

    auto second = engine.try_admit_tune(1U, terrestrial(1U), 20U);
    R2_CHECK(second && second.value().tune_revision == 2U);
    R2_CHECK(engine.abort_unstarted_tune(second.value(), Error::INTERNAL));

    // Release and reacquire.  The numeric lease ID may repeat, but the lease
    // instance changes, so a queued stale ticket can never touch it.
    R2_CHECK(engine.release(1U, 1U));
    R2_CHECK(engine.acquire(1U, kTerrestrial));
    f.calls.clear();
    R2_CHECK(failed(engine.execute_admitted_tune(first.value()), Error::NOT_FOUND));
    R2_CHECK(failed(engine.execute_admitted_tune(second.value()), Error::NOT_FOUND));
    R2_CHECK(same_calls("stale", f.calls.take(), {}));
    const auto status = engine.status();
    R2_CHECK(status && !status.value().tune_metadata[0U].current_valid);
    return true;
}

bool test_precedence_and_unknown_system()
{
    Fixture f;
    ReceiverEngine engine(f.backend, f.ids.data(), f.ids.size(), f.tokens, f.clock);
    R2_CHECK(configure_strict(engine));
    R2_CHECK(engine.acquire(1U, kTerrestrial));
    f.calls.clear();

    // Unknown lease first.
    R2_CHECK(failed(engine.try_admit_tune(1U, terrestrial(99U), 0U), Error::NOT_FOUND));
    // Unknown / unsupported system via the immutable table, before validation.
    TuneRequest unknown = terrestrial(1U, Error::INVALID_ARGUMENT);
    unknown.system = static_cast<ReceiverSystem>(7U);
    R2_CHECK(failed(engine.try_admit_tune(1U, unknown, 0U), Error::INVALID_ARGUMENT));
    // Unsupported but valid system via the table.
    TuneRequest satellite = terrestrial(1U);
    satellite.system = ReceiverSystem::satellite;
    R2_CHECK(failed(engine.try_admit_tune(1U, satellite, 0U), Error::INVALID_ARGUMENT));
    // Request validation decision is returned last.
    R2_CHECK(failed(engine.try_admit_tune(1U, terrestrial(1U, Error::UNSUPPORTED), 0U),
                    Error::UNSUPPORTED));
    R2_CHECK(same_calls("rejected", f.calls.take(), {}));
    const auto status = engine.status();
    R2_CHECK(status && status.value().tune_metadata[0U].revision == 0U &&
             !status.value().tune_metadata[0U].current_valid &&
             status.value().operations[0U] == ReceiverTuneOperation::idle);
    return true;
}

bool test_legacy_entry_respects_pending_ticket()
{
    Fixture f;
    ReceiverEngine engine(f.backend, f.ids.data(), f.ids.size(), f.tokens, f.clock);
    R2_CHECK(configure_strict(engine));
    R2_CHECK(engine.acquire(1U, kTerrestrial));
    auto admitted = engine.try_admit_tune(1U, terrestrial(1U), 0U);
    R2_CHECK(admitted);
    f.calls.clear();
    // The legacy direct entrance is excluded while a strict ticket is pending.
    R2_CHECK(failed(engine.tune(1U, terrestrial(1U)), Error::BUSY));
    R2_CHECK(same_calls("legacy excluded", f.calls.take(), {}));
    R2_CHECK(engine.abort_unstarted_tune(admitted.value(), Error::INTERNAL));
    // After completion the legacy entrance still works and keeps its old trace.
    R2_CHECK(engine.tune(1U, terrestrial(1U)));
    R2_CHECK(same_calls("legacy after", f.calls.take(),
                        {"supports 17 1", "power 17 0", "tune 17 1 557000 1000",
                         "lock 17", "commit 17"}));
    return true;
}

bool test_precommit_deadline_boundaries()
{
    Fixture f;
    ReceiverEngine engine(f.backend, f.ids.data(), f.ids.size(), f.tokens, f.clock);
    R2_CHECK(configure_strict(engine));
    R2_CHECK(engine.acquire(1U, kTerrestrial));
    f.calls.clear();

    // A lock callback that returns true at/after the deadline must not reach
    // commit: pre-commit expiry rolls back and publishes no current.
    f.backend.lock_advance_ms = 1000U;
    {
        auto admitted = engine.try_admit_tune(1U, terrestrial(1U), 0U);
        R2_CHECK(admitted);
        R2_CHECK(failed(engine.execute_admitted_tune(admitted.value()), Error::TIMEOUT));
    }
    R2_CHECK(same_calls("lock late", f.calls.take(),
                        {"power 17 0", "tune 17 1 557000 1000", "lock 17", "rollback 17"}));
    R2_CHECK(!engine.status().value().tune_metadata[0U].current_valid);
    f.backend.lock_advance_ms = 0U;

    // A settle sleep that overshoots the budget is caught before commit.
    f.clock.now.store(0U);
    f.calls.clear();
    f.backend.settle_ms = 340U;
    f.clock.oversleep_extra = 1000U;
    {
        auto admitted = engine.try_admit_tune(1U, terrestrial(1U), 0U);
        R2_CHECK(admitted);
        R2_CHECK(failed(engine.execute_admitted_tune(admitted.value()), Error::TIMEOUT));
    }
    R2_CHECK(same_calls("settle oversleep", f.calls.take(),
                        {"power 17 0", "tune 17 1 557000 1000", "lock 17", "sleep 340",
                         "rollback 17"}));
    f.clock.oversleep_extra = 0U;
    f.backend.settle_ms = 0U;
    return true;
}

bool test_legacy_active_operation_blocks_admission()
{
    Fixture f;
    ReceiverEngine engine(f.backend, f.ids.data(), f.ids.size(), f.tokens, f.clock);
    R2_CHECK(configure_strict(engine));
    R2_CHECK(engine.acquire(1U, kTerrestrial));
    f.calls.clear();

    f.backend.tune_gate.arm(static_cast<int>(kTerrestrial));
    Result<void> direct = Result<void>::failure(Error::INTERNAL);
    std::thread worker([&]() { direct = engine.tune(1U, terrestrial(1U)); });
    const auto cleanup = on_scope_exit([&]() noexcept {
        f.backend.tune_gate.open();
        if (worker.joinable()) worker.join();
    });
    R2_CHECK(f.backend.tune_gate.wait_entered());
    // An active legacy direct tune is an active operation: strict admission is
    // BUSY and does not advance revision, invalidate current, or write history.
    R2_CHECK(failed(engine.try_admit_tune(1U, terrestrial(1U), 0U), Error::BUSY));
    const auto status = engine.status();
    R2_CHECK(status && status.value().tune_metadata[0U].revision == 0U &&
             !status.value().tune_metadata[0U].current_valid &&
             !status.value().tune_metadata[0U].last_successful_valid &&
             status.value().operations[0U] == ReceiverTuneOperation::tuning);
    return true;
}

bool test_revision_exhaustion_fails_safely()
{
    Fixture f;
    ReceiverEngine engine(f.backend, f.ids.data(), f.ids.size(), f.tokens, f.clock);
    R2_CHECK(configure_strict(engine));
    R2_CHECK(engine.acquire(1U, kTerrestrial));
    f.backend.tune_gate.open();
    engine.set_tune_revision_for_test(
        0U, std::numeric_limits<std::uint64_t>::max());
    const auto before = engine.status().value().tune_metadata[0U];
    R2_CHECK(failed(engine.try_admit_tune(1U, terrestrial(1U), 0U),
                    Error::RESOURCE_LIMIT));
    const auto after = engine.status().value().tune_metadata[0U];
    R2_CHECK(after.revision == before.revision &&
             after.current_valid == before.current_valid &&
             after.last_successful_valid == before.last_successful_valid &&
             engine.status().value().operations[0U] == ReceiverTuneOperation::idle);
    return true;
}

bool test_cleanup_revokes_all_pending_before_blocking()
{
    Fixture f;
    ReceiverEngine engine(f.backend, f.ids.data(), f.ids.size(), f.tokens, f.clock);
    R2_CHECK(configure_strict(engine));
    R2_CHECK(engine.acquire(1U, kTerrestrial));
    R2_CHECK(engine.acquire(1U, kSatellite));
    // Both receivers first reach a valid current tune.
    auto first = engine.try_admit_tune(1U, terrestrial(1U), 0U);
    R2_CHECK(first && engine.execute_admitted_tune(first.value()));
    auto peer = engine.try_admit_tune(1U, satellite_slot(2U, 0U), 0U);
    R2_CHECK(peer && engine.execute_admitted_tune(peer.value()));
    R2_CHECK(engine.status().value().tune_metadata[0U].current_valid &&
             engine.status().value().tune_metadata[1U].current_valid);
    // Leave a pending ticket on the second receiver.
    const auto second = engine.try_admit_tune(1U, satellite_slot(2U, 0U), 0U);
    R2_CHECK(second);
    f.calls.clear();

    // Gate the first receiver's close so the second receiver's pending ticket
    // and both current tunes must already be handled by the cleanup metadata
    // phase before the first blocking close.
    f.backend.close_gate.arm(static_cast<int>(kTerrestrial));
    Result<void> cleanup = Result<void>::failure(Error::INTERNAL);
    std::thread cleaner([&]() { cleanup = engine.disconnect_client(1U); });
    const auto guard = on_scope_exit([&]() noexcept {
        f.backend.close_gate.open();
        if (cleaner.joinable()) cleaner.join();
    });
    R2_CHECK(f.backend.close_gate.wait_entered());
    const auto status = engine.status();
    R2_CHECK(status && !status.value().tune_metadata[0U].current_valid &&
             !status.value().tune_metadata[1U].current_valid);
    // Drop the in-flight first close; the second receiver must issue no I/O.
    f.calls.clear();
    R2_CHECK(failed(engine.execute_admitted_tune(second.value()), Error::NOT_FOUND));
    R2_CHECK(same_calls("second revoked", f.calls.take(), {}));
    return true;
}

bool test_current_invalidated_on_release_retains_history()
{
    Fixture f;
    ReceiverEngine engine(f.backend, f.ids.data(), f.ids.size(), f.tokens, f.clock);
    R2_CHECK(configure_strict(engine));
    R2_CHECK(engine.acquire(1U, kTerrestrial));
    auto admitted = engine.try_admit_tune(1U, terrestrial(1U), 0U);
    R2_CHECK(admitted && engine.execute_admitted_tune(admitted.value()));
    const auto tuned = engine.status().value().tune_metadata[0U];
    R2_CHECK(tuned.current_valid && tuned.last_successful_valid);

    R2_CHECK(engine.release(1U, 1U));
    const auto released = engine.status().value().tune_metadata[0U];
    R2_CHECK(!released.current_valid && released.last_successful_valid);

    R2_CHECK(engine.acquire(1U, kTerrestrial));
    const auto reacquired = engine.status().value().tune_metadata[0U];
    R2_CHECK(!reacquired.current_valid && reacquired.last_successful_valid &&
             reacquired.last_successful_revision == tuned.last_successful_revision);
    return true;
}

// Additive characterization of the OLD direct entrance: a satellite selection
// that returns success at/after the deadline still fails with TIMEOUT and the
// pending rollback (it must not silently succeed).
bool test_legacy_late_selection_expiry()
{
    Fixture f;
    ReceiverEngine engine(f.backend, f.ids.data(), f.ids.size(), f.tokens, f.clock);
    R2_CHECK(engine.acquire(1U, kSatellite));
    f.calls.clear();
    f.backend.select_advance_ms = 1000U;
    R2_CHECK(failed(engine.tune(1U, satellite_slot(1U, 0U)), Error::TIMEOUT));
    R2_CHECK(same_calls("legacy late select", f.calls.take(),
                        {"supports 513 2", "power 513 0", "tune 513 2 1049480 1000",
                         "lock 513", "slot 513 0 1000", "rollback 513"}));
    f.backend.select_advance_ms = 0U;
    return true;
}

// The initial clock callback is delayed by a finite gate; owner revocation then
// linearizes.  The pre-power registry transition recheck must issue no power.
bool test_prepower_registry_recheck()
{
    Fixture f;
    ReceiverEngine engine(f.backend, f.ids.data(), f.ids.size(), f.tokens, f.clock);
    R2_CHECK(configure_strict(engine));
    R2_CHECK(engine.acquire(1U, kTerrestrial));
    f.calls.clear();
    const auto admitted = engine.try_admit_tune(1U, terrestrial(1U), 0U);
    R2_CHECK(admitted);
    f.clock.gate_clock.store(true);
    f.clock.clock_gate.arm(0);
    Result<void> executed = Result<void>::failure(Error::INTERNAL);
    {
        std::thread worker(
            [&]() { executed = engine.execute_admitted_tune(admitted.value()); });
        const auto guard = on_scope_exit([&]() noexcept {
            f.clock.clock_gate.open();
            if (worker.joinable()) worker.join();
        });
        R2_CHECK(f.clock.clock_gate.wait_entered());
        R2_CHECK(engine.revoke_tune_ticket(1U, admitted.value().lease_id));
    }
    R2_CHECK(failed(executed, Error::NOT_FOUND));
    R2_CHECK(same_calls("no power after revoke", f.calls.take(), {}));
    return true;
}

// Initial-delay proof (not registry contention): a delayed first clock
// sample that leaves a positive remaining budget is still consumed, so the
// relative tune timeout reflects it.  Real registry-held contention is proven
// separately by test_registry_held_stage_budget / test_registry_held_final_deadline.
bool test_initial_delay_consumes_budget()
{
    Fixture f;
    ReceiverEngine engine(f.backend, f.ids.data(), f.ids.size(), f.tokens, f.clock);
    R2_CHECK(configure_strict(engine));
    R2_CHECK(engine.acquire(1U, kTerrestrial));
    const auto admitted = engine.try_admit_tune(1U, terrestrial(1U), 0U);
    R2_CHECK(admitted);
    f.calls.clear();
    f.clock.gate_clock.store(true);
    f.clock.clock_gate.arm(0);
    Result<void> executed = Result<void>::failure(Error::INTERNAL);
    {
        std::thread worker(
            [&]() { executed = engine.execute_admitted_tune(admitted.value()); });
        const auto guard = on_scope_exit([&]() noexcept {
            f.clock.clock_gate.open();
            if (worker.joinable()) worker.join();
        });
        R2_CHECK(f.clock.clock_gate.wait_entered());
        // Delay 300 ms of the 1000 ms budget before the first sample returns.
        f.clock.now.store(300U);
    }
    R2_CHECK(executed);
    R2_CHECK(same_calls("registry wait consumed", f.calls.take(),
                        {"power 17 0", "tune 17 1 557000 700", "lock 17", "commit 17"}));
    f.clock.now.store(0U);
    return true;
}

// A stale admitted ticket must not clear a replacement lease's active DIRECT
// tune marker through either stale execute or stale abort.
bool test_stale_ticket_vs_replacement_direct()
{
    Fixture f;
    ReceiverEngine engine(f.backend, f.ids.data(), f.ids.size(), f.tokens, f.clock);
    R2_CHECK(configure_strict(engine));
    R2_CHECK(engine.acquire(1U, kTerrestrial));
    const auto stale = engine.try_admit_tune(1U, terrestrial(1U), 0U);
    R2_CHECK(stale);
    R2_CHECK(engine.release(1U, 1U));
    R2_CHECK(engine.acquire(1U, kTerrestrial));
    f.calls.clear();

    f.backend.tune_gate.arm(static_cast<int>(kTerrestrial));
    Result<void> direct = Result<void>::failure(Error::INTERNAL);
    {
        std::thread worker([&]() { direct = engine.tune(1U, terrestrial(2U)); });
        const auto guard = on_scope_exit([&]() noexcept {
            f.backend.tune_gate.open();
            if (worker.joinable()) worker.join();
        });
        R2_CHECK(f.backend.tune_gate.wait_entered());
        // Stale execute and stale abort must both leave the replacement's
        // direct tuning marker active.
        R2_CHECK(failed(engine.execute_admitted_tune(stale.value()), Error::NOT_FOUND));
        R2_CHECK(failed(engine.try_admit_tune(1U, terrestrial(2U), 0U), Error::BUSY));
        R2_CHECK(engine.abort_unstarted_tune(stale.value(), Error::INTERNAL));
        R2_CHECK(failed(engine.try_admit_tune(1U, terrestrial(2U), 0U), Error::BUSY));
        const auto status = engine.status();
        R2_CHECK(status &&
                 status.value().operations[0U] == ReceiverTuneOperation::tuning);
    }
    R2_CHECK(direct);
    return true;
}

// Actual owner revoked after a completed tune (pending empty) invalidates the
// current metadata, while a wrong-owner revoke does not.
bool test_revoke_invalidates_current_and_wrong_owner()
{
    Fixture f;
    ReceiverEngine engine(f.backend, f.ids.data(), f.ids.size(), f.tokens, f.clock);
    R2_CHECK(configure_strict(engine));
    R2_CHECK(engine.acquire(1U, kTerrestrial));
    auto admitted = engine.try_admit_tune(1U, terrestrial(1U), 0U);
    R2_CHECK(admitted && engine.execute_admitted_tune(admitted.value()));
    R2_CHECK(engine.status().value().tune_metadata[0U].current_valid);

    // A different client/lease must not invalidate this owner's current tune.
    R2_CHECK(engine.revoke_tune_ticket(2U, 1U));
    R2_CHECK(engine.status().value().tune_metadata[0U].current_valid);

    // The exact owner's logical teardown revokes current and keeps history.
    R2_CHECK(engine.revoke_tune_ticket(1U, 1U));
    R2_CHECK(!engine.status().value().tune_metadata[0U].current_valid);
    R2_CHECK(engine.status().value().tune_metadata[0U].last_successful_valid);
    return true;
}

// Release is already waiting behind an in-flight consumed commit; the commit
// succeeds, the owner was revoked, and the close then fails.  The receiver
// stays excluded (strict) without a post-commit rollback.
bool test_release_waiting_consumed_commit_close_failure()
{
    Fixture f;
    ReceiverEngine engine(f.backend, f.ids.data(), f.ids.size(), f.tokens, f.clock);
    R2_CHECK(configure_strict(engine));
    R2_CHECK(engine.acquire(1U, kTerrestrial));
    auto admitted = engine.try_admit_tune(1U, terrestrial(1U), 0U);
    R2_CHECK(admitted);
    f.calls.clear();
    f.backend.consume_power_transaction = true;
    f.backend.close_error = Error::USB_IO;
    f.backend.commit_gate.arm(static_cast<int>(kTerrestrial));

    Result<void> executed = Result<void>::failure(Error::INTERNAL);
    Result<void> released = Result<void>::failure(Error::INTERNAL);
    std::thread worker;
    std::thread releaser;
    bool cleanup_reached = false;
    {
        const auto guard = on_scope_exit([&]() noexcept {
            f.backend.commit_gate.open();
            if (worker.joinable()) worker.join();
            if (releaser.joinable()) releaser.join();
        });
        worker = std::thread(
            [&]() { executed = engine.execute_admitted_tune(admitted.value()); });
        R2_CHECK(f.backend.commit_gate.wait_entered());
        // Release linearizes its first phase (revoke current + pending, mark
        // cleanup) while the commit is in flight, then waits on the receiver.
        releaser = std::thread([&]() { released = engine.release(1U, 1U); });
        // Deterministically wait until release's metadata phase has run.
        for (int spin = 0; spin < 2000000; ++spin) {
            const auto probe = engine.status();
            if (probe && probe.value().operations[0U] == ReceiverTuneOperation::cleanup) {
                cleanup_reached = true;
                break;
            }
            std::this_thread::yield();
        }
        R2_CHECK(cleanup_reached);
    }
    R2_CHECK(failed(executed, Error::NOT_FOUND));
    R2_CHECK(!released && released.error() == Error::USB_IO);
    // The consumed commit is never rolled back; only the failed close runs.
    R2_CHECK(same_calls("consumed commit close", f.calls.take(),
                        {"power 17 0", "tune 17 1 557000 1000", "lock 17", "commit 17",
                         "close 17"}));
    const auto status = engine.status();
    R2_CHECK(status && !status.value().tune_metadata[0U].current_valid &&
             status.value().states[0U] == ReceiverState::error);
    f.backend.consume_power_transaction = false;
    f.backend.close_error = Error::OK;
    return true;
}

// A direct tune that returns while logical cleanup has linearized must not
// erase the cleanup marker or publish reusable/tuned success, and START/ATTACH
// must not proceed while cleanup remains in flight.
bool test_direct_completion_preserves_cleanup()
{
    Fixture f;
    ReceiverEngine engine(f.backend, f.ids.data(), f.ids.size(), f.tokens, f.clock);
    R2_CHECK(configure_strict(engine));
    R2_CHECK(engine.acquire(1U, kTerrestrial));
    f.calls.clear();

    f.backend.tune_gate.arm(static_cast<int>(kTerrestrial));
    // Also gate the release close so the marker can be inspected after the
    // direct tune has finished and returned.
    f.backend.close_gate.arm(static_cast<int>(kTerrestrial));
    Result<void> direct = Result<void>::failure(Error::INTERNAL);
    Result<void> released = Result<void>::failure(Error::INTERNAL);
    std::thread worker;
    std::thread releaser;
    bool cleanup_reached = false;
    bool marker_survived = false;
    {
        const auto guard = on_scope_exit([&]() noexcept {
            f.backend.tune_gate.open();
            f.backend.close_gate.open();
            if (worker.joinable()) worker.join();
            if (releaser.joinable()) releaser.join();
        });
        worker = std::thread([&]() { direct = engine.tune(1U, terrestrial(1U)); });
        R2_CHECK(f.backend.tune_gate.wait_entered());
        // Release linearizes cleanup while the direct tune holds the receiver.
        releaser = std::thread([&]() { released = engine.release(1U, 1U); });
        for (int spin = 0; spin < 2000000; ++spin) {
            const auto probe = engine.status();
            if (probe && probe.value().operations[0U] == ReceiverTuneOperation::cleanup) {
                cleanup_reached = true;
                break;
            }
            std::this_thread::yield();
        }
        R2_CHECK(cleanup_reached);
        // While the direct tune is still gated the lease exists, so strict
        // admission is BUSY.
        R2_CHECK(failed(engine.try_admit_tune(1U, terrestrial(1U), 0U), Error::BUSY));
        // Finish the direct tune; its completion must not clear cleanup.
        f.backend.tune_gate.open();
        if (worker.joinable()) worker.join();
        // Release's close is blocked, so the marker can be inspected after the
        // direct tune returned.
        const auto probe = engine.status();
        marker_survived = probe &&
            probe.value().operations[0U] == ReceiverTuneOperation::cleanup &&
            probe.value().states[0U] != ReceiverState::tuned;
        R2_CHECK(marker_survived);
        // The receiver stays excluded after direct completion (the lease is
        // already cleared by release, so admission/START fail without reaching
        // a new hardware or capture path).
        R2_CHECK(!engine.try_admit_tune(1U, terrestrial(1U), 0U));
        R2_CHECK(!engine.start_stream(1U, 1U));
    }
    R2_CHECK(cleanup_reached && marker_survived);
    R2_CHECK(failed(direct, Error::NOT_FOUND));
    R2_CHECK(released);
    // No direct success re-publication and no capture I/O.
    for (const std::string& call : f.calls.take()) {
        R2_CHECK(call.rfind("start ", 0U) != 0U && call.rfind("attach ", 0U) != 0U);
    }
    return true;
}

// An admitted false lock query whose result passes its gate, with owner
// revocation during the inter-poll sleep, must not issue a second query.
bool test_poll_sleep_cancellation()
{
    Fixture f;
    ReceiverEngine engine(f.backend, f.ids.data(), f.ids.size(), f.tokens, f.clock);
    R2_CHECK(configure_strict(engine));
    R2_CHECK(engine.acquire(1U, kTerrestrial));
    const auto admitted = engine.try_admit_tune(1U, terrestrial(1U), 0U);
    R2_CHECK(admitted);
    f.calls.clear();
    f.backend.lock_locked = false;
    f.clock.gate_sleep.store(true);
    f.clock.sleep_gate.arm(0);
    Result<void> executed = Result<void>::failure(Error::INTERNAL);
    {
        std::thread worker(
            [&]() { executed = engine.execute_admitted_tune(admitted.value()); });
        const auto guard = on_scope_exit([&]() noexcept {
            f.clock.sleep_gate.open();
            if (worker.joinable()) worker.join();
        });
        R2_CHECK(f.clock.sleep_gate.wait_entered());
        R2_CHECK(engine.revoke_tune_ticket(1U, admitted.value().lease_id));
    }
    R2_CHECK(failed(executed, Error::NOT_FOUND));
    R2_CHECK(same_calls("sleep cancelled", f.calls.take(),
                        {"power 17 0", "tune 17 1 557000 1000", "lock 17", "sleep 10",
                         "rollback 17"}));
    f.backend.lock_locked = true;
    f.clock.gate_sleep.store(false);
    return true;
}

// Budget expiry during the inter-poll sleep also stops before a second query.
bool test_poll_sleep_expiry()
{
    Fixture f;
    ReceiverEngine engine(f.backend, f.ids.data(), f.ids.size(), f.tokens, f.clock);
    R2_CHECK(configure_strict(engine));
    R2_CHECK(engine.acquire(1U, kTerrestrial));
    f.calls.clear();
    f.backend.lock_locked = false;
    TuneRequest request = terrestrial(1U);
    request.timeout_ms = 10U;
    const auto admitted = engine.try_admit_tune(1U, request, 0U);
    R2_CHECK(admitted);
    R2_CHECK(failed(engine.execute_admitted_tune(admitted.value()), Error::TIMEOUT));
    R2_CHECK(same_calls("sleep expired", f.calls.take(),
                        {"power 17 0", "tune 17 1 557000 10", "lock 17", "sleep 10",
                         "rollback 17"}));
    f.backend.lock_locked = true;
    return true;
}

// Real registry contention: hold the registry from a concurrent ACQUIRE while
// an admitted execution waits at its stage gate, advance time, then release.
bool test_registry_held_stage_budget()
{
    Calls calls;
    FakeClock clock(calls);
    FakeBackend backend(calls, clock);
    GatedTokens tokens;
    const std::array<ReceiverId, 2> ids{kTerrestrial, kSatellite};
    ReceiverEngine engine(backend, ids.data(), ids.size(), tokens, clock);
    R2_CHECK(configure_strict(engine));
    R2_CHECK(engine.acquire(1U, kTerrestrial));
    const auto admitted = engine.try_admit_tune(1U, terrestrial(1U), 0U);
    R2_CHECK(admitted);
    calls.clear();

    tokens.gate.arm(0);
    Result<AcquireResult> other = Result<AcquireResult>::failure(Error::INTERNAL);
    Result<void> executed = Result<void>::failure(Error::INTERNAL);
    std::thread accepter;
    std::thread worker;
    {
        const auto guard = on_scope_exit([&]() noexcept {
            tokens.gate.open();
            if (accepter.joinable()) accepter.join();
            if (worker.joinable()) worker.join();
        });
        accepter = std::thread([&]() { other = engine.acquire(2U, kSatellite); });
        R2_CHECK(tokens.gate.wait_entered());
        worker = std::thread(
            [&]() { executed = engine.execute_admitted_tune(admitted.value()); });
        // Let the execution block on the held registry, then advance 300 ms.
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        clock.now.store(300U);
    }
    R2_CHECK(executed);
    R2_CHECK(same_calls("registry held budget", calls.take_for(kTerrestrial),
                        {"power 17 0", "tune 17 1 557000 700", "lock 17", "commit 17"}));
    R2_CHECK(other);
    return true;
}

// Real registry contention at the final publication gate: the consumed commit
// succeeds, the registry is held while time expires, then the late success is
// quarantined with no rollback.
bool test_registry_held_final_deadline()
{
    Calls calls;
    FakeClock clock(calls);
    FakeBackend backend(calls, clock);
    GatedTokens tokens;
    const std::array<ReceiverId, 2> ids{kTerrestrial, kSatellite};
    ReceiverEngine engine(backend, ids.data(), ids.size(), tokens, clock);
    R2_CHECK(configure_strict(engine));
    R2_CHECK(engine.acquire(1U, kTerrestrial));
    const auto admitted = engine.try_admit_tune(1U, terrestrial(1U), 0U);
    R2_CHECK(admitted);
    backend.consume_power_transaction = true;
    calls.clear();

    backend.commit_gate.arm(static_cast<int>(kTerrestrial));
    Result<AcquireResult> other = Result<AcquireResult>::failure(Error::INTERNAL);
    Result<void> executed = Result<void>::failure(Error::INTERNAL);
    std::thread worker;
    std::thread accepter;
    {
        const auto guard = on_scope_exit([&]() noexcept {
            backend.commit_gate.open();
            tokens.gate.open();
            if (worker.joinable()) worker.join();
            if (accepter.joinable()) accepter.join();
        });
        worker = std::thread(
            [&]() { executed = engine.execute_admitted_tune(admitted.value()); });
        R2_CHECK(backend.commit_gate.wait_entered());
        // Hold the registry while the consumed commit returns.
        tokens.gate.arm(0);
        accepter = std::thread([&]() { other = engine.acquire(2U, kSatellite); });
        R2_CHECK(tokens.gate.wait_entered());
        clock.now.store(2000U);
        backend.commit_gate.open();
    }
    R2_CHECK(failed(executed, Error::TIMEOUT));
    R2_CHECK(same_calls("registry held final", calls.take_for(kTerrestrial),
                        {"power 17 0", "tune 17 1 557000 1000", "lock 17", "commit 17"}));
    const auto status = engine.status();
    R2_CHECK(status && !status.value().tune_metadata[0U].current_valid &&
             status.value().states[0U] == ReceiverState::error);
    backend.consume_power_transaction = false;
    return true;
}

#if defined(DTV_R2_LOCK_OBSERVER)
bool wait_flag(const std::atomic<bool>& flag)
{
    const auto deadline = std::chrono::steady_clock::now() + kWaitBound;
    while (!flag.load()) {
        if (std::chrono::steady_clock::now() >= deadline) return false;
        std::this_thread::yield();
    }
    return true;
}

// A token source that blocks inside generate() while the holder thread owns the
// real registry, and publishes the registry mutex address (thread-local
// last-acquired) for the observer.
class HolderTokens final : public ReceiverTokenSource {
public:
    Result<ReceiverToken> generate() noexcept override
    {
        if (!block.load()) {
            ReceiverToken token{};
            token[0U] = next.fetch_add(1U);
            return Result<ReceiverToken>::success(token);
        }
        g_r2_lock_observer.target.store(g_r2_last_acquired, std::memory_order_release);
        entered.store(true, std::memory_order_release);
        const auto deadline = std::chrono::steady_clock::now() + kObserverBound;
        while (!release.load(std::memory_order_acquire)) {
            if (std::chrono::steady_clock::now() >= deadline) {
                holder_timed_out.store(true, std::memory_order_release);
                break;
            }
            std::this_thread::yield();
        }
        ReceiverToken token{};
        token[0U] = next.fetch_add(1U);
        return Result<ReceiverToken>::success(token);
    }

    std::atomic<bool> block{false};
    std::atomic<bool> entered{false};
    std::atomic<bool> release{false};
    std::atomic<bool> holder_timed_out{false};
    std::atomic<std::uint8_t> next{1U};
};

// Identify the engine registry mutex address through a preliminary holder
// ACQUIRE, then release that receiver again.
bool identify_registry(ReceiverEngine& engine, HolderTokens& tokens)
{
    tokens.block.store(true);
    Result<AcquireResult> lease = Result<AcquireResult>::failure(Error::INTERNAL);
    bool entered = false;
    {
        std::thread identifier([&]() {
            g_r2_role = 1;
            lease = engine.acquire(2U, kSatellite);
            g_r2_role = 0;
        });
        entered = wait_flag(tokens.entered);
        tokens.release.store(true);
        identifier.join();
    }
    const bool timed_out = tokens.holder_timed_out.load();
    tokens.block.store(false);
    tokens.entered.store(false);
    tokens.release.store(false);
    tokens.holder_timed_out.store(false);
    if (lease) (void)engine.release(2U, lease.value().lease_id);
    return entered && !timed_out && g_r2_lock_observer.target.load() != nullptr;
}

// Test A: the registry is held while the worker waits at the final pre-tune
// gate; time advances while that gate cannot pass, so the frontend must receive
// the reduced positive remainder (600), not a value cached before the gate.
bool test_registry_lock_stage_budget()
{
    Calls calls;
    FakeClock clock(calls);
    FakeBackend backend(calls, clock);
    HolderTokens tokens;
    const std::array<ReceiverId, 2> ids{kTerrestrial, kSatellite};
    ReceiverEngine engine(backend, ids.data(), ids.size(), tokens, clock);
    R2_CHECK(configure_strict(engine));
    R2_CHECK(engine.acquire(1U, kTerrestrial));
    const auto admitted = engine.try_admit_tune(1U, terrestrial(1U), 0U);
    R2_CHECK(admitted);
    g_r2_lock_observer.reset();
    g_r2_lock_observer.enabled.store(true, std::memory_order_release);
    R2_CHECK(identify_registry(engine, tokens));
    calls.clear();

    clock.now.store(100U);
    clock.worker_known = false;
    clock.stop_sample = 3;
    clock.worker_samples.store(0);
    clock.gate_sample_active = true;
    clock.sample_gate.arm(0);
    g_r2_lock_observer.pause_before_real.store(true, std::memory_order_release);

    Result<void> executed = Result<void>::failure(Error::INTERNAL);
    std::thread worker;
    std::thread holder;
    bool stage_success = false;
    {
        const auto guard = on_scope_exit([&]() noexcept {
            clock.sample_gate.open();
            g_r2_lock_observer.pause_before_real.store(false, std::memory_order_release);
            tokens.release.store(true);
            if (worker.joinable()) worker.join();
            if (holder.joinable()) holder.join();
        });
        worker = std::thread([&]() {
            g_r2_role = 2;
            clock.worker_thread = pthread_self();
            clock.worker_known = true;
            executed = engine.execute_admitted_tune(admitted.value());
            g_r2_role = 0;
        });
        R2_CHECK(clock.sample_gate.wait_entered());
        g_r2_lock_observer.observe_worker.store(true, std::memory_order_release);
        tokens.block.store(true);
        holder = std::thread([&]() {
            g_r2_role = 1;
            (void)engine.acquire(2U, kSatellite);
            g_r2_role = 0;
        });
        clock.sample_gate.open();
        R2_CHECK(wait_flag(g_r2_lock_observer.requested));
        R2_CHECK(wait_flag(tokens.entered));
        R2_CHECK(!g_r2_lock_observer.acquired.load());
        clock.now.store(400U);
        g_r2_lock_observer.pause_before_real.store(false, std::memory_order_release);
        tokens.release.store(true);
        stage_success = true;
    }
    // Read the timeout flags only after the scope guard has joined all threads,
    // so a late timeout cannot be missed.
    const bool timed_out = g_r2_lock_observer.observer_timed_out.load() ||
                           tokens.holder_timed_out.load();
    R2_CHECK(stage_success);
    R2_CHECK(!timed_out);
    R2_CHECK(executed);
    R2_CHECK(same_calls("observer stage budget", calls.take_for(kTerrestrial),
                        {"power 17 0", "tune 17 1 557000 600", "lock 17", "commit 17"}));
    g_r2_lock_observer.reset();
    return true;
}

// Test B: the consumed commit returns while in-budget and the registry is held;
// the worker then waits to acquire the final registry lock, time expires, and
// only then is it released.  The late committed success is quarantined.
bool test_registry_lock_final_deadline()
{
    Calls calls;
    FakeClock clock(calls);
    FakeBackend backend(calls, clock);
    HolderTokens tokens;
    const std::array<ReceiverId, 2> ids{kTerrestrial, kSatellite};
    ReceiverEngine engine(backend, ids.data(), ids.size(), tokens, clock);
    R2_CHECK(configure_strict(engine));
    R2_CHECK(engine.acquire(1U, kTerrestrial));
    const auto admitted = engine.try_admit_tune(1U, terrestrial(1U), 0U);
    R2_CHECK(admitted);
    g_r2_lock_observer.reset();
    g_r2_lock_observer.enabled.store(true, std::memory_order_release);
    R2_CHECK(identify_registry(engine, tokens));
    backend.consume_power_transaction = true;
    calls.clear();
    clock.now.store(100U);
    backend.commit_gate.arm(static_cast<int>(kTerrestrial));
    g_r2_lock_observer.pause_before_real.store(true, std::memory_order_release);

    Result<void> executed = Result<void>::failure(Error::INTERNAL);
    std::thread worker;
    std::thread holder;
    bool final_success = false;
    {
        const auto guard = on_scope_exit([&]() noexcept {
            backend.commit_gate.open();
            g_r2_lock_observer.pause_before_real.store(false, std::memory_order_release);
            tokens.release.store(true);
            if (worker.joinable()) worker.join();
            if (holder.joinable()) holder.join();
        });
        worker = std::thread([&]() {
            g_r2_role = 2;
            executed = engine.execute_admitted_tune(admitted.value());
            g_r2_role = 0;
        });
        R2_CHECK(backend.commit_gate.wait_entered());
        g_r2_lock_observer.observe_worker.store(true, std::memory_order_release);
        tokens.block.store(true);
        holder = std::thread([&]() {
            g_r2_role = 1;
            (void)engine.acquire(2U, kSatellite);
            g_r2_role = 0;
        });
        R2_CHECK(wait_flag(tokens.entered));
        backend.commit_gate.open();
        R2_CHECK(wait_flag(g_r2_lock_observer.requested));
        R2_CHECK(!g_r2_lock_observer.acquired.load());
        clock.now.store(1100U);
        g_r2_lock_observer.pause_before_real.store(false, std::memory_order_release);
        tokens.release.store(true);
        final_success = true;
    }
    // Read the timeout flags only after the scope guard has joined all threads.
    const bool timed_out = g_r2_lock_observer.observer_timed_out.load() ||
                           tokens.holder_timed_out.load();
    R2_CHECK(final_success);
    R2_CHECK(!timed_out);
    R2_CHECK(failed(executed, Error::TIMEOUT));
    R2_CHECK(same_calls("observer final deadline", calls.take_for(kTerrestrial),
                        {"power 17 0", "tune 17 1 557000 900", "lock 17", "commit 17"}));
    const auto status = engine.status();
    R2_CHECK(status && !status.value().tune_metadata[0U].current_valid &&
             status.value().states[0U] == ReceiverState::error);
    backend.consume_power_transaction = false;
    g_r2_lock_observer.reset();
    return true;
}
#endif  // DTV_R2_LOCK_OBSERVER

}  // namespace

int main()
{
    return test_strict_setup_validation() && test_admission_is_callback_free() &&
                   test_registry_contention_and_gated_callbacks() &&
                   test_expired_and_power_late() &&
                   test_budget_is_not_restarted_and_commit_is_gated() &&
                   test_postcommit_cancellation_quarantines() &&
                   test_precommit_deadline_boundaries() &&
                   test_cancellation_during_gated_tune() &&
                   test_stale_ticket_and_revision_monotonic() &&
                   test_legacy_active_operation_blocks_admission() &&
                   test_revision_exhaustion_fails_safely() &&
                   test_cleanup_revokes_all_pending_before_blocking() &&
                   test_current_invalidated_on_release_retains_history() &&
                   test_legacy_late_selection_expiry() &&
                   test_prepower_registry_recheck() &&
                   test_initial_delay_consumes_budget() &&
                   test_stale_ticket_vs_replacement_direct() &&
                   test_revoke_invalidates_current_and_wrong_owner() &&
                   test_release_waiting_consumed_commit_close_failure() &&
                   test_direct_completion_preserves_cleanup() &&
                   test_poll_sleep_cancellation() &&
                   test_poll_sleep_expiry() &&
                   test_registry_held_stage_budget() &&
                   test_registry_held_final_deadline() &&
#if defined(DTV_R2_LOCK_OBSERVER)
                   test_registry_lock_stage_budget() &&
                   test_registry_lock_final_deadline() &&
#endif
                   test_precedence_and_unknown_system() &&
                   test_legacy_entry_respects_pending_ticket()
               ? 0
               : 1;
}
