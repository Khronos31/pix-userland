// SPDX-License-Identifier: MIT
#include "dtv/userland/detail/receiver_engine.h"

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <initializer_list>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace dtv::userland;
using namespace dtv::userland::detail;

#define ENGINE_CHECK(condition)                                                       \
    do {                                                                               \
        if (!(condition)) {                                                            \
            std::fprintf(stderr, "receiver engine check failed at line %d: %s\n",     \
                         __LINE__, #condition);                                       \
            return false;                                                              \
        }                                                                              \
    } while (false)

// Bound for every cross-thread wait.  Gates order the threads; a wait that
// runs out records a failure and lets its thread continue, so a regression
// ends the test instead of hanging it.  CTest's TIMEOUT is the outer bound.
constexpr std::chrono::milliseconds kWaitBound{10000};
// Shorter bound for the deliberately serialized experiment, which is expected
// to run out.
constexpr std::chrono::milliseconds kExpectedTimeout{200};
constexpr int kNoReceiver = -1;

// Sparse, non-contiguous identities.  513 is satellite-only; every other ID
// is terrestrial-only.
constexpr ReceiverId kTerrestrial = 17U;
constexpr ReceiverId kSatellite = 513U;
constexpr ReceiverId kSecondTerrestrial = 4097U;

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

    std::vector<std::string> for_receiver(int receiver)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<std::string> texts;
        for (const Entry& entry : entries_) {
            if (entry.receiver == receiver) texts.push_back(entry.text);
        }
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

Result<void> take_result(std::deque<Error>& failures) noexcept
{
    if (failures.empty()) return Result<void>::success();
    const Error error = failures.front();
    failures.pop_front();
    return error == Error::OK ? Result<void>::success() : Result<void>::failure(error);
}

// A one-shot rendezvous: the operation thread enters and waits until the test
// opens the gate or the bound runs out.
class Gate final {
public:
    void arm(int receiver)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        receiver_ = receiver;
        entered_ = false;
        open_ = false;
        timed_out_ = false;
    }

    void pass(int receiver)
    {
        std::unique_lock<std::mutex> lock(mutex_);
        if (receiver_ != receiver) return;
        receiver_ = kNoReceiver;
        entered_ = true;
        changed_.notify_all();
        if (!changed_.wait_for(lock, kWaitBound, [&]() noexcept { return open_; }))
            timed_out_ = true;
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

    bool timed_out()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return timed_out_;
    }

private:
    std::mutex mutex_;
    std::condition_variable changed_;
    int receiver_ = kNoReceiver;
    bool entered_ = false;
    bool open_ = false;
    bool timed_out_ = false;
};

class Done final {
public:
    void set()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        done_ = true;
        changed_.notify_all();
    }

    bool wait(std::chrono::milliseconds bound)
    {
        std::unique_lock<std::mutex> lock(mutex_);
        return changed_.wait_for(lock, bound, [&]() noexcept { return done_; });
    }

private:
    std::mutex mutex_;
    std::condition_variable changed_;
    bool done_ = false;
};

// Runs `held` on one thread until it reaches the armed gate, then `probe` on a
// second thread, and waits a bounded time for the probe to finish while the
// first operation is still held.  The gate is always opened and both threads
// joined, so a probe that cannot finish (for example because the operations
// were serialized) fails the check instead of hanging the process.
template <typename Held, typename Probe>
bool probe_while_held(Gate& gate, int receiver, Held held, Probe probe,
                      std::chrono::milliseconds bound = kWaitBound)
{
    gate.arm(receiver);
    std::thread holder(held);
    const bool entered = gate.wait_entered();
    Done probed;
    std::thread prober([&]() {
        probe();
        probed.set();
    });
    const bool finished = probed.wait(bound);
    gate.open();
    prober.join();
    holder.join();
    if (!entered) std::fprintf(stderr, "held operation did not reach its gate\n");
    if (!finished) std::fprintf(stderr, "probe did not finish while the operation was held\n");
    return entered && finished && !gate.timed_out();
}

class FakeClock final : public ReceiverClock {
public:
    explicit FakeClock(Calls& calls) noexcept : calls_(calls) {}

    std::uint64_t monotonic_ms() noexcept override { return now.load(); }

    void sleep_ms(std::uint32_t milliseconds) noexcept override
    {
        calls_.add(kNoReceiver, "sleep " + num(milliseconds));
        now.fetch_add(milliseconds);
    }

    std::atomic<std::uint64_t> now{0U};

private:
    Calls& calls_;
};

class FakeBackend final : public ReceiverBackend {
public:
    FakeBackend(Calls& calls, ReceiverClock& clock) noexcept : calls_(calls), clock_(clock) {}

    bool supports(ReceiverId receiver, ReceiverSystem system) const noexcept override
    {
        calls_.add(receiver, "supports " + num(receiver) + " " + sys(system));
        std::unique_lock<std::mutex> serial(serial_, std::defer_lock);
        if (serialize) serial.lock();
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
        std::unique_lock<std::mutex> serial(serial_, std::defer_lock);
        if (serialize) serial.lock();
        tune_gate.pass(receiver);
        return Result<void>::success();
    }

    Result<bool> is_locked(ReceiverId receiver, ReceiverSystem) noexcept override
    {
        calls_.add(receiver, "lock " + num(receiver));
        return Result<bool>::success(true);
    }

    Result<void> select_satellite_slot(ReceiverId receiver, std::uint8_t slot,
                                       std::uint32_t timeout_ms) noexcept override
    {
        calls_.add(receiver, "slot " + num(receiver) + " " + num(slot) + " " +
                                 num(timeout_ms));
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
        return Result<void>::success();
    }

    Result<void> begin_tune_power(ReceiverId receiver, ReceiverSystem,
                                  std::uint8_t lnb_voltage) noexcept override
    {
        calls_.add(receiver, "power " + num(receiver) + " " + num(lnb_voltage));
        if (power_wait_ms != 0U) clock_.sleep_ms(power_wait_ms);
        return Result<void>::success();
    }

    Result<void> commit_tune_power(ReceiverId receiver) noexcept override
    {
        calls_.add(receiver, "commit " + num(receiver));
        return Result<void>::success();
    }

    Result<void> rollback_tune_power(ReceiverId receiver) noexcept override
    {
        calls_.add(receiver, "rollback " + num(receiver));
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

    std::uint32_t settle_ms = 0U;
    std::uint32_t power_wait_ms = 0U;
    // Test-only regression model: one lock across every receiver's support
    // query and frontend tune, as if separate receivers were serialized.
    bool serialize = false;
    mutable Gate supports_gate;
    Gate tune_gate;

private:
    Calls& calls_;
    ReceiverClock& clock_;
    mutable std::mutex serial_;
};

class FakeStream final : public ReceiverStreamControl {
public:
    explicit FakeStream(Calls& calls) noexcept : calls_(calls) {}

    Result<void> attach(const Attachment& attachment) noexcept override
    {
        calls_.add(attachment.receiver, "attach " + identity(attachment));
        return Result<void>::success();
    }

    Result<void> detach(const Attachment& attachment) noexcept override
    {
        calls_.add(attachment.receiver, "detach " + identity(attachment));
        std::lock_guard<std::mutex> lock(mutex_);
        return take_result(detach_failures);
    }

    Result<StreamCounters> stats(const Attachment& attachment) const noexcept override
    {
        calls_.add(attachment.receiver, "stats " + identity(attachment));
        StreamCounters counters{};
        counters.packets = attachment.attachment_id * 10U;
        return Result<StreamCounters>::success(counters);
    }

    Result<StreamFinalSnapshot> final_snapshot(
        const Attachment& attachment) const noexcept override
    {
        calls_.add(attachment.receiver, "final " + identity(attachment));
        StreamFinalSnapshot snapshot{};
        snapshot.counters.packets = attachment.attachment_id;
        return Result<StreamFinalSnapshot>::success(snapshot);
    }

    static std::string identity(const Attachment& attachment)
    {
        return num(attachment.receiver) + " " + num(attachment.lease_id) + " " +
               num(attachment.attachment_id);
    }

    std::deque<Error> detach_failures;

private:
    Calls& calls_;
    std::mutex mutex_;
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

struct Fixture final {
    Calls calls;
    FakeClock clock{calls};
    FakeBackend backend{calls, clock};
    FakeStream stream{calls};
    CountingTokens tokens;
    std::array<ReceiverId, 3> ids{kTerrestrial, kSatellite, kSecondTerrestrial};
};

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

bool state_at(const ReceiverEngine& engine, std::size_t slot, ReceiverState state)
{
    const auto status = engine.status();
    return status && status.value().states[slot] == state;
}

template <typename T>
bool failed(const Result<T>& result, Error expected)
{
    return !result && result.error() == expected;
}

bool test_sparse_ids_through_lifecycle()
{
    Fixture f;
    ReceiverEngine engine(f.backend, f.ids.data(), 2U, f.tokens, f.clock, nullptr, nullptr,
                          &f.stream);
    ENGINE_CHECK(engine.configured());
    const auto initial = engine.status();
    ENGINE_CHECK(initial && initial.value().receiver_count == 2U &&
                 initial.value().receivers[0U] == kTerrestrial &&
                 initial.value().receivers[1U] == kSatellite &&
                 initial.value().generation == 1U);

    const auto terrestrial_lease = engine.acquire(1U, kTerrestrial);
    ENGINE_CHECK(terrestrial_lease && terrestrial_lease.value().lease_id == 1U);
    ENGINE_CHECK(same_calls("acquire 17", f.calls.take(), {"open 17", "supports 17 2"}));
    const auto satellite_lease = engine.acquire(2U, kSatellite);
    ENGINE_CHECK(satellite_lease && satellite_lease.value().lease_id == 2U);
    ENGINE_CHECK(same_calls("acquire 513", f.calls.take(),
                            {"open 513", "supports 513 2", "supports 513 1"}));
    ENGINE_CHECK(failed(engine.acquire(3U, kSatellite), Error::BUSY));

    ENGINE_CHECK(engine.tune(1U, terrestrial(1U)));
    ENGINE_CHECK(same_calls("tune 17", f.calls.take(),
                            {"supports 17 1", "power 17 0", "tune 17 1 557000 1000",
                             "lock 17", "commit 17"}));
    ENGINE_CHECK(engine.tune(2U, satellite_slot(2U, 3U)));
    ENGINE_CHECK(same_calls("tune 513", f.calls.take(),
                            {"supports 513 2", "power 513 0", "tune 513 2 1049480 1000",
                             "lock 513", "slot 513 3 1000", "commit 513"}));

    ENGINE_CHECK(engine.start_stream(1U, 1U) && engine.start_stream(2U, 2U));
    const auto first = engine.attach_stream(1U, terrestrial_lease.value().token);
    const auto second = engine.attach_stream(2U, satellite_lease.value().token);
    ENGINE_CHECK(first && first.value().receiver == kTerrestrial &&
                 first.value().attachment_id == 1U &&
                 first.value().system == ReceiverSystem::terrestrial);
    ENGINE_CHECK(second && second.value().receiver == kSatellite &&
                 second.value().attachment_id == 2U &&
                 second.value().system == ReceiverSystem::satellite &&
                 second.value().owner_client_id == 2U);
    ENGINE_CHECK(same_calls("attach", f.calls.take(),
                            {"start 17", "attach 17 1 1", "start 513", "attach 513 2 2"}));
    ENGINE_CHECK(state_at(engine, 0U, ReceiverState::streaming) &&
                 state_at(engine, 1U, ReceiverState::streaming));

    const auto stats = engine.stats(2U, 2U);
    ENGINE_CHECK(stats && stats.value().packets == 20U);
    ENGINE_CHECK(failed(engine.stats(1U, 2U), Error::NOT_FOUND));
    const auto stopped = engine.stop_stream(1U, 1U);
    ENGINE_CHECK(stopped && stopped.value().counters.packets == 1U);
    ENGINE_CHECK(engine.detach_stream(second.value()));
    ENGINE_CHECK(same_calls("stop", f.calls.take(),
                            {"stats 513 2 2", "detach 17 1 1", "stop 17", "final 17 1 1",
                             "detach 513 2 2", "stop 513"}));
    ENGINE_CHECK(state_at(engine, 0U, ReceiverState::tuned) &&
                 state_at(engine, 1U, ReceiverState::tuned));

    ENGINE_CHECK(engine.release(1U, 1U));
    ENGINE_CHECK(engine.disconnect_client(2U));
    ENGINE_CHECK(engine.shutdown());
    ENGINE_CHECK(same_calls("cleanup", f.calls.take(), {"close 17", "close 513", "shutdown"}));
    ENGINE_CHECK(state_at(engine, 0U, ReceiverState::free) &&
                 state_at(engine, 1U, ReceiverState::free));
    return true;
}

bool test_invalid_missing_ids_and_capacity()
{
    Fixture f;
    {
        ReceiverEngine engine(f.backend, f.ids.data(), 2U, f.tokens, f.clock);
        for (const ReceiverId missing : {ReceiverId{0U}, ReceiverId{16U}, ReceiverId{18U},
                                         ReceiverId{512U}, ReceiverId{514U},
                                         kSecondTerrestrial, ReceiverId{0xffffU}}) {
            ENGINE_CHECK(failed(engine.acquire(1U, missing), Error::INVALID_ARGUMENT));
        }
        ENGINE_CHECK(failed(engine.acquire(0U, kTerrestrial), Error::INVALID_ARGUMENT));
        Attachment forged{};
        forged.owner_client_id = 1U;
        forged.lease_id = 1U;
        forged.attachment_id = 1U;
        forged.receiver = 18U;
        ENGINE_CHECK(failed(engine.detach_stream(forged), Error::NOT_FOUND));
        ENGINE_CHECK(same_calls("missing ids", f.calls.take(), {}));
    }
    f.calls.clear();

    // The slot ceiling bounds how many receivers exist, not which IDs.
    const std::array<ReceiverId, 9> nine{1U, 100U, 200U, 300U, 400U, 500U, 600U, 0xffffU, 7U};
    {
        ReceiverEngine full(f.backend, nine.data(), 8U, f.tokens, f.clock);
        ENGINE_CHECK(full.configured());
        ENGINE_CHECK(full.acquire(1U, 0xffffU));
        ENGINE_CHECK(failed(full.acquire(1U, 7U), Error::INVALID_ARGUMENT));
        const auto status = full.status();
        ENGINE_CHECK(status && status.value().receiver_count == 8U &&
                     status.value().states[7U] == ReceiverState::leased);
    }
    {
        ReceiverEngine over(f.backend, nine.data(), nine.size(), f.tokens, f.clock);
        ENGINE_CHECK(!over.configured());
        ENGINE_CHECK(failed(over.acquire(1U, 1U), Error::INVALID_ARGUMENT));
        const auto status = over.status();
        ENGINE_CHECK(status && status.value().receiver_count == 0U);
    }
    {
        const std::array<ReceiverId, 2> repeated{kTerrestrial, kTerrestrial};
        ReceiverEngine duplicate(f.backend, repeated.data(), repeated.size(), f.tokens,
                                 f.clock);
        ENGINE_CHECK(!duplicate.configured());
        ENGINE_CHECK(failed(duplicate.acquire(1U, kTerrestrial), Error::INVALID_ARGUMENT));
    }
    return true;
}

struct IndependentTunes final {
    bool proceeded = false;
    bool observed = false;
    Result<void> held = Result<void>::failure(Error::INTERNAL);
    Result<void> other = Result<void>::failure(Error::INTERNAL);
};

// Receiver 513 must complete its tune, and status must answer, while 17 is
// held inside its frontend tune.
IndependentTunes tune_while_other_receiver_held(Fixture& f, ReceiverEngine& engine,
                                                std::chrono::milliseconds bound)
{
    IndependentTunes result;
    result.proceeded = probe_while_held(
        f.backend.tune_gate, kTerrestrial,
        [&]() { result.held = engine.tune(1U, terrestrial(1U)); },
        [&]() {
            result.other = engine.tune(1U, satellite_slot(2U, 0U));
            result.observed = state_at(engine, 0U, ReceiverState::leased) &&
                              state_at(engine, 1U, ReceiverState::tuned);
        },
        bound);
    return result;
}

bool test_separate_receivers_are_not_serialized()
{
    Fixture f;
    ReceiverEngine engine(f.backend, f.ids.data(), 2U, f.tokens, f.clock);
    ENGINE_CHECK(engine.acquire(1U, kTerrestrial) && engine.acquire(1U, kSatellite));
    f.calls.clear();

    const IndependentTunes result = tune_while_other_receiver_held(f, engine, kWaitBound);
    ENGINE_CHECK(result.proceeded && result.observed && result.held && result.other);
    ENGINE_CHECK(state_at(engine, 0U, ReceiverState::tuned));
    ENGINE_CHECK(same_calls("blocked 17", f.calls.for_receiver(kTerrestrial),
                            {"supports 17 1", "power 17 0", "tune 17 1 557000 1000",
                             "lock 17", "commit 17"}));
    return true;
}

bool test_serialized_regression_fails_without_hanging()
{
    // Failure experiment: with every receiver behind one lock, the probe
    // cannot finish while 17 is held.  The check must report that and return;
    // the gate release then lets both operations complete.
    Fixture f;
    f.backend.serialize = true;
    ReceiverEngine engine(f.backend, f.ids.data(), 2U, f.tokens, f.clock);
    ENGINE_CHECK(engine.acquire(1U, kTerrestrial) && engine.acquire(1U, kSatellite));

    const auto started = std::chrono::steady_clock::now();
    const IndependentTunes result =
        tune_while_other_receiver_held(f, engine, kExpectedTimeout);
    const auto elapsed = std::chrono::steady_clock::now() - started;
    ENGINE_CHECK(!result.proceeded);
    ENGINE_CHECK(elapsed < kWaitBound);
    // Both operations still completed after the gate was released.
    ENGINE_CHECK(result.held && result.other);
    return true;
}

bool test_validation_travels_with_each_request()
{
    Fixture f;
    ReceiverEngine engine(f.backend, f.ids.data(), f.ids.size(), f.tokens, f.clock);
    ENGINE_CHECK(engine.acquire(1U, kTerrestrial) && engine.acquire(1U, kSecondTerrestrial));
    f.calls.clear();

    // Precedence: the decision is not consulted before the lease, stream and
    // support checks.
    ENGINE_CHECK(failed(engine.tune(1U, terrestrial(99U, Error::INVALID_ARGUMENT)),
                        Error::NOT_FOUND));
    TuneRequest unsupported = satellite_slot(1U, 0U);
    unsupported.validation = Error::UNSUPPORTED;
    ENGINE_CHECK(failed(engine.tune(1U, unsupported), Error::INVALID_ARGUMENT));
    ENGINE_CHECK(failed(engine.tune(1U, terrestrial(1U, Error::UNSUPPORTED)),
                        Error::UNSUPPORTED));
    ENGINE_CHECK(same_calls("precedence", f.calls.take(), {"supports 17 2", "supports 17 1"}));

    // Two requests whose neutral fields coincide (apart from the lease) run on
    // separate receivers.  The valid one waits at the validation point while
    // the invalid one completes; neither sees the other's decision.
    Result<void> held = Result<void>::failure(Error::INTERNAL);
    Result<void> rejected = Result<void>::failure(Error::INTERNAL);
    const bool proceeded = probe_while_held(
        f.backend.supports_gate, kTerrestrial,
        [&]() { held = engine.tune(1U, terrestrial(1U)); },
        [&]() { rejected = engine.tune(1U, terrestrial(2U, Error::INVALID_ARGUMENT)); });
    ENGINE_CHECK(proceeded && held && failed(rejected, Error::INVALID_ARGUMENT));
    ENGINE_CHECK(same_calls("rejected 4097", f.calls.for_receiver(kSecondTerrestrial),
                            {"supports 4097 1"}));
    ENGINE_CHECK(same_calls("held 17", f.calls.for_receiver(kTerrestrial),
                            {"supports 17 1", "power 17 0", "tune 17 1 557000 1000",
                             "lock 17", "commit 17"}));
    f.calls.clear();

    // Reverse roles: the invalid request waits while the valid one completes.
    held = Result<void>::failure(Error::INTERNAL);
    Result<void> accepted = Result<void>::failure(Error::INTERNAL);
    const bool reproceeded = probe_while_held(
        f.backend.supports_gate, kTerrestrial,
        [&]() { held = engine.tune(1U, terrestrial(1U, Error::INVALID_ARGUMENT)); },
        [&]() { accepted = engine.tune(1U, terrestrial(2U)); });
    ENGINE_CHECK(reproceeded && accepted && failed(held, Error::INVALID_ARGUMENT));
    ENGINE_CHECK(same_calls("held invalid 17", f.calls.for_receiver(kTerrestrial),
                            {"supports 17 1"}));
    ENGINE_CHECK(same_calls("accepted 4097", f.calls.for_receiver(kSecondTerrestrial),
                            {"supports 4097 1", "power 4097 0", "tune 4097 1 557000 1000",
                             "lock 4097", "commit 4097"}));

    // An armed stream (BUSY) also precedes the decision.
    ENGINE_CHECK(engine.start_stream(1U, 2U));
    f.calls.clear();
    ENGINE_CHECK(failed(engine.tune(1U, terrestrial(2U, Error::INVALID_ARGUMENT)),
                        Error::BUSY));
    ENGINE_CHECK(same_calls("busy", f.calls.take(), {}));
    return true;
}

bool test_stale_attachment_cleanup()
{
    Fixture f;
    ReceiverEngine engine(f.backend, f.ids.data(), 2U, f.tokens, f.clock, nullptr, nullptr,
                          &f.stream);
    const auto lease = engine.acquire(1U, kSatellite);
    ENGINE_CHECK(lease && engine.tune(1U, satellite_slot(1U, 1U)) && engine.start_stream(1U, 1U));
    const auto first = engine.attach_stream(1U, lease.value().token);
    ENGINE_CHECK(first && engine.stop_stream(1U, 1U));
    ENGINE_CHECK(engine.tune(1U, satellite_slot(1U, 1U)) && engine.start_stream(1U, 1U));
    const auto second = engine.attach_stream(1U, lease.value().token);
    ENGINE_CHECK(second && second.value().attachment_id == 2U);
    f.calls.clear();

    // Only the exact current identity may detach.
    ENGINE_CHECK(failed(engine.detach_stream(first.value()), Error::NOT_FOUND));
    Attachment moved = second.value();
    moved.receiver = kTerrestrial;
    ENGINE_CHECK(failed(engine.detach_stream(moved), Error::NOT_FOUND));
    Attachment forged = second.value();
    forged.token[0U] ^= 0xffU;
    ENGINE_CHECK(failed(engine.detach_stream(forged), Error::NOT_FOUND));
    ENGINE_CHECK(failed(engine.revoke_stream(1U, 1U, forged.token), Error::NOT_FOUND));
    ENGINE_CHECK(same_calls("stale", f.calls.take(), {}));
    ENGINE_CHECK(state_at(engine, 1U, ReceiverState::streaming));

    // Release of a streaming lease cleans up exactly the current identity.
    ENGINE_CHECK(engine.release(1U, 1U));
    ENGINE_CHECK(same_calls("release", f.calls.take(),
                            {"detach 513 1 2", "stop 513", "close 513"}));
    ENGINE_CHECK(failed(engine.detach_stream(second.value()), Error::NOT_FOUND));

    // A lost data plane quarantines the sparse receiver without further I/O.
    const auto again = engine.acquire(2U, kSatellite);
    ENGINE_CHECK(again && engine.tune(2U, satellite_slot(2U, 1U)) && engine.start_stream(2U, 2U));
    ENGINE_CHECK(engine.attach_stream(2U, again.value().token));
    f.calls.clear();
    f.stream.detach_failures = {Error::DISCONNECTED};
    ENGINE_CHECK(failed(engine.disconnect_client(2U), Error::DISCONNECTED));
    ENGINE_CHECK(same_calls("lost", f.calls.take(), {"detach 513 2 3", "lost 513"}));
    ENGINE_CHECK(state_at(engine, 1U, ReceiverState::error));
    ENGINE_CHECK(failed(engine.acquire(3U, kSatellite), Error::NOT_READY));
    return true;
}

bool test_legacy_timing_is_preserved()
{
    Fixture f;
    ReceiverEngine engine(f.backend, f.ids.data(), 1U, f.tokens, f.clock);
    ENGINE_CHECK(engine.acquire(1U, kTerrestrial));
    f.calls.clear();

    // Compatibility semantics, not the adopted v1 deadline: the tune budget
    // starts after power preparation, so a 5 s power wait does not consume a
    // 1 s budget.
    f.backend.power_wait_ms = 5000U;
    ENGINE_CHECK(engine.tune(1U, terrestrial(1U)));
    ENGINE_CHECK(same_calls("power outside budget", f.calls.take(),
                            {"supports 17 1", "power 17 0", "sleep 5000",
                             "tune 17 1 557000 1000", "lock 17", "commit 17"}));
    f.backend.power_wait_ms = 0U;

    // The settle interval is supplied by the backend and measured from the
    // first lock query.
    f.backend.settle_ms = 340U;
    ENGINE_CHECK(engine.tune(1U, terrestrial(1U)));
    ENGINE_CHECK(same_calls("settle", f.calls.take(),
                            {"supports 17 1", "power 17 0", "tune 17 1 557000 1000",
                             "lock 17", "sleep 340", "commit 17"}));
    TuneRequest short_budget = terrestrial(1U);
    short_budget.timeout_ms = 340U;
    ENGINE_CHECK(failed(engine.tune(1U, short_budget), Error::TIMEOUT));
    ENGINE_CHECK(same_calls("settle beyond budget", f.calls.take(),
                            {"supports 17 1", "power 17 0", "tune 17 1 557000 340",
                             "lock 17", "rollback 17"}));
    return true;
}

}  // namespace

int main()
{
    return test_sparse_ids_through_lifecycle() && test_invalid_missing_ids_and_capacity() &&
                   test_separate_receivers_are_not_serialized() &&
                   test_serialized_regression_fails_without_hanging() &&
                   test_validation_travels_with_each_request() &&
                   test_stale_attachment_cleanup() && test_legacy_timing_is_preserved()
               ? 0
               : 1;
}
