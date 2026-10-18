// SPDX-License-Identifier: MIT
#ifndef DTV_USERLAND_DETAIL_RECEIVER_ENGINE_H
#define DTV_USERLAND_DETAIL_RECEIVER_ENGINE_H

#include "dtv/userland/detail/stream_session_types.h"
#include "dtv/userland/error.h"
#include "dtv/userland/records.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <mutex>

// Internal compatibility engine for receiver leases, tuning, and stream
// attachment.  It carries the existing PX4 TunerService lifecycle unchanged so
// that product facades can share one implementation.  It is not the adopted
// v1 receiver contract: there are no public wire IDs, epochs or JSON records.
// The legacy direct entrance keeps its kHz frequencies and post-power timing;
// strict admission adds an end-to-end deadline and, since R3, an explicit
// strict Hz entrance with exact u64 requested Hz and typed evidence.  Only the
// bounded record types of records.h are reused; no codec is linked.  These
// declarations may change without notice and are not a stable API.
namespace dtv::userland::detail {

// Resource ceiling of the fixed slot table, not a receiver ID range.
inline constexpr std::size_t kReceiverEngineCapacity = 8U;

enum class ReceiverState : std::uint8_t {
    free,
    leased,
    tuned,
    streaming,
    error,
};

// Common frequency scalar bound: requested Hz is 1..2^53-1.
inline constexpr std::uint64_t kMaxRequestedHz = (std::uint64_t{1U} << 53U) - 1U;

// Programming entrance of one admitted request.  It is fixed by the request
// type at ingress, never inferred from a numeric value: the legacy kHz
// request runs the original kHz backend callback; the strict Hz request runs
// the strict Hz callback.
enum class TuneEntrance : std::uint8_t {
    legacy_khz = 0U,
    strict_hz = 1U,
};

// Immutable per-receiver readiness policy for strict Hz tunes.
enum class TuneReadiness : std::uint8_t {
    // Poll the backend demod lock predicate after programming (P/A).
    lock_poll = 0U,
    // The strict callback already completed a real bounded lock poll and
    // returns terminal lock evidence for this exact invocation.
    terminal_lock = 1U,
    // Command completion only; lock is left unsupported or unknown.
    commands = 2U,
    // The strict callback returns actual synchronized-TS evidence.
    ts_sync = 3U,
};

// Legacy-shaped tune request.  For satellite exactly one of stream_id/slot is
// in use and 0xffff marks the other.
struct TuneRequest final {
    std::uint64_t lease_id = 0U;
    ReceiverSystem system = ReceiverSystem::terrestrial;
    std::uint32_t frequency_khz = 0U;
    std::uint16_t stream_id = 0xffffU;
    std::uint16_t slot = 0xffffU;
    std::uint8_t lnb_voltage = 0U;
    std::uint32_t timeout_ms = 0U;
    // The caller's validation decision for this request.  The engine returns
    // it after the lease, receiver-state, stream, and receiver-support checks
    // and before any backend operation.
    Error validation = Error::OK;
};

// Strict Hz tune request.  requested_hz is the exact canonical frequency in
// the system-specific domain (satellite: IF), the single frequency authority
// for admission, current and history.  validation is the bounded outcome of
// the caller's pure profile preparation; the engine consumes it at the same
// position as the legacy decision.
struct HzTuneRequest final {
    std::uint64_t lease_id = 0U;
    // Receiver whose immutable profile produced validation.  Admission of a
    // lease on any other receiver is INVALID_ARGUMENT at the same position.
    ReceiverId prepared_receiver = 0U;
    ReceiverSystem system = ReceiverSystem::terrestrial;
    std::uint64_t requested_hz = 0U;
    std::uint16_t stream_id = 0xffffU;
    std::uint16_t slot = 0xffffU;
    std::uint8_t lnb_voltage = 0U;
    std::uint32_t timeout_ms = 0U;
    Error validation = Error::OK;
};

// Canonical request carried by admitted tickets and the shared helper.  For
// the legacy kHz entrance requested_hz is the legacy kHz value times 1000,
// which is exact; the kHz callback receives that original value back.
struct AdmittedRequest final {
    TuneEntrance entrance = TuneEntrance::legacy_khz;
    // Meaningful for the strict Hz entrance only.
    ReceiverId prepared_receiver = 0U;
    ReceiverSystem system = ReceiverSystem::terrestrial;
    std::uint64_t requested_hz = 0U;
    std::uint16_t stream_id = 0xffffU;
    std::uint16_t slot = 0xffffU;
    std::uint8_t lnb_voltage = 0U;
    std::uint32_t timeout_ms = 0U;
    Error validation = Error::OK;
};

// Small stack-local result of one strict Hz programming call.  It is never
// published by the backend itself; the engine copies it into current and
// history only in the final exact-ticket critical section.
struct TuneProgramming final {
    // Driver command value actually selected after the backend's deliberate
    // transforms, in the same domain as requested_hz.  Absent when the
    // backend exposes no honest scalar.  It is not a measured RF frequency
    // and not an inverse of synthesized PLL registers.
    wire::Optional<std::uint64_t> programmed_hz{};
    // Terminal lock evidence of this exact invocation (terminal_lock policy).
    wire::Measurement<bool> lock{};
    // Actual synchronized-TS evidence of this invocation (ts_sync policy).
    bool ts_synchronized = false;
};

// Immutable per-receiver capability descriptor for strict admission.  The
// product supplies the audited model/count metadata once before its workers
// start; strict admission then never calls the virtual supports predicate.
struct ReceiverCapability final {
    ReceiverId receiver = 0U;
    bool supports_terrestrial = false;
    bool supports_satellite = false;
    // The backend implements the strict Hz programming callback.  Without it
    // a strict Hz request is UNSUPPORTED before any revision change.
    bool native_hz = false;
    TuneReadiness readiness = TuneReadiness::lock_poll;
    // Whether this receiver publishes a lock measurement at all; selects
    // unknown versus unsupported for the commands policy.
    bool lock_measurement = true;
};

// Conservative per-receiver strict operation marker.  It is independent of
// the legacy ReceiverState vocabulary so an admission can exclude an
// in-flight tune or cleanup without changing the old status values.
enum class ReceiverTuneOperation : std::uint8_t {
    idle = 0U,
    tuning = 1U,
    cleanup = 2U,
};

// Permitted internal tune fields.  It deliberately excludes the lease ID,
// client identity, token, and validation credential that a request carries,
// so a status snapshot can never expose them.
struct TuneRecord final {
    ReceiverSystem system = ReceiverSystem::terrestrial;
    // Derived compatibility projection of requested_hz: floor(Hz / 1000) when
    // it fits 32 bits, otherwise 0.  It is never an execution input.
    std::uint32_t frequency_khz = 0U;
    std::uint16_t stream_id = 0xffffU;
    std::uint16_t slot = 0xffffU;
    std::uint8_t lnb_voltage = 0U;
    std::uint32_t timeout_ms = 0U;
    TuneEntrance entrance = TuneEntrance::legacy_khz;
    // Canonical exact request (satellite: IF).
    std::uint64_t requested_hz = 0U;
    // Backend command value, possibly absent; never a measured RF value.
    wire::Optional<std::uint64_t> programmed_hz{};
    wire::Verification verification = wire::Verification::lock;
    // Typed lock evidence.  A valid sample's sampled_at_ns is the engine
    // clock's monotonic milliseconds times 1000000 (checked); that is the
    // engine clock domain, not a public context-relative origin.  When the
    // time cannot be represented the state is unknown, never valid at 0.
    wire::Measurement<bool> lock{};
};

// Internal tune metadata.  It is not the adopted public v1 record and is
// never presented as a completed wire record.
struct TuneMetadata final {
    std::uint64_t revision = 0U;
    bool current_valid = false;
    std::uint64_t current_revision = 0U;
    TuneRecord current{};
    bool last_successful_valid = false;
    std::uint64_t last_successful_revision = 0U;
    TuneRecord last_successful{};
};

// Immutable admitted-ticket value owned by the worker queue.  It stores no
// pointer or reference into a caller; execution revalidates it against the
// registry before and after the hardware stages.
struct AdmittedTune final {
    std::uint64_t client_id = 0U;
    std::uint64_t lease_id = 0U;
    // Lease-instance identity.  It changes on every ACQUIRE, so a stale
    // queued ticket cannot match a later lease that reuses the numeric ID.
    std::uint64_t lease_instance = 0U;
    ReceiverId receiver = 0U;
    std::size_t slot = 0U;
    std::uint64_t tune_revision = 0U;
    std::uint64_t accepted_at_ms = 0U;
    AdmittedRequest request{};
};

struct AcquireResult final {
    std::uint64_t lease_id = 0U;
    ReceiverToken token{};
};

struct ReceiverEngineStatus final {
    std::uint64_t generation = 1U;
    // Slots [0, receiver_count) hold the configured receivers in order.
    std::size_t receiver_count = 0U;
    std::array<ReceiverId, kReceiverEngineCapacity> receivers{};
    std::array<ReceiverState, kReceiverEngineCapacity> states{};
    // Token-free strict operation and tune metadata.  It is internal state
    // (canonical Hz plus a derived kHz projection), not a completed public
    // v1 record.
    std::array<ReceiverTuneOperation, kReceiverEngineCapacity> operations{};
    std::array<TuneMetadata, kReceiverEngineCapacity> tune_metadata{};
    // Whether a logical lease is registered on the slot, projected from
    // leases_[slot].active under the same registry lock.  It carries no lease
    // ID, owner or token.  A release first revokes the streams and marks the
    // slot with ReceiverTuneOperation::cleanup under that lock, then clears
    // the Lease only after the receiver-serialization wait, so a slot whose
    // release is in progress still reports true here; false does not by
    // itself prove the physical cleanup has completed.
    std::array<bool, kReceiverEngineCapacity> leased{};
};

// Physical receiver seam.  Methods receive the stable receiver ID.
class ReceiverBackend {
public:
    virtual ~ReceiverBackend() noexcept = default;
    virtual bool supports(ReceiverId receiver, ReceiverSystem system) const noexcept = 0;
    virtual bool selects_satellite_stream_before_tune() const noexcept = 0;
    // Post-lock settle measured from the first lock query; 0 disables it.
    virtual std::uint32_t lock_settle_ms(ReceiverId receiver,
                                         ReceiverSystem system) const noexcept = 0;
    virtual Result<void> open(ReceiverId receiver) noexcept = 0;
    virtual Result<void> tune(ReceiverId receiver, ReceiverSystem system,
                              std::uint32_t frequency_khz,
                              std::uint32_t timeout_ms) noexcept = 0;
    // Strict Hz programming callback (R3).  requested_hz is exact; there is
    // no shared kHz fallback, so a backend that does not implement it reports
    // UNSUPPORTED.  It replaces only the frontend programming stage: power,
    // selection, lock/readiness, settle and commit stay in the engine.
    virtual Result<TuneProgramming> program_hz(ReceiverId receiver, ReceiverSystem system,
                                               std::uint64_t requested_hz,
                                               std::uint32_t timeout_ms) noexcept
    {
        (void)receiver;
        (void)system;
        (void)requested_hz;
        (void)timeout_ms;
        return Result<TuneProgramming>::failure(Error::UNSUPPORTED);
    }
    virtual Result<bool> is_locked(ReceiverId receiver, ReceiverSystem system) noexcept = 0;
    virtual Result<void> select_satellite_slot(ReceiverId receiver, std::uint8_t slot,
                                               std::uint32_t timeout_ms) noexcept = 0;
    virtual Result<void> select_satellite_tsid(ReceiverId receiver, std::uint16_t tsid,
                                               std::uint32_t timeout_ms) noexcept = 0;
    virtual Result<void> close(ReceiverId receiver) noexcept = 0;
    // begin_tune_power() precedes the frontend operations; a later failure is
    // paired with rollback_tune_power() and full success with commit.
    virtual Result<void> begin_tune_power(ReceiverId receiver, ReceiverSystem system,
                                          std::uint8_t lnb_voltage) noexcept = 0;
    virtual Result<void> commit_tune_power(ReceiverId receiver) noexcept = 0;
    virtual Result<void> rollback_tune_power(ReceiverId receiver) noexcept = 0;
    // Metadata-only transport-loss notification; must not perform I/O.
    virtual void mark_disconnected(ReceiverId receiver) noexcept = 0;
    // Called after all leases have been released.
    virtual Result<void> shutdown() noexcept = 0;
    virtual Result<void> start_capture(ReceiverId receiver,
                                       ReceiverSystem system) noexcept = 0;
    virtual Result<void> stop_capture(ReceiverId receiver,
                                      ReceiverSystem system) noexcept = 0;
};

class ReceiverTokenSource {
public:
    virtual ~ReceiverTokenSource() noexcept = default;
    virtual Result<ReceiverToken> generate() noexcept = 0;
};

class ReceiverIdSource {
public:
    virtual ~ReceiverIdSource() noexcept = default;
    virtual Result<std::uint64_t> next() noexcept = 0;
};

class ReceiverClock {
public:
    virtual ~ReceiverClock() noexcept = default;
    virtual std::uint64_t monotonic_ms() noexcept = 0;
    virtual void sleep_ms(std::uint32_t milliseconds) noexcept = 0;
};

// Optional data-plane seam, called while the receiver-local lock is held.
// detach() must logically invalidate the identity before it returns, even when
// physical cleanup fails, and retain the final snapshot for final_snapshot().
class ReceiverStreamControl {
public:
    virtual ~ReceiverStreamControl() noexcept = default;
    virtual Result<void> attach(const Attachment& attachment) noexcept = 0;
    virtual Result<void> detach(const Attachment& attachment) noexcept = 0;
    virtual Result<StreamCounters> stats(const Attachment& attachment) const noexcept = 0;
    virtual Result<StreamFinalSnapshot> final_snapshot(
        const Attachment& attachment) const noexcept = 0;
};

class ReceiverEngine final {
public:
    // receivers lists receiver_count distinct IDs in slot order; cleanup visits
    // them in this order.  A longer list or a repeated ID leaves the engine
    // unconfigured with no receivers.  The set is fixed for the engine's
    // lifetime; there is no receiver hot-plug or topology change.
    ReceiverEngine(ReceiverBackend& backend, const ReceiverId* receivers,
                   std::size_t receiver_count, ReceiverTokenSource& token_source,
                   ReceiverClock& clock, ReceiverIdSource* lease_id_source = nullptr,
                   ReceiverIdSource* attachment_id_source = nullptr,
                   ReceiverStreamControl* stream_control = nullptr) noexcept;
    ~ReceiverEngine() noexcept;

    ReceiverEngine(const ReceiverEngine&) = delete;
    ReceiverEngine& operator=(const ReceiverEngine&) = delete;

    bool configured() const noexcept { return configured_; }

    Result<AcquireResult> acquire(std::uint64_t client_id, ReceiverId receiver) noexcept;
    Result<void> release(std::uint64_t client_id, std::uint64_t lease_id) noexcept;
    Result<void> tune(std::uint64_t client_id, const TuneRequest& request) noexcept;
    Result<void> start_stream(std::uint64_t client_id, std::uint64_t lease_id) noexcept;
    Result<Attachment> attach_stream(std::uint64_t lease_id,
                                     const ReceiverToken& token) noexcept;
    Result<StreamFinalSnapshot> stop_stream(std::uint64_t client_id,
                                            std::uint64_t lease_id) noexcept;
    Result<StreamCounters> stats(std::uint64_t client_id,
                                 std::uint64_t lease_id) const noexcept;
    Result<void> detach_stream(const Attachment& attachment) noexcept;
    // Metadata-only; never waits for receiver I/O.
    Result<void> revoke_stream(std::uint64_t client_id, std::uint64_t lease_id) noexcept;
    // The token is checked under the same registry lock, so an invalid attach
    // cannot revoke a valid credential for the lease it guessed.
    Result<void> revoke_stream(std::uint64_t client_id, std::uint64_t lease_id,
                               const ReceiverToken& token) noexcept;
    Result<ReceiverEngineStatus> status() const noexcept;
    Result<void> disconnect_client(std::uint64_t client_id) noexcept;
    Result<void> shutdown() noexcept;

    // One-time strict capability setup.  It replaces the legacy virtual
    // supports query with the supplied immutable table for admission.  It
    // must list exactly the configured receiver slots, each supported in at
    // least one system, with no duplicates; otherwise it fails and legacy
    // mode is unchanged.  It performs no backend, clock, or ID-source call.
    Result<void> configure_strict_capabilities(const ReceiverCapability* capabilities,
                                               std::size_t count) noexcept;
    bool strict_capabilities_configured() const noexcept { return strict_enabled_; }

    // Callback-free metadata admission for the lane submission path.  It
    // takes the registry lock with try_lock only: contention returns BUSY
    // before any state change, and the caller must not wait.  It takes no
    // receiver lock and performs no backend, clock, ID-source, or worker call.
    Result<AdmittedTune> try_admit_tune(std::uint64_t client_id,
                                        const TuneRequest& request,
                                        std::uint64_t accepted_at_ms) noexcept;
    // Strict Hz entrance of the same admission body.  After the owner,
    // state, operation and system-support checks it returns UNSUPPORTED for a
    // receiver without native Hz programming, then INVALID_ARGUMENT for a
    // requested Hz outside 1..kMaxRequestedHz or an outcome prepared for
    // another receiver, then the prepared validation.  A rejection spends no
    // revision and changes no metadata.
    Result<AdmittedTune> try_admit_tune(std::uint64_t client_id,
                                        const HzTuneRequest& request,
                                        std::uint64_t accepted_at_ms) noexcept;
    // Executes one admitted ticket with the shared tune algorithm and the
    // admitted end-to-end budget.  It rechecks ticket identity, revision and
    // cancellation before the hardware stages and at the final commit.
    Result<void> execute_admitted_tune(const AdmittedTune& ticket) noexcept;
    // Aborts an admitted ticket that will never execute (publication or stop
    // failure after admission).  It keeps the advanced revision and clears
    // the pending metadata exactly once, without hardware I/O.
    Result<void> abort_unstarted_tune(const AdmittedTune& ticket, Error error) noexcept;
    // Synchronous logical-teardown invalidation of a pending admitted ticket.
    // The receiver stays excluded until the existing hardware cleanup runs.
    Result<void> revoke_tune_ticket(std::uint64_t client_id,
                                    std::uint64_t lease_id) noexcept;

    // Bounded deterministic seam for the nonwrapping revision-exhaustion path.
    // It is an internal detail helper used only by the R2 core test; production
    // code never calls it.
    void set_tune_revision_for_test(std::size_t slot, std::uint64_t value) noexcept;

private:
    enum class StreamState : std::uint8_t {
        none,
        armed,
        attaching,
        active,
        revoked,
        consumed,
    };

    struct PendingTune final {
        bool valid = false;
        AdmittedTune ticket{};
    };

    struct Lease final {
        bool active = false;
        std::uint64_t client_id = 0U;
        std::uint64_t lease_id = 0U;
        // Monotonic lease-instance identity, assigned at ACQUIRE.  It is not
        // the wire lease ID and never repeats for two live leases.
        std::uint64_t instance = 0U;
        std::size_t slot = 0U;
        ReceiverSystem system = ReceiverSystem::terrestrial;
        ReceiverToken token{};
        std::uint64_t attachment_id = 0U;
        StreamState stream_state = StreamState::none;
        std::uint64_t attach_started_ms = 0U;
        // A failed or revoked start may have partially enabled the pin.  Keep
        // this flag until physical cleanup has been attempted.
        bool capture_maybe_active = false;
        bool stream_maybe_attached = false;
        bool backend_disconnected = false;
    };

    static bool valid_client(std::uint64_t client_id) noexcept;
    int find_slot(ReceiverId receiver) const noexcept;
    ReceiverSystem initial_system(std::size_t slot) const noexcept;
    Attachment attachment_for(const Lease& lease,
                              std::uint64_t attachment_id) const noexcept;
    int find_lease_locked(std::uint64_t client_id,
                          std::uint64_t lease_id) const noexcept;
    bool lease_id_in_use_locked(std::uint64_t lease_id) const noexcept;
    bool attachment_id_in_use_locked(std::uint64_t attachment_id) const noexcept;
    Result<std::uint64_t> allocate_lease_id_locked(std::size_t slot) noexcept;
    Result<std::uint64_t> allocate_attachment_id_locked(std::size_t slot) noexcept;
    void set_state_locked(std::size_t slot, ReceiverState state) noexcept;
    Result<void> close_lease(const Lease& lease) noexcept;
    Result<void> stop_capture_if_needed(const Lease& lease) noexcept;
    Result<void> detach_data_plane_if_needed(const Lease& lease) noexcept;
    Result<void> release_lease(std::uint64_t client_id, std::uint64_t lease_id) noexcept;
    Result<void> cleanup_client(std::uint64_t client_id) noexcept;
    Result<void> cleanup_all() noexcept;
    void observe_disconnect(std::size_t slot, Error error) noexcept;
    // The admitted identity is threaded through the shared lock-poll loop so an
    // admitted cancellation/deadline is honored after every returned is_locked
    // (true or false) and before another poll/sleep.  The legacy entrance
    // passes admitted=false and keeps its exact old sampling and traces.
    // On a true result *sampled_ms is the clock value sampled before that
    // successful is_locked call (no extra clock read).
    Result<bool> poll_lock(std::size_t slot, ReceiverSystem system, std::uint64_t start_ms,
                           std::uint32_t timeout_ms, bool admitted,
                           const AdmittedTune& ticket, std::uint64_t* sampled_ms) noexcept;
    bool capability_supports_locked(std::size_t slot, ReceiverSystem system) const noexcept;
    // Shared admission body for both entrances.
    Result<AdmittedTune> admit(std::uint64_t client_id, std::uint64_t lease_id,
                               const AdmittedRequest& request,
                               std::uint64_t accepted_at_ms) noexcept;
    // Decision at R2's request-validation position.
    Error admission_decision_locked(std::size_t slot,
                                    const AdmittedRequest& request) const noexcept;
    bool ticket_current_locked(const AdmittedTune& ticket) const noexcept;
    // True when this exact ticket owns the slot: a matching pending ticket, or
    // the operation this ticket started (same lease instance and revision).
    // Empty pending metadata alone is not ownership.
    bool ticket_owns_slot_locked(std::size_t slot,
                                 const AdmittedTune& ticket) const noexcept;
    void set_operation_locked(std::size_t slot, ReceiverTuneOperation operation,
                              std::uint64_t lease_instance,
                              std::uint64_t revision) noexcept;
    void clear_operation_locked(std::size_t slot) noexcept;
    // Ownership includes the operation kind, so a cancelled direct (tuning,
    // revision 0) completion can never erase a cleanup marker recorded for the
    // same lease instance.
    bool operation_owned_by_locked(std::size_t slot, ReceiverTuneOperation kind,
                                   std::uint64_t lease_instance,
                                   std::uint64_t revision) const noexcept;
    void clear_operation_if_owner_locked(std::size_t slot, ReceiverTuneOperation kind,
                                         std::uint64_t lease_instance,
                                         std::uint64_t revision) noexcept;
    // Invalidate current while retaining the last-successful history.
    void invalidate_current_locked(std::size_t slot) noexcept;
    static AdmittedRequest legacy_request(const TuneRequest& request) noexcept;
    static TuneRecord tune_record(const AdmittedRequest& request,
                                  const TuneProgramming& programming,
                                  wire::Verification verification,
                                  const wire::Measurement<bool>& lock) noexcept;
    // Drops the pending ticket but leaves the operation marker alone, so a
    // synchronous logical-teardown invalidation keeps the receiver excluded
    // until the in-flight execution returns.
    void clear_pending_locked(std::size_t slot) noexcept;
    // Completes the exact ticket: drops its pending metadata and returns the
    // operation marker to idle.  It is a no-op when a newer admission already
    // owns the slot, so a stale task can never disturb a replacement ticket.
    void complete_tune_locked(std::size_t slot, const AdmittedTune& ticket) noexcept;
    // One shared tune execution helper for both entrances.  It runs with the
    // receiver lock held and implements the exact legacy hardware sequence.
    // Admitted tickets use the acceptance origin and end-to-end budget; the
    // legacy direct entrance keeps its post-power origin.
    // The programming stage is the only branch: the legacy kHz entrance calls
    // tune(), the strict Hz entrance calls program_hz().  Power, selection,
    // readiness, settle, commit and failure handling are shared.
    Result<void> run_tune(std::size_t slot, const AdmittedRequest& request, bool admitted,
                          const AdmittedTune& ticket) noexcept;

    ReceiverBackend& backend_;
    ReceiverTokenSource& token_source_;
    ReceiverClock& clock_;
    ReceiverIdSource* lease_id_source_;
    ReceiverIdSource* attachment_id_source_;
    ReceiverStreamControl* stream_control_;
    bool configured_ = false;
    std::size_t receiver_count_ = 0U;
    std::array<ReceiverId, kReceiverEngineCapacity> receivers_{};
    std::uint64_t next_lease_id_ = 1U;
    std::uint64_t next_attachment_id_ = 1U;
    std::uint64_t last_attachment_id_ = 0U;
    std::uint64_t next_lease_instance_ = 1U;
    std::uint64_t generation_ = 1U;
    std::array<Lease, kReceiverEngineCapacity> leases_{};
    std::array<std::uint64_t, kReceiverEngineCapacity> reserved_lease_ids_{};
    std::array<std::uint64_t, kReceiverEngineCapacity> reserved_attachment_ids_{};
    std::array<ReceiverState, kReceiverEngineCapacity> states_{};
    bool strict_enabled_ = false;
    std::array<ReceiverCapability, kReceiverEngineCapacity> strict_capabilities_{};
    std::array<ReceiverTuneOperation, kReceiverEngineCapacity> operations_{};
    // Identity of the operation recorded in operations_.  A direct legacy tune
    // records revision 0; a strict admission records its tune revision; a
    // cleanup records the lease instance.  This lets a stale ticket finalize
    // only its own operation and never a replacement lease's.
    std::array<std::uint64_t, kReceiverEngineCapacity> operation_instances_{};
    std::array<std::uint64_t, kReceiverEngineCapacity> operation_revisions_{};
    std::array<TuneMetadata, kReceiverEngineCapacity> tune_metadata_{};
    std::array<std::uint64_t, kReceiverEngineCapacity> tune_revisions_{};
    std::array<PendingTune, kReceiverEngineCapacity> pending_{};
    mutable std::mutex mutex_;
    mutable std::array<std::mutex, kReceiverEngineCapacity> receiver_mutexes_{};
};

}  // namespace dtv::userland::detail

#endif  // DTV_USERLAND_DETAIL_RECEIVER_ENGINE_H
