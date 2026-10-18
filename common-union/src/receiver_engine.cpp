// SPDX-License-Identifier: MIT
#include "dtv/userland/detail/receiver_engine.h"

#include <cstddef>
#include <limits>

namespace dtv::userland::detail {

ReceiverEngine::ReceiverEngine(ReceiverBackend& backend, const ReceiverId* receivers,
                               std::size_t receiver_count,
                               ReceiverTokenSource& token_source, ReceiverClock& clock,
                               ReceiverIdSource* lease_id_source,
                               ReceiverIdSource* attachment_id_source,
                               ReceiverStreamControl* stream_control) noexcept
    : backend_(backend), token_source_(token_source), clock_(clock),
      lease_id_source_(lease_id_source), attachment_id_source_(attachment_id_source),
      stream_control_(stream_control)
{
    states_.fill(ReceiverState::free);
    bool configured = receiver_count <= receivers_.size() &&
                      (receiver_count == 0U || receivers != nullptr);
    for (std::size_t slot = 0U; configured && slot < receiver_count; ++slot) {
        for (std::size_t earlier = 0U; earlier < slot; ++earlier) {
            if (receivers[earlier] == receivers[slot]) configured = false;
        }
        receivers_[slot] = receivers[slot];
    }
    configured_ = configured;
    receiver_count_ = configured ? receiver_count : 0U;
}

ReceiverEngine::~ReceiverEngine() noexcept
{
    (void)shutdown();
}

bool ReceiverEngine::valid_client(std::uint64_t client_id) noexcept
{
    return client_id != 0U;
}

int ReceiverEngine::find_slot(ReceiverId receiver) const noexcept
{
    for (std::size_t slot = 0U; slot < receiver_count_; ++slot) {
        if (receivers_[slot] == receiver) return static_cast<int>(slot);
    }
    return -1;
}

ReceiverSystem ReceiverEngine::initial_system(std::size_t slot) const noexcept
{
    // A dual-system receiver cannot stream before its first tune, which
    // replaces this value with the tuned system.
    const ReceiverId receiver = receivers_[slot];
    return backend_.supports(receiver, ReceiverSystem::satellite) &&
                   !backend_.supports(receiver, ReceiverSystem::terrestrial)
        ? ReceiverSystem::satellite
        : ReceiverSystem::terrestrial;
}

Attachment ReceiverEngine::attachment_for(const Lease& lease,
                                          std::uint64_t attachment_id) const noexcept
{
    return Attachment{lease.client_id, lease.lease_id, attachment_id,
                      receivers_[lease.slot], lease.system, lease.token};
}

int ReceiverEngine::find_lease_locked(std::uint64_t client_id,
                                      std::uint64_t lease_id) const noexcept
{
    if (!valid_client(client_id) || lease_id == 0U) return -1;
    for (std::size_t index = 0U; index < leases_.size(); ++index) {
        const Lease& lease = leases_[index];
        if (lease.active && lease.client_id == client_id &&
            lease.lease_id == lease_id)
            return static_cast<int>(index);
    }
    return -1;
}

bool ReceiverEngine::lease_id_in_use_locked(std::uint64_t lease_id) const noexcept
{
    if (lease_id == 0U) return true;
    for (const Lease& lease : leases_) {
        if (lease.active && lease.lease_id == lease_id) return true;
    }
    for (const std::uint64_t reserved : reserved_lease_ids_) {
        if (reserved == lease_id) return true;
    }
    return false;
}

bool ReceiverEngine::attachment_id_in_use_locked(
    std::uint64_t attachment_id) const noexcept
{
    if (attachment_id == 0U) return true;
    for (const Lease& lease : leases_) {
        if (lease.active && lease.attachment_id == attachment_id) return true;
    }
    for (const std::uint64_t reserved : reserved_attachment_ids_) {
        if (reserved == attachment_id) return true;
    }
    return false;
}

Result<std::uint64_t> ReceiverEngine::allocate_lease_id_locked(std::size_t slot) noexcept
{
    constexpr std::size_t kMaxAttempts = kReceiverEngineCapacity + 8U;
    for (std::size_t attempt = 0U; attempt < kMaxAttempts; ++attempt) {
        std::uint64_t candidate = 0U;
        if (lease_id_source_ != nullptr) {
            const auto next = lease_id_source_->next();
            if (!next) return Result<std::uint64_t>::failure(next.error());
            candidate = next.value();
        } else {
            candidate = next_lease_id_;
            next_lease_id_ = candidate == std::numeric_limits<std::uint64_t>::max()
                ? 1U
                : candidate + 1U;
        }
        if (candidate == 0U) {
            return Result<std::uint64_t>::failure(Error::INVALID_ARGUMENT);
        }
        if (!lease_id_in_use_locked(candidate)) {
            reserved_lease_ids_[slot] = candidate;
            return Result<std::uint64_t>::success(candidate);
        }
    }
    return Result<std::uint64_t>::failure(Error::BUSY);
}

Result<std::uint64_t> ReceiverEngine::allocate_attachment_id_locked(
    std::size_t slot) noexcept
{
    constexpr std::size_t kMaxAttempts = kReceiverEngineCapacity + 8U;
    for (std::size_t attempt = 0U; attempt < kMaxAttempts; ++attempt) {
        std::uint64_t candidate = 0U;
        if (attachment_id_source_ != nullptr) {
            const auto next = attachment_id_source_->next();
            if (!next) return Result<std::uint64_t>::failure(next.error());
            candidate = next.value();
        } else {
            candidate = next_attachment_id_;
            next_attachment_id_ = candidate == std::numeric_limits<std::uint64_t>::max()
                ? 0U
                : candidate + 1U;
        }
        if (candidate == 0U) {
            return Result<std::uint64_t>::failure(Error::BUSY);
        }
        // Attachment identities are never reused during this engine's
        // lifetime.  This remains true even when a deterministic test source
        // offers a stale numeric value after the old stream detached.
        if (candidate > last_attachment_id_ &&
            !attachment_id_in_use_locked(candidate)) {
            reserved_attachment_ids_[slot] = candidate;
            last_attachment_id_ = candidate;
            return Result<std::uint64_t>::success(candidate);
        }
    }
    return Result<std::uint64_t>::failure(Error::BUSY);
}

void ReceiverEngine::set_state_locked(std::size_t slot, ReceiverState state) noexcept
{
    if (states_[slot] != state) {
        states_[slot] = state;
        ++generation_;
    }
}

Result<AcquireResult> ReceiverEngine::acquire(std::uint64_t client_id,
                                              ReceiverId receiver) noexcept
{
    const int found = valid_client(client_id) ? find_slot(receiver) : -1;
    if (found < 0) return Result<AcquireResult>::failure(Error::INVALID_ARGUMENT);
    const auto slot = static_cast<std::size_t>(found);
    std::unique_lock<std::mutex> receiver_lock(receiver_mutexes_[slot]);
    ReceiverToken token_value{};
    std::uint64_t lease_id_value = 0U;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (leases_[slot].active)
            return Result<AcquireResult>::failure(Error::BUSY);
        if (states_[slot] == ReceiverState::error)
            return Result<AcquireResult>::failure(Error::NOT_READY);

        const auto token = token_source_.generate();
        if (!token) return Result<AcquireResult>::failure(token.error());
        const auto lease_id = allocate_lease_id_locked(slot);
        if (!lease_id)
            return Result<AcquireResult>::failure(lease_id.error());
        token_value = token.value();
        lease_id_value = lease_id.value();
    }

    const auto opened = backend_.open(receiver);
    if (!opened) {
        observe_disconnect(slot, opened.error());
        std::lock_guard<std::mutex> lock(mutex_);
        reserved_lease_ids_[slot] = 0U;
        return Result<AcquireResult>::failure(opened.error());
    }

    std::lock_guard<std::mutex> lock(mutex_);
    // receiver_lock prevents another operation from changing this slot while
    // the backend open is in flight.
    Lease& lease = leases_[slot];
    reserved_lease_ids_[slot] = 0U;
    lease.active = true;
    lease.client_id = client_id;
    lease.lease_id = lease_id_value;
    lease.instance = next_lease_instance_;
    next_lease_instance_ = next_lease_instance_ == std::numeric_limits<std::uint64_t>::max()
        ? 1U
        : next_lease_instance_ + 1U;
    lease.slot = slot;
    lease.system = initial_system(slot);
    lease.token = token_value;
    set_state_locked(slot, ReceiverState::leased);
    return Result<AcquireResult>::success(AcquireResult{lease.lease_id, lease.token});
}

void ReceiverEngine::observe_disconnect(std::size_t slot, Error error) noexcept
{
    if (error == Error::DISCONNECTED) {
        backend_.mark_disconnected(receivers_[slot]);
    }
}

Result<void> ReceiverEngine::close_lease(const Lease& lease) noexcept
{
    const auto result = backend_.close(receivers_[lease.slot]);
    observe_disconnect(lease.slot, result.error());
    return result;
}

Result<void> ReceiverEngine::stop_capture_if_needed(const Lease& lease) noexcept
{
    if (lease.backend_disconnected)
        return Result<void>::failure(Error::DISCONNECTED);
    if (!lease.capture_maybe_active) return Result<void>::success();
    const auto result = backend_.stop_capture(receivers_[lease.slot], lease.system);
    observe_disconnect(lease.slot, result.error());
    return result;
}

Result<void> ReceiverEngine::detach_data_plane_if_needed(const Lease& lease) noexcept
{
    if (!lease.stream_maybe_attached || stream_control_ == nullptr)
        return Result<void>::success();
    const auto result = stream_control_->detach(attachment_for(lease, lease.attachment_id));
    observe_disconnect(lease.slot, result.error());
    return result;
}

Result<void> ReceiverEngine::release_lease(std::uint64_t client_id,
                                           std::uint64_t lease_id) noexcept
{
    std::size_t slot = 0U;
    if (!valid_client(client_id) || lease_id == 0U)
        return Result<void>::failure(Error::NOT_FOUND);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const int index = find_lease_locked(client_id, lease_id);
        if (index < 0) return Result<void>::failure(Error::NOT_FOUND);
        slot = leases_[static_cast<std::size_t>(index)].slot;
        // Linearize cleanup with ATTACH before waiting for the receiver
        // operation.  ATTACH will compensate if its finite start is already
        // in progress.
        const StreamState stream = leases_[slot].stream_state;
        if (stream == StreamState::armed || stream == StreamState::attaching ||
            stream == StreamState::active)
            leases_[slot].stream_state = StreamState::revoked;
        // Synchronous logical teardown: drop any admitted ticket, invalidate
        // current (retaining last-successful history) and keep the receiver
        // excluded while the hardware cleanup runs.
        clear_pending_locked(slot);
        invalidate_current_locked(slot);
        set_operation_locked(slot, ReceiverTuneOperation::cleanup,
                             leases_[slot].instance, 0U);
    }

    std::unique_lock<std::mutex> receiver_lock(receiver_mutexes_[slot]);
    Lease lease;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const int index = find_lease_locked(client_id, lease_id);
        if (index < 0) {
            clear_operation_locked(slot);
            return Result<void>::failure(Error::NOT_FOUND);
        }
        lease = leases_[static_cast<std::size_t>(index)];
        leases_[static_cast<std::size_t>(index)] = Lease{};
    }

    Error first = Error::OK;
    const auto detached = detach_data_plane_if_needed(lease);
    const bool detach_clean = detached || detached.error() == Error::NOT_FOUND;
    if (!detach_clean) first = detached.error();
    const bool disconnected = lease.backend_disconnected ||
                              detached.error() == Error::DISCONNECTED;
    const auto stopped = disconnected
        ? Result<void>::failure(Error::DISCONNECTED)
        : stop_capture_if_needed(lease);
    if (!stopped && first == Error::OK) first = stopped.error();
    // A disconnected transport is terminal for this cleanup path; do not
    // issue a later close write to the same device.
    if (!disconnected &&
        stopped.error() != Error::DISCONNECTED) {
        const auto closed = close_lease(lease);
        if (!closed && first == Error::OK) first = closed.error();
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        // The receiver is announced free only after a fully successful
        // hardware cleanup.  It is re-evaluated after receiver serialization,
        // not from a value sampled before the wait: an in-flight rejected
        // commit or any other quarantine may have been established in the
        // interim.  In a strict engine any ambiguous cleanup failure (detach,
        // stop, close, or transport loss) stays excluded; a nonstrict engine
        // keeps the old direct close-only compatibility.  A successful close
        // still returns the receiver to free so an exact RELEASE is reusable.
        const bool failed = first != Error::OK;
        if (!failed)
            set_state_locked(slot, ReceiverState::free);
        else if (strict_enabled_ || !detach_clean || !stopped || disconnected)
            set_state_locked(slot, ReceiverState::error);
        else
            set_state_locked(slot, ReceiverState::free);
        clear_operation_locked(slot);
    }
    return first == Error::OK ? Result<void>::success()
                              : Result<void>::failure(first);
}

Result<void> ReceiverEngine::release(std::uint64_t client_id,
                                     std::uint64_t lease_id) noexcept
{
    return release_lease(client_id, lease_id);
}

Result<bool> ReceiverEngine::poll_lock(std::size_t slot, ReceiverSystem system,
                                       std::uint64_t start_ms,
                                       std::uint32_t timeout_ms, bool admitted,
                                       const AdmittedTune& ticket,
                                       std::uint64_t* sampled_ms) noexcept
{
    const std::size_t max_polls = static_cast<std::size_t>(timeout_ms / 10U) + 1U;
    // Admitted-only exact ticket/deadline decision.  It is checked before every
    // hardware poll (covering a cancellation/expiry during the previous
    // inter-poll sleep) and after every returned poll (true or false).  The
    // legacy entrance is a no-op here.
    const auto admitted_gate = [&]() noexcept -> Error {
        if (!admitted) return Error::OK;
        bool current = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            current = pending_[slot].valid &&
                      pending_[slot].ticket.tune_revision == ticket.tune_revision &&
                      ticket_current_locked(pending_[slot].ticket);
        }
        if (!current) return Error::NOT_FOUND;
        return clock_.monotonic_ms() - ticket.accepted_at_ms >= timeout_ms
                   ? Error::TIMEOUT
                   : Error::OK;
    };
    for (std::size_t count = 0U; count < max_polls; ++count) {
        const std::uint64_t before = clock_.monotonic_ms();
        if (before - start_ms >= timeout_ms) break;
        // Gate before the next poll (after the previous sleep).
        const Error pre = admitted_gate();
        if (pre != Error::OK) return Result<bool>::failure(pre);
        const auto locked = backend_.is_locked(receivers_[slot], system);
        if (!locked) {
            // Hardware error/transport primary and terminal handling stay
            // ahead of the success-only admitted metadata gates.
            observe_disconnect(slot, locked.error());
            return Result<bool>::failure(locked.error());
        }
        // Gate after the returned poll (true or false).
        const Error post = admitted_gate();
        if (post != Error::OK) return Result<bool>::failure(post);
        if (locked.value()) {
            if (sampled_ms != nullptr) *sampled_ms = before;
            return Result<bool>::success(true);
        }

        const std::uint64_t now = clock_.monotonic_ms();
        const std::uint64_t elapsed = now - start_ms;
        if (elapsed >= timeout_ms || count + 1U == max_polls)
            break;
        const std::uint64_t remaining = timeout_ms - elapsed;
        if (remaining < 10U) break;
        clock_.sleep_ms(10U);
    }
    return Result<bool>::success(false);
}

bool ReceiverEngine::capability_supports_locked(std::size_t slot,
                                                ReceiverSystem system) const noexcept
{
    const ReceiverCapability& capability = strict_capabilities_[slot];
    if (system == ReceiverSystem::terrestrial) return capability.supports_terrestrial;
    if (system == ReceiverSystem::satellite) return capability.supports_satellite;
    // Unknown system numbers are unsupported in the strict descriptor.
    return false;
}

void ReceiverEngine::clear_pending_locked(std::size_t slot) noexcept
{
    pending_[slot] = PendingTune{};
}

void ReceiverEngine::invalidate_current_locked(std::size_t slot) noexcept
{
    // Logical owner teardown clears the current tune but retains the
    // last-successful history for observation.
    tune_metadata_[slot].current_valid = false;
}

void ReceiverEngine::set_operation_locked(std::size_t slot,
                                          ReceiverTuneOperation operation,
                                          std::uint64_t lease_instance,
                                          std::uint64_t revision) noexcept
{
    operations_[slot] = operation;
    operation_instances_[slot] = operation == ReceiverTuneOperation::idle ? 0U
                                                                          : lease_instance;
    operation_revisions_[slot] = operation == ReceiverTuneOperation::idle ? 0U : revision;
}

void ReceiverEngine::clear_operation_locked(std::size_t slot) noexcept
{
    set_operation_locked(slot, ReceiverTuneOperation::idle, 0U, 0U);
}

bool ReceiverEngine::operation_owned_by_locked(std::size_t slot,
                                               ReceiverTuneOperation kind,
                                               std::uint64_t lease_instance,
                                               std::uint64_t revision) const noexcept
{
    return operations_[slot] == kind && kind != ReceiverTuneOperation::idle &&
           operation_instances_[slot] == lease_instance &&
           operation_revisions_[slot] == revision;
}

void ReceiverEngine::clear_operation_if_owner_locked(std::size_t slot,
                                                     ReceiverTuneOperation kind,
                                                     std::uint64_t lease_instance,
                                                     std::uint64_t revision) noexcept
{
    if (operation_owned_by_locked(slot, kind, lease_instance, revision))
        clear_operation_locked(slot);
}

AdmittedRequest ReceiverEngine::legacy_request(const TuneRequest& request) noexcept
{
    AdmittedRequest admitted{};
    admitted.entrance = TuneEntrance::legacy_khz;
    admitted.system = request.system;
    // Checked ingress: any 32-bit kHz value times 1000 fits 64 bits exactly.
    admitted.requested_hz = static_cast<std::uint64_t>(request.frequency_khz) * 1000U;
    admitted.stream_id = request.stream_id;
    admitted.slot = request.slot;
    admitted.lnb_voltage = request.lnb_voltage;
    admitted.timeout_ms = request.timeout_ms;
    admitted.validation = request.validation;
    return admitted;
}

TuneRecord ReceiverEngine::tune_record(const AdmittedRequest& request,
                                       const TuneProgramming& programming,
                                       wire::Verification verification,
                                       const wire::Measurement<bool>& lock) noexcept
{
    TuneRecord record{};
    record.system = request.system;
    const std::uint64_t khz = request.requested_hz / 1000U;
    record.frequency_khz = khz <= std::numeric_limits<std::uint32_t>::max()
        ? static_cast<std::uint32_t>(khz)
        : 0U;
    record.stream_id = request.stream_id;
    record.slot = request.slot;
    record.lnb_voltage = request.lnb_voltage;
    record.timeout_ms = request.timeout_ms;
    record.entrance = request.entrance;
    record.requested_hz = request.requested_hz;
    record.programmed_hz = programming.programmed_hz;
    record.verification = verification;
    record.lock = lock;
    return record;
}

bool ReceiverEngine::ticket_owns_slot_locked(std::size_t slot,
                                             const AdmittedTune& ticket) const noexcept
{
    if (pending_[slot].valid) {
        const AdmittedTune& pending = pending_[slot].ticket;
        return pending.client_id == ticket.client_id &&
               pending.lease_id == ticket.lease_id &&
               pending.lease_instance == ticket.lease_instance &&
               pending.tune_revision == ticket.tune_revision;
    }
    // Empty pending metadata does not prove ownership.  The ticket owns the
    // slot only if it still holds the recorded operation, or no operation is
    // active and its exact lease instance is still the current one.
    if (operations_[slot] != ReceiverTuneOperation::idle)
        return operation_owned_by_locked(slot, ReceiverTuneOperation::tuning,
                                         ticket.lease_instance, ticket.tune_revision);
    const int index = find_lease_locked(ticket.client_id, ticket.lease_id);
    return index >= 0 &&
           leases_[static_cast<std::size_t>(index)].instance == ticket.lease_instance;
}

void ReceiverEngine::complete_tune_locked(std::size_t slot,
                                          const AdmittedTune& ticket) noexcept
{
    if (pending_[slot].valid) {
        const AdmittedTune& pending = pending_[slot].ticket;
        if (pending.client_id != ticket.client_id ||
            pending.lease_id != ticket.lease_id ||
            pending.lease_instance != ticket.lease_instance ||
            pending.tune_revision != ticket.tune_revision)
            return;  // a newer admission owns the pending ticket
    }
    pending_[slot] = PendingTune{};
    // Return the operation marker to idle only when this exact ticket started
    // it (kind tuning, same instance/revision).  A stale ticket must never
    // clear a replacement lease's active direct tune or a cleanup marker.
    clear_operation_if_owner_locked(slot, ReceiverTuneOperation::tuning,
                                    ticket.lease_instance, ticket.tune_revision);
}

bool ReceiverEngine::ticket_current_locked(const AdmittedTune& ticket) const noexcept
{
    if (!pending_[ticket.slot].valid) return false;
    const AdmittedTune& pending = pending_[ticket.slot].ticket;
    if (pending.client_id != ticket.client_id ||
        pending.lease_id != ticket.lease_id ||
        pending.lease_instance != ticket.lease_instance ||
        pending.tune_revision != ticket.tune_revision)
        return false;
    // The lease instance must still be the one the ticket was admitted for.
    const int index = find_lease_locked(ticket.client_id, ticket.lease_id);
    return index >= 0 && leases_[static_cast<std::size_t>(index)].instance ==
                             ticket.lease_instance;
}

Result<void> ReceiverEngine::configure_strict_capabilities(
    const ReceiverCapability* capabilities, std::size_t count) noexcept
{
    if (!configured_ || capabilities == nullptr || count != receiver_count_ ||
        count == 0U)
        return Result<void>::failure(Error::INVALID_ARGUMENT);

    std::array<ReceiverCapability, kReceiverEngineCapacity> table{};
    std::array<bool, kReceiverEngineCapacity> seen{};
    for (std::size_t index = 0U; index < count; ++index) {
        const int slot = find_slot(capabilities[index].receiver);
        if (slot < 0 || seen[static_cast<std::size_t>(slot)])
            return Result<void>::failure(Error::INVALID_ARGUMENT);
        if (!capabilities[index].supports_terrestrial &&
            !capabilities[index].supports_satellite)
            return Result<void>::failure(Error::INVALID_ARGUMENT);
        seen[static_cast<std::size_t>(slot)] = true;
        table[static_cast<std::size_t>(slot)] = capabilities[index];
    }
    for (std::size_t slot = 0U; slot < receiver_count_; ++slot) {
        if (!seen[slot]) return Result<void>::failure(Error::INVALID_ARGUMENT);
    }

    std::lock_guard<std::mutex> lock(mutex_);
    strict_capabilities_ = table;
    strict_enabled_ = true;
    return Result<void>::success();
}

Error ReceiverEngine::admission_decision_locked(std::size_t slot,
                                                const AdmittedRequest& request) const noexcept
{
    if (!capability_supports_locked(slot, request.system)) return Error::INVALID_ARGUMENT;
    if (request.entrance == TuneEntrance::strict_hz) {
        if (!strict_capabilities_[slot].native_hz) return Error::UNSUPPORTED;
        // Common scalar check on the canonical value before any narrowing.
        if (request.requested_hz == 0U || request.requested_hz > kMaxRequestedHz)
            return Error::INVALID_ARGUMENT;
        if (request.prepared_receiver != receivers_[slot]) return Error::INVALID_ARGUMENT;
    }
    return request.validation;
}

Result<AdmittedTune> ReceiverEngine::try_admit_tune(std::uint64_t client_id,
                                                    const TuneRequest& request,
                                                    std::uint64_t accepted_at_ms) noexcept
{
    return admit(client_id, request.lease_id, legacy_request(request), accepted_at_ms);
}

Result<AdmittedTune> ReceiverEngine::try_admit_tune(std::uint64_t client_id,
                                                    const HzTuneRequest& request,
                                                    std::uint64_t accepted_at_ms) noexcept
{
    AdmittedRequest admitted{};
    admitted.entrance = TuneEntrance::strict_hz;
    admitted.prepared_receiver = request.prepared_receiver;
    admitted.system = request.system;
    admitted.requested_hz = request.requested_hz;
    admitted.stream_id = request.stream_id;
    admitted.slot = request.slot;
    admitted.lnb_voltage = request.lnb_voltage;
    admitted.timeout_ms = request.timeout_ms;
    admitted.validation = request.validation;
    return admit(client_id, request.lease_id, admitted, accepted_at_ms);
}

Result<AdmittedTune> ReceiverEngine::admit(std::uint64_t client_id, std::uint64_t lease_id,
                                           const AdmittedRequest& request,
                                           std::uint64_t accepted_at_ms) noexcept
{
    if (!strict_enabled_ || !valid_client(client_id))
        return Result<AdmittedTune>::failure(Error::NOT_READY);
    std::unique_lock<std::mutex> lock(mutex_, std::try_to_lock);
    if (!lock.owns_lock()) return Result<AdmittedTune>::failure(Error::BUSY);

    const int index = find_lease_locked(client_id, lease_id);
    if (index < 0) return Result<AdmittedTune>::failure(Error::NOT_FOUND);
    const std::size_t slot = leases_[static_cast<std::size_t>(index)].slot;
    if (states_[slot] == ReceiverState::error)
        return Result<AdmittedTune>::failure(Error::NOT_READY);
    const Lease& lease = leases_[static_cast<std::size_t>(index)];
    if (lease.stream_state == StreamState::armed ||
        lease.stream_state == StreamState::attaching ||
        lease.stream_state == StreamState::active ||
        lease.stream_state == StreamState::revoked)
        return Result<AdmittedTune>::failure(Error::BUSY);
    // An active tune or hardware cleanup on this receiver is BUSY rather than
    // a blocking wait on the poll thread.
    if (operations_[slot] != ReceiverTuneOperation::idle)
        return Result<AdmittedTune>::failure(Error::BUSY);
    // Support, strict-Hz entrance and validation decisions at R2's position.
    const Error decision = admission_decision_locked(slot, request);
    if (decision != Error::OK) return Result<AdmittedTune>::failure(decision);
    // Nonwrapping revision issuance: exhaustion fails without state change.
    // RESOURCE_LIMIT is the existing internal vocabulary for this issuance
    // limit; facades map it to an existing old-wire code until the v1 wire
    // carries a dedicated value.
    if (tune_revisions_[slot] == std::numeric_limits<std::uint64_t>::max())
        return Result<AdmittedTune>::failure(Error::RESOURCE_LIMIT);
    const std::uint64_t revision = tune_revisions_[slot] + 1U;

    // This is the admission linearization point: the accepted request
    // advances the revision and invalidates the current tune.
    tune_revisions_[slot] = revision;
    tune_metadata_[slot].revision = revision;
    tune_metadata_[slot].current_valid = false;
    set_operation_locked(slot, ReceiverTuneOperation::tuning, lease.instance, revision);
    pending_[slot].valid = true;
    pending_[slot].ticket = AdmittedTune{
        client_id, lease.lease_id, lease.instance, receivers_[slot], slot, revision,
        accepted_at_ms, request};
    return Result<AdmittedTune>::success(pending_[slot].ticket);
}

Result<void> ReceiverEngine::execute_admitted_tune(const AdmittedTune& ticket) noexcept
{
    std::size_t slot = ticket.slot;
    Lease lease;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (slot >= kReceiverEngineCapacity) return Result<void>::failure(Error::NOT_FOUND);
        if (!ticket_current_locked(ticket)) {
            // Invalidated before execution started.  Complete only if this
            // ticket still owns the slot.
            complete_tune_locked(slot, ticket);
            return Result<void>::failure(Error::NOT_FOUND);
        }
        const int index = find_lease_locked(ticket.client_id, ticket.lease_id);
        lease = leases_[static_cast<std::size_t>(index)];
        if (states_[slot] == ReceiverState::error) {
            complete_tune_locked(slot, ticket);
            return Result<void>::failure(Error::NOT_READY);
        }
    }
    std::unique_lock<std::mutex> receiver_lock(receiver_mutexes_[slot]);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        // Revalidate after receiver serialization: an owner teardown may have
        // invalidated the ticket while this task waited.
        if (!ticket_current_locked(ticket)) {
            complete_tune_locked(slot, ticket);
            return Result<void>::failure(Error::NOT_FOUND);
        }
    }
    if (lease.stream_state == StreamState::armed ||
        lease.stream_state == StreamState::attaching ||
        lease.stream_state == StreamState::active ||
        lease.stream_state == StreamState::revoked) {
        std::lock_guard<std::mutex> lock(mutex_);
        complete_tune_locked(slot, ticket);
        return Result<void>::failure(Error::BUSY);
    }
    const AdmittedRequest& request = ticket.request;
    // Strict admission and the admitted helper use only the immutable
    // capability table; the virtual supports predicate is not consulted.
    if (!strict_enabled_ || !capability_supports_locked(slot, request.system)) {
        std::lock_guard<std::mutex> lock(mutex_);
        complete_tune_locked(slot, ticket);
        return Result<void>::failure(Error::INVALID_ARGUMENT);
    }
    {
        const Error decision = admission_decision_locked(slot, request);
        if (decision != Error::OK) {
            std::lock_guard<std::mutex> lock(mutex_);
            complete_tune_locked(slot, ticket);
            return Result<void>::failure(decision);
        }
    }
    const auto result = run_tune(slot, request, true, ticket);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        complete_tune_locked(slot, ticket);
    }
    return result;
}

Result<void> ReceiverEngine::abort_unstarted_tune(const AdmittedTune& ticket,
                                                  Error error) noexcept
{
    (void)error;
    if (ticket.slot >= kReceiverEngineCapacity)
        return Result<void>::failure(Error::NOT_FOUND);
    std::lock_guard<std::mutex> lock(mutex_);
    complete_tune_locked(ticket.slot, ticket);
    return Result<void>::success();
}

Result<void> ReceiverEngine::revoke_tune_ticket(std::uint64_t client_id,
                                                std::uint64_t lease_id) noexcept
{
    if (!valid_client(client_id) || lease_id == 0U)
        return Result<void>::failure(Error::NOT_FOUND);
    std::lock_guard<std::mutex> lock(mutex_);
    // Logical owner teardown invalidates current (retaining last-success
    // history) and revokes the exact pending ticket for this lease.
    const int index = find_lease_locked(client_id, lease_id);
    if (index >= 0) invalidate_current_locked(leases_[static_cast<std::size_t>(index)].slot);
    for (std::size_t slot = 0U; slot < pending_.size(); ++slot) {
        if (!pending_[slot].valid) continue;
        const AdmittedTune& ticket = pending_[slot].ticket;
        if (ticket.client_id == client_id && ticket.lease_id == lease_id) {
            clear_pending_locked(slot);
            return Result<void>::success();
        }
    }
    return Result<void>::success();
}

// Bounded deterministic seam for the nonwrapping revision-exhaustion path.
// The symbol is always built so a test translation unit that declares it via
// DTV_USERLAND_RECEIVER_TEST_ACCESS can link; production does not use it.
void ReceiverEngine::set_tune_revision_for_test(std::size_t slot,
                                                std::uint64_t value) noexcept
{
    if (slot >= kReceiverEngineCapacity) return;
    std::lock_guard<std::mutex> lock(mutex_);
    tune_revisions_[slot] = value;
    tune_metadata_[slot].revision = value;
}

Result<void> ReceiverEngine::tune(std::uint64_t client_id,
                                  const TuneRequest& request) noexcept
{
    std::size_t slot = 0U;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const int index = find_lease_locked(client_id, request.lease_id);
        if (index < 0)
            return Result<void>::failure(Error::NOT_FOUND);
        slot = leases_[static_cast<std::size_t>(index)].slot;
    }
    std::unique_lock<std::mutex> receiver_lock(receiver_mutexes_[slot]);
    Lease lease;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const int index = find_lease_locked(client_id, request.lease_id);
        if (index < 0)
            return Result<void>::failure(Error::NOT_FOUND);
        lease = leases_[static_cast<std::size_t>(index)];
        if (states_[lease.slot] == ReceiverState::error)
            return Result<void>::failure(Error::NOT_READY);
        // The legacy direct entrance shares exclusion with a strict pending
        // ticket: a tuning or cleanup operation is BUSY here too.
        if (operations_[slot] != ReceiverTuneOperation::idle)
            return Result<void>::failure(Error::BUSY);
        // Mark the direct operation active under the registry lock so a
        // concurrent strict try_admit_tune sees BUSY with no metadata change.
        // The marker records this exact lease instance with revision 0 so a
        // stale admitted ticket cannot clear it.
        set_operation_locked(slot, ReceiverTuneOperation::tuning, lease.instance, 0U);
    }
    const auto clear_direct_operation = [&]() noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        // Only clear our own tuning marker: a cleanup installed by a
        // concurrent release during the finite tune must survive.
        clear_operation_if_owner_locked(slot, ReceiverTuneOperation::tuning,
                                        lease.instance, 0U);
    };
    if (lease.stream_state == StreamState::armed ||
        lease.stream_state == StreamState::attaching ||
        lease.stream_state == StreamState::active ||
        lease.stream_state == StreamState::revoked) {
        clear_direct_operation();
        return Result<void>::failure(Error::BUSY);
    }
    const ReceiverId receiver = receivers_[slot];
    if (!backend_.supports(receiver, request.system)) {
        clear_direct_operation();
        return Result<void>::failure(Error::INVALID_ARGUMENT);
    }
    // The decision travels with this request; nothing else is consulted.
    if (request.validation != Error::OK) {
        const Error decision = request.validation;
        clear_direct_operation();
        return Result<void>::failure(decision);
    }

    // Legacy direct compatibility entrance: it keeps the old virtual supports
    // query above and the old post-power timing origin inside the helper.
    const auto result = run_tune(slot, legacy_request(request), false, AdmittedTune{});
    clear_direct_operation();
    return result;
}

Result<void> ReceiverEngine::run_tune(std::size_t slot, const AdmittedRequest& request,
                                      bool admitted, const AdmittedTune& ticket) noexcept
{
    const ReceiverId receiver = receivers_[slot];
    const std::uint64_t accepted_at_ms =
        admitted ? ticket.accepted_at_ms : 0U;
    const std::uint64_t revision = admitted ? ticket.tune_revision : 0U;
    if (admitted) {
        // An already expired ticket performs no power or front-end I/O.
        if (clock_.monotonic_ms() - accepted_at_ms >= request.timeout_ms)
            return Result<void>::failure(Error::TIMEOUT);
    }
    {
        // Pre-power registry transition: after any initial clock delay or
        // registry wait, recheck the exact ticket/owner and deadline before
        // publishing the leased state or starting power.  An unstarted
        // cancelled/expired task issues no power/frontend I/O.
        std::lock_guard<std::mutex> lock(mutex_);
        if (admitted) {
            const bool current = pending_[slot].valid &&
                                 pending_[slot].ticket.tune_revision == revision &&
                                 ticket_current_locked(pending_[slot].ticket);
            if (!current) return Result<void>::failure(Error::NOT_FOUND);
            if (clock_.monotonic_ms() - accepted_at_ms >= request.timeout_ms)
                return Result<void>::failure(Error::TIMEOUT);
        }
        set_state_locked(slot, ReceiverState::leased);
    }
    const auto power_prepared = backend_.begin_tune_power(
        receiver, request.system, request.lnb_voltage);
    if (!power_prepared) {
        observe_disconnect(slot, power_prepared.error());
        return Result<void>::failure(power_prepared.error());
    }
    const auto fail_after_power = [&](Error primary) noexcept {
        // A transport-loss result is terminal before rollback: rollback may
        // update logical references but must not write GPIO on that bridge.
        observe_disconnect(slot, primary);
        const auto rollback = backend_.rollback_tune_power(receiver);
        observe_disconnect(slot, rollback.error());
        if (!rollback) {
            std::lock_guard<std::mutex> lock(mutex_);
            set_state_locked(slot, ReceiverState::error);
        }
        return Result<void>::failure(primary);
    };
    // Admitted-only stage boundary gate.  It rechecks the exact ticket and the
    // remaining budget after a returned stage and before starting the next
    // one.  It reads only owned metadata and performs no backend/OS/worker
    // callback.  The legacy direct entrance is a no-op here.
    const auto stage_gate = [&]() noexcept -> Error {
        if (!admitted) return Error::OK;
        std::lock_guard<std::mutex> lock(mutex_);
        const bool current = pending_[slot].valid &&
                             pending_[slot].ticket.tune_revision == revision &&
                             ticket_current_locked(pending_[slot].ticket);
        if (!current) return Error::NOT_FOUND;
        // Sample the deadline while holding the registry lock so registry
        // acquisition time is accounted for.
        return clock_.monotonic_ms() - accepted_at_ms >= request.timeout_ms
                   ? Error::TIMEOUT
                   : Error::OK;
    };
    // The admitted deadline starts at request acceptance; the legacy direct
    // entrance keeps its post-power origin.
    const std::uint64_t start_ms = admitted ? accepted_at_ms : clock_.monotonic_ms();
    const auto remaining_before_tune = [&]() noexcept -> std::uint32_t {
        const std::uint64_t elapsed = clock_.monotonic_ms() - start_ms;
        return elapsed >= request.timeout_ms
            ? 0U
            : static_cast<std::uint32_t>(request.timeout_ms - elapsed);
    };
    // After returned power preparation, before the first frontend stage.
    {
        const Error gate = stage_gate();
        if (gate != Error::OK) return fail_after_power(gate);
    }
    const bool select_before_tune = request.system == ReceiverSystem::satellite &&
                                    backend_.selects_satellite_stream_before_tune();
    const auto select_satellite_stream = [&](std::uint32_t remaining) noexcept {
        return request.slot != 0xffffU
            ? backend_.select_satellite_slot(
                  receiver, static_cast<std::uint8_t>(request.slot), remaining)
            : backend_.select_satellite_tsid(receiver, request.stream_id, remaining);
    };
    if (select_before_tune) {
        const std::uint32_t select_timeout = remaining_before_tune();
        if (select_timeout == 0U)
            return fail_after_power(Error::TIMEOUT);
        const auto selected = select_satellite_stream(select_timeout);
        if (!selected)
            return fail_after_power(selected.error());
        const Error gate = stage_gate();
        if (gate != Error::OK) return fail_after_power(gate);
    }
    // Recompute the relative timeout after the last potentially blocking gate
    // so registry-wait time is consumed even when a positive budget remains.
    {
        const Error gate = stage_gate();
        if (gate != Error::OK) return fail_after_power(gate);
    }
    const std::uint32_t tune_timeout = remaining_before_tune();
    if (tune_timeout == 0U)
        return fail_after_power(Error::TIMEOUT);
    // The only entrance-specific stage.  The legacy entrance passes back the
    // exact legacy kHz value (requested_hz is that value times 1000).
    TuneProgramming programming{};
    if (request.entrance == TuneEntrance::legacy_khz) {
        const auto tuned = backend_.tune(
            receiver, request.system, static_cast<std::uint32_t>(request.requested_hz / 1000U),
            tune_timeout);
        if (!tuned)
            return fail_after_power(tuned.error());
    } else {
        const auto programmed =
            backend_.program_hz(receiver, request.system, request.requested_hz, tune_timeout);
        if (!programmed)
            return fail_after_power(programmed.error());
        programming = programmed.value();
    }
    // A cancellation/budget observed after the returned frontend tune stops
    // the next stage without starting further hardware I/O.
    {
        const Error gate = stage_gate();
        if (gate != Error::OK) return fail_after_power(gate);
    }

    // The legacy entrance always polls lock, as before.
    const TuneReadiness readiness = request.entrance == TuneEntrance::legacy_khz
        ? TuneReadiness::lock_poll
        : strict_capabilities_[slot].readiness;
    wire::Verification verification = wire::Verification::lock;
    wire::Measurement<bool> lock_evidence{};
    // Keep this origin separate from start_ms: the settle interval starts
    // when the lock check begins, after the frontend tune has completed.
    const std::uint64_t lock_poll_start_ms = clock_.monotonic_ms();
    if (readiness == TuneReadiness::lock_poll) {
        std::uint64_t sampled_ms = 0U;
        const auto locked = poll_lock(slot, request.system, start_ms, request.timeout_ms,
                                      admitted, ticket, &sampled_ms);
        if (!locked) return fail_after_power(locked.error());
        if (!locked.value())
            return fail_after_power(Error::TIMEOUT);
        // Valid only after the real predicate returned true; the sample time
        // is that poll's engine-clock reading, converted with a range check.
        lock_evidence.value = true;
        if (sampled_ms <= std::numeric_limits<std::uint64_t>::max() / 1000000U) {
            lock_evidence.state = wire::MeasureState::valid;
            lock_evidence.sampled_at_ns = sampled_ms * 1000000U;
        } else {
            lock_evidence.state = wire::MeasureState::unknown;
            lock_evidence.reason = wire::UnknownReason::not_sampled;
            lock_evidence.value = false;
        }
    } else if (readiness == TuneReadiness::terminal_lock) {
        // The backend's own bounded poll for this exact invocation; no fake
        // second poll and no promotion of missing evidence to a lock.
        if (programming.lock.state != wire::MeasureState::valid)
            return fail_after_power(Error::PROTOCOL_ERROR);
        if (!programming.lock.value)
            return fail_after_power(Error::TIMEOUT);
        lock_evidence = programming.lock;
    } else if (readiness == TuneReadiness::commands) {
        // Command completion only: never a lock claim.
        verification = wire::Verification::commands;
        lock_evidence.state = strict_capabilities_[slot].lock_measurement
            ? wire::MeasureState::unknown
            : wire::MeasureState::unsupported;
        lock_evidence.reason = wire::UnknownReason::not_sampled;
    } else {
        // ts_sync needs actual synchronized-TS evidence from this invocation.
        if (!programming.ts_synchronized)
            return fail_after_power(Error::TIMEOUT);
        verification = wire::Verification::ts_sync;
        lock_evidence = programming.lock;
    }
    // A poll or backend report can return with a late or revoked ticket.
    {
        const Error gate = stage_gate();
        if (gate != Error::OK) return fail_after_power(gate);
    }

    const std::uint32_t settle_after_lock_ms =
        backend_.lock_settle_ms(receiver, request.system);
    if (settle_after_lock_ms != 0U) {
        const std::uint64_t now = clock_.monotonic_ms();
        const std::uint64_t elapsed_total = now - start_ms;
        if (elapsed_total >= request.timeout_ms)
            return fail_after_power(Error::TIMEOUT);

        const std::uint64_t lock_poll_elapsed = now - lock_poll_start_ms;
        if (lock_poll_elapsed < settle_after_lock_ms) {
            const std::uint32_t settle_ms = static_cast<std::uint32_t>(
                settle_after_lock_ms - lock_poll_elapsed);
            const std::uint32_t remaining = static_cast<std::uint32_t>(
                request.timeout_ms - elapsed_total);
            if (remaining <= settle_ms)
                return fail_after_power(Error::TIMEOUT);
            clock_.sleep_ms(settle_ms);
        }
    }

    // After settle (which may oversleep) and before the next stage.
    {
        const Error gate = stage_gate();
        if (gate != Error::OK) return fail_after_power(gate);
    }

    if (request.system == ReceiverSystem::satellite && !select_before_tune) {
        const std::uint64_t elapsed = clock_.monotonic_ms() - start_ms;
        if (elapsed >= request.timeout_ms)
            return fail_after_power(Error::TIMEOUT);
        const auto remaining = static_cast<std::uint32_t>(
            request.timeout_ms - elapsed);
        const auto selected = select_satellite_stream(remaining);
        if (!selected)
            return fail_after_power(selected.error());
        // Preserve the legacy direct post-select deadline check exactly.
        if (!admitted &&
            clock_.monotonic_ms() - start_ms >= request.timeout_ms)
            return fail_after_power(Error::TIMEOUT);
        const Error gate = stage_gate();
        if (gate != Error::OK) return fail_after_power(gate);
    }

    // Immediate pre-commit gate: a cancellation or exhausted budget found here
    // is still a pending-transaction failure, so rollback is valid.
    {
        const Error gate = stage_gate();
        if (gate != Error::OK) return fail_after_power(gate);
    }

    const auto committed = backend_.commit_tune_power(receiver);
    if (!committed) return fail_after_power(committed.error());

    if (admitted) {
        // The final exact-ticket/owner/revision and deadline decision and the
        // success/history publication happen in ONE registry critical section.
        // The deadline is sampled after acquiring the lock, so registry
        // acquisition time is accounted for, and a revocation or release that
        // linearized in the interim cannot be overwritten by a stale success.
        std::lock_guard<std::mutex> lock(mutex_);
        if (!ticket_owns_slot_locked(slot, ticket)) {
            // A newer admission owns the slot; leave it completely untouched.
            return Result<void>::failure(Error::NOT_FOUND);
        }
        const bool current = pending_[slot].valid &&
                             pending_[slot].ticket.tune_revision == revision &&
                             ticket_current_locked(pending_[slot].ticket);
        const bool late =
            clock_.monotonic_ms() - accepted_at_ms >= request.timeout_ms;
        if (!current || late) {
            // The power transaction was already consumed by a successful
            // commit.  Ordinary pending rollback is invalid (Q3/MLT reject it)
            // or a no-op (A), so it must not be called or interpreted as
            // compensation.  Quarantine the receiver: preserve the primary
            // TIMEOUT/owner-cancellation error, keep current invalid, retain
            // the lease's power reference, and let the existing exact RELEASE
            // or owner cleanup perform the normal close.
            set_state_locked(slot, ReceiverState::error);
            complete_tune_locked(slot, ticket);
            return Result<void>::failure(current ? Error::TIMEOUT : Error::NOT_FOUND);
        }
        // receiver_lock keeps this lease in place since it was copied above.
        leases_[slot].system = request.system;
        // A successful tune re-arms a lease whose previous stream ended:
        // STOP leaves the lease consumed, and a new stream start must be
        // possible again without release/acquire.  The attach token stays the
        // one issued at acquire; its validity is per arm.
        if (leases_[slot].stream_state == StreamState::consumed)
            leases_[slot].stream_state = StreamState::none;
        // Request, programmed value and evidence are published only here.
        TuneMetadata& metadata = tune_metadata_[slot];
        const TuneRecord record =
            tune_record(request, programming, verification, lock_evidence);
        metadata.revision = revision;
        metadata.current = record;
        metadata.current_revision = revision;
        metadata.current_valid = true;
        metadata.last_successful = record;
        metadata.last_successful_revision = revision;
        metadata.last_successful_valid = true;
        complete_tune_locked(slot, ticket);
        set_state_locked(slot, ReceiverState::tuned);
        return Result<void>::success();
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        // Strict cleanup exclusion: if logical owner cleanup linearized during
        // the finite direct tune, do not publish a reusable/`tuned` success.
        // The cleanup marker and the lease stay in place for the exact release.
        // Nonstrict legacy direct behaviour is retained unchanged.
        if (strict_enabled_ &&
            operations_[slot] == ReceiverTuneOperation::cleanup)
            return Result<void>::failure(Error::NOT_FOUND);
        // receiver_lock keeps this lease in place since it was copied above.
        leases_[slot].system = request.system;
        // A successful tune re-arms a lease whose previous stream ended:
        // STOP leaves the lease consumed, and a new stream start must be
        // possible again without release/acquire.  The attach token stays the
        // one issued at acquire; its validity is per arm.
        if (leases_[slot].stream_state == StreamState::consumed)
            leases_[slot].stream_state = StreamState::none;
        set_state_locked(slot, ReceiverState::tuned);
    }
    return Result<void>::success();
}

Result<void> ReceiverEngine::start_stream(std::uint64_t client_id,
                                          std::uint64_t lease_id) noexcept
{
    if (!valid_client(client_id) || lease_id == 0U)
        return Result<void>::failure(Error::NOT_FOUND);
    std::lock_guard<std::mutex> lock(mutex_);
    const int index = find_lease_locked(client_id, lease_id);
    if (index < 0) return Result<void>::failure(Error::NOT_FOUND);
    Lease& lease = leases_[static_cast<std::size_t>(index)];
    if (states_[lease.slot] != ReceiverState::tuned)
        return Result<void>::failure(Error::NOT_READY);
    // A strict receiver under logical cleanup must not arm a new stream.
    if (strict_enabled_ &&
        operations_[lease.slot] != ReceiverTuneOperation::idle)
        return Result<void>::failure(Error::NOT_READY);
    if (lease.stream_state != StreamState::none)
        return Result<void>::failure(Error::BUSY);
    lease.attach_started_ms = clock_.monotonic_ms();
    lease.stream_state = StreamState::armed;
    return Result<void>::success();
}

Result<Attachment> ReceiverEngine::attach_stream(std::uint64_t lease_id,
                                                 const ReceiverToken& token) noexcept
{
    if (lease_id == 0U)
        return Result<Attachment>::failure(Error::NOT_FOUND);

    std::size_t slot = 0U;
    Lease pending;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        int index = -1;
        for (std::size_t candidate = 0U; candidate < leases_.size(); ++candidate) {
            const Lease& lease = leases_[candidate];
            if (lease.active && lease.lease_id == lease_id && lease.token == token) {
                index = static_cast<int>(candidate);
                break;
            }
        }
        if (index < 0) return Result<Attachment>::failure(Error::NOT_FOUND);
        Lease& lease = leases_[static_cast<std::size_t>(index)];
        slot = lease.slot;
    }

    std::unique_lock<std::mutex> receiver_lock(receiver_mutexes_[slot]);
    std::uint64_t attachment_id = 0U;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        int index = -1;
        for (std::size_t candidate = 0U; candidate < leases_.size(); ++candidate) {
            const Lease& lease = leases_[candidate];
            if (lease.active && lease.lease_id == lease_id && lease.token == token) {
                index = static_cast<int>(candidate);
                break;
            }
        }
        if (index < 0) return Result<Attachment>::failure(Error::NOT_FOUND);
        Lease& lease = leases_[static_cast<std::size_t>(index)];
        // A strict receiver under logical cleanup must not complete ATTACH.
        if (strict_enabled_ && operations_[slot] != ReceiverTuneOperation::idle)
            return Result<Attachment>::failure(Error::NOT_READY);
        if (lease.slot != slot || lease.stream_state != StreamState::armed)
            return Result<Attachment>::failure(
                lease.stream_state == StreamState::none
                    ? Error::NOT_READY : Error::NOT_FOUND);
        // Unsigned subtraction is defined modulo 2^64 and therefore remains
        // correct when a monotonic counter wraps during the five-second arm.
        if (clock_.monotonic_ms() - lease.attach_started_ms >= 5000U) {
            lease.stream_state = StreamState::consumed;
            return Result<Attachment>::failure(Error::TIMEOUT);
        }
        const auto allocated = allocate_attachment_id_locked(slot);
        if (!allocated)
            return Result<Attachment>::failure(allocated.error());
        attachment_id = allocated.value();
        lease.stream_state = StreamState::attaching;
        pending = lease;
    }
    const ReceiverId receiver = receivers_[slot];
    const Attachment pending_attachment = attachment_for(pending, attachment_id);
    const auto started = backend_.start_capture(receiver, pending.system);
    observe_disconnect(slot, started.error());
    bool data_attached = false;
    Error data_error = Error::OK;
    if (started && stream_control_ != nullptr) {
        const auto attached = stream_control_->attach(pending_attachment);
        observe_disconnect(slot, attached.error());
        if (!attached) {
            data_error = attached.error();
        } else {
            data_attached = true;
        }
    }
    if (!started || data_error != Error::OK) {
        Error result_error = !started ? started.error() : data_error;
        bool data_maybe_attached = false;
        pending.capture_maybe_active = static_cast<bool>(started);
        pending.backend_disconnected = started.error() == Error::DISCONNECTED ||
                                       data_error == Error::DISCONNECTED;
        if (data_attached || (started && stream_control_ != nullptr)) {
            const auto detached = stream_control_->detach(pending_attachment);
            observe_disconnect(slot, detached.error());
            data_maybe_attached = !detached && detached.error() != Error::NOT_FOUND;
            if (detached.error() == Error::DISCONNECTED)
                pending.backend_disconnected = true;
        }
        if (!pending.backend_disconnected) {
            const auto stopped = backend_.stop_capture(receiver, pending.system);
            observe_disconnect(slot, stopped.error());
            pending.backend_disconnected = stopped.error() == Error::DISCONNECTED;
            pending.capture_maybe_active = !stopped;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        reserved_attachment_ids_[slot] = 0U;
        const int index = find_lease_locked(pending.client_id, pending.lease_id);
        if (index >= 0) {
            Lease& lease = leases_[static_cast<std::size_t>(index)];
            lease.stream_state = StreamState::consumed;
            // Keep the identity when compensating detach failed.  The
            // subsequent lease release must retry that exact data session.
            lease.attachment_id = data_maybe_attached ? attachment_id : 0U;
            lease.capture_maybe_active = pending.capture_maybe_active;
            lease.stream_maybe_attached = data_maybe_attached;
            lease.backend_disconnected = pending.backend_disconnected;
            if (pending.capture_maybe_active || data_maybe_attached ||
                pending.backend_disconnected)
                set_state_locked(slot, ReceiverState::error);
        }
        return Result<Attachment>::failure(result_error);
    }

    bool attach_accepted = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const int index = find_lease_locked(pending.client_id, pending.lease_id);
        if (index >= 0) {
            Lease& lease = leases_[static_cast<std::size_t>(index)];
            if (lease.stream_state == StreamState::attaching) {
                lease.stream_state = StreamState::active;
                lease.attachment_id = attachment_id;
                lease.capture_maybe_active = true;
                lease.stream_maybe_attached = data_attached;
                set_state_locked(slot, ReceiverState::streaming);
                attach_accepted = true;
            }
        }
    }
    if (attach_accepted) {
        std::lock_guard<std::mutex> lock(mutex_);
        reserved_attachment_ids_[slot] = 0U;
        const Lease& lease = leases_[slot];
        return Result<Attachment>::success(attachment_for(lease, lease.attachment_id));
    }

    // revoke_stream() won the race while start_capture was in flight.  The
    // physical start must be compensated before ATTACH can complete.
    Error result_error = Error::NOT_FOUND;
    const auto detached = data_attached && stream_control_ != nullptr
        ? stream_control_->detach(pending_attachment)
        : Result<void>::success();
    observe_disconnect(slot, detached.error());
    const bool data_maybe_attached =
        !detached && detached.error() != Error::NOT_FOUND;
    if (!detached && detached.error() != Error::NOT_FOUND)
        result_error = detached.error();
    Result<void> stopped = Result<void>::failure(Error::DISCONNECTED);
    if (detached.error() != Error::DISCONNECTED) {
        stopped = backend_.stop_capture(receiver, pending.system);
        observe_disconnect(slot, stopped.error());
    }
    if (!stopped && result_error == Error::NOT_FOUND) result_error = stopped.error();
    if (stopped.error() == Error::DISCONNECTED) result_error = Error::DISCONNECTED;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        reserved_attachment_ids_[slot] = 0U;
        const int index = find_lease_locked(pending.client_id, pending.lease_id);
        if (index >= 0) {
            Lease& lease = leases_[static_cast<std::size_t>(index)];
            lease.stream_state = StreamState::consumed;
            lease.attachment_id = data_maybe_attached ? attachment_id : 0U;
            lease.capture_maybe_active = !stopped;
            lease.stream_maybe_attached = data_maybe_attached;
            lease.backend_disconnected = stopped.error() == Error::DISCONNECTED;
            set_state_locked(slot, stopped && !data_maybe_attached
                                       ? ReceiverState::tuned
                                       : ReceiverState::error);
        }
    }
    return Result<Attachment>::failure(result_error);
}

Result<StreamFinalSnapshot> ReceiverEngine::stop_stream(
    std::uint64_t client_id, std::uint64_t lease_id) noexcept
{
    if (!valid_client(client_id) || lease_id == 0U)
        return Result<StreamFinalSnapshot>::failure(Error::NOT_FOUND);
    std::size_t slot = 0U;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const int index = find_lease_locked(client_id, lease_id);
        if (index < 0)
            return Result<StreamFinalSnapshot>::failure(Error::NOT_FOUND);
        slot = leases_[static_cast<std::size_t>(index)].slot;
    }
    std::unique_lock<std::mutex> receiver_lock(receiver_mutexes_[slot]);
    Lease pending;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const int index = find_lease_locked(client_id, lease_id);
        if (index < 0)
            return Result<StreamFinalSnapshot>::failure(Error::NOT_FOUND);
        pending = leases_[static_cast<std::size_t>(index)];
        if (pending.slot != slot ||
            (pending.stream_state != StreamState::active &&
             pending.stream_state != StreamState::revoked) ||
            !pending.capture_maybe_active)
            return Result<StreamFinalSnapshot>::failure(Error::NOT_READY);
    }
    const Attachment attachment = attachment_for(pending, pending.attachment_id);
    const auto detached = detach_data_plane_if_needed(pending);
    const bool detach_clean = detached || detached.error() == Error::NOT_FOUND;
    Error first = Error::OK;
    if (!detach_clean) first = detached.error();
    Result<void> stopped = Result<void>::failure(Error::DISCONNECTED);
    if (detached.error() != Error::DISCONNECTED) {
        stopped = backend_.stop_capture(receivers_[slot], pending.system);
        observe_disconnect(slot, stopped.error());
    }
    if (!stopped && first == Error::OK) first = stopped.error();
    StreamFinalSnapshot final_snapshot{};
    bool snapshot_disconnected = false;
    if (detach_clean && stream_control_ != nullptr &&
        pending.stream_maybe_attached) {
        const auto snapshot = stream_control_->final_snapshot(attachment);
        if (snapshot) {
            final_snapshot = snapshot.value();
            snapshot_disconnected = final_snapshot.terminal == kStreamTerminalDisconnected;
            if (snapshot_disconnected)
                observe_disconnect(slot, Error::DISCONNECTED);
        } else {
            snapshot_disconnected = snapshot.error() == Error::DISCONNECTED;
            observe_disconnect(slot, snapshot.error());
            if (first == Error::OK) first = snapshot.error();
        }
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const int index = find_lease_locked(client_id, lease_id);
        if (index >= 0) {
            Lease& lease = leases_[static_cast<std::size_t>(index)];
            lease.stream_state = StreamState::consumed;
            lease.attachment_id = detach_clean ? 0U : pending.attachment_id;
            lease.capture_maybe_active = !stopped;
            lease.stream_maybe_attached = !detach_clean;
            lease.backend_disconnected = detached.error() == Error::DISCONNECTED ||
                                         stopped.error() == Error::DISCONNECTED ||
                                         snapshot_disconnected;
            set_state_locked(slot, stopped && detach_clean && !snapshot_disconnected
                                       ? ReceiverState::tuned
                                       : ReceiverState::error);
        }
    }
    return first == Error::OK
        ? Result<StreamFinalSnapshot>::success(final_snapshot)
        : Result<StreamFinalSnapshot>::failure(first);
}

Result<StreamCounters> ReceiverEngine::stats(std::uint64_t client_id,
                                             std::uint64_t lease_id) const noexcept
{
    if (!valid_client(client_id) || lease_id == 0U)
        return Result<StreamCounters>::failure(Error::NOT_FOUND);
    std::size_t slot = 0U;
    Lease lease;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const int index = find_lease_locked(client_id, lease_id);
        if (index < 0)
            return Result<StreamCounters>::failure(Error::NOT_FOUND);
        lease = leases_[static_cast<std::size_t>(index)];
        slot = lease.slot;
    }
    std::unique_lock<std::mutex> receiver_lock(receiver_mutexes_[slot]);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const int index = find_lease_locked(client_id, lease_id);
        if (index < 0)
            return Result<StreamCounters>::failure(Error::NOT_FOUND);
        lease = leases_[static_cast<std::size_t>(index)];
        if (lease.slot != slot || lease.stream_state != StreamState::active ||
            !lease.capture_maybe_active)
            return Result<StreamCounters>::failure(Error::NOT_READY);
        if (stream_control_ == nullptr || !lease.stream_maybe_attached)
            return Result<StreamCounters>::failure(Error::UNSUPPORTED);
    }
    return stream_control_->stats(attachment_for(lease, lease.attachment_id));
}

Result<void> ReceiverEngine::detach_stream(const Attachment& attachment) noexcept
{
    const int found = find_slot(attachment.receiver);
    if (attachment.owner_client_id == 0U || attachment.lease_id == 0U ||
        attachment.attachment_id == 0U || found < 0)
        return Result<void>::failure(Error::NOT_FOUND);
    const auto slot = static_cast<std::size_t>(found);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const Lease& lease = leases_[slot];
        if (!lease.active || lease.client_id != attachment.owner_client_id ||
            lease.lease_id != attachment.lease_id ||
            lease.attachment_id != attachment.attachment_id ||
            lease.system != attachment.system || lease.token != attachment.token ||
            (lease.stream_state != StreamState::active &&
             lease.stream_state != StreamState::revoked))
            return Result<void>::failure(Error::NOT_FOUND);
        leases_[slot].stream_state = StreamState::revoked;
    }
    std::unique_lock<std::mutex> receiver_lock(receiver_mutexes_[slot]);
    Lease pending;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const Lease& lease = leases_[slot];
        if (!lease.active || lease.client_id != attachment.owner_client_id ||
            lease.lease_id != attachment.lease_id ||
            lease.attachment_id != attachment.attachment_id ||
            lease.system != attachment.system || lease.token != attachment.token ||
            (lease.stream_state != StreamState::active &&
             lease.stream_state != StreamState::revoked))
            return Result<void>::failure(Error::NOT_FOUND);
        pending = lease;
    }
    const auto detached = detach_data_plane_if_needed(pending);
    const bool detach_clean = detached || detached.error() == Error::NOT_FOUND;
    Error first = Error::OK;
    if (!detach_clean) first = detached.error();
    Result<void> stopped = Result<void>::failure(Error::DISCONNECTED);
    if (detached.error() != Error::DISCONNECTED) {
        stopped = backend_.stop_capture(receivers_[slot], pending.system);
        observe_disconnect(slot, stopped.error());
    }
    if (!stopped && first == Error::OK) first = stopped.error();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const int index = find_lease_locked(attachment.owner_client_id,
                                            attachment.lease_id);
        if (index >= 0 && leases_[static_cast<std::size_t>(index)].attachment_id ==
                              attachment.attachment_id) {
            Lease& lease = leases_[static_cast<std::size_t>(index)];
            lease.stream_state = StreamState::consumed;
            lease.attachment_id = detach_clean ? 0U : attachment.attachment_id;
            lease.capture_maybe_active = !stopped;
            lease.stream_maybe_attached = !detach_clean;
            lease.backend_disconnected = detached.error() == Error::DISCONNECTED ||
                                         stopped.error() == Error::DISCONNECTED;
            set_state_locked(slot, stopped && detach_clean
                                       ? ReceiverState::tuned
                                       : ReceiverState::error);
        }
    }
    return first == Error::OK ? Result<void>::success()
                              : Result<void>::failure(first);
}

Result<void> ReceiverEngine::revoke_stream(std::uint64_t client_id,
                                           std::uint64_t lease_id) noexcept
{
    if (!valid_client(client_id) || lease_id == 0U)
        return Result<void>::failure(Error::NOT_FOUND);
    std::lock_guard<std::mutex> lock(mutex_);
    const int index = find_lease_locked(client_id, lease_id);
    if (index < 0) return Result<void>::failure(Error::NOT_FOUND);
    Lease& lease = leases_[static_cast<std::size_t>(index)];
    if (lease.stream_state == StreamState::armed ||
        lease.stream_state == StreamState::attaching ||
        lease.stream_state == StreamState::active)
        lease.stream_state = StreamState::revoked;
    return Result<void>::success();
}

Result<void> ReceiverEngine::revoke_stream(std::uint64_t client_id,
                                           std::uint64_t lease_id,
                                           const ReceiverToken& token) noexcept
{
    if (!valid_client(client_id) || lease_id == 0U)
        return Result<void>::failure(Error::NOT_FOUND);
    std::lock_guard<std::mutex> lock(mutex_);
    const int index = find_lease_locked(client_id, lease_id);
    if (index < 0) return Result<void>::failure(Error::NOT_FOUND);
    Lease& lease = leases_[static_cast<std::size_t>(index)];
    if (lease.token != token) return Result<void>::failure(Error::NOT_FOUND);
    if (lease.stream_state == StreamState::armed ||
        lease.stream_state == StreamState::attaching ||
        lease.stream_state == StreamState::active)
        lease.stream_state = StreamState::revoked;
    return Result<void>::success();
}

Result<ReceiverEngineStatus> ReceiverEngine::status() const noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);
    ReceiverEngineStatus status{};
    status.generation = generation_;
    status.receiver_count = receiver_count_;
    status.receivers = receivers_;
    status.states = states_;
    status.operations = operations_;
    status.tune_metadata = tune_metadata_;
    for (std::size_t slot = 0U; slot < leases_.size(); ++slot)
        status.leased[slot] = leases_[slot].active;
    return Result<ReceiverEngineStatus>::success(status);
}

Result<void> ReceiverEngine::cleanup_client(std::uint64_t client_id) noexcept
{
    if (!valid_client(client_id)) return Result<void>::failure(Error::INVALID_ARGUMENT);
    std::array<std::uint64_t, kReceiverEngineCapacity> lease_ids{};
    // Revoke the complete connection atomically before any receiver I/O.  A
    // slow cleanup on one bridge must not leave another lease attachable.
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (std::size_t slot = 0U; slot < leases_.size(); ++slot) {
            Lease& lease = leases_[slot];
            if (!lease.active || lease.client_id != client_id) continue;
            lease_ids[slot] = lease.lease_id;
            if (lease.stream_state == StreamState::armed ||
                lease.stream_state == StreamState::attaching ||
                lease.stream_state == StreamState::active)
                lease.stream_state = StreamState::revoked;
            // Revoke every targeted pending ticket and invalidate current in
            // this first metadata phase, before any blocking receiver cleanup.
            clear_pending_locked(slot);
            invalidate_current_locked(slot);
            set_operation_locked(slot, ReceiverTuneOperation::cleanup,
                                 lease.instance, 0U);
        }
    }
    Error first = Error::OK;
    for (std::size_t slot = 0U; slot < lease_ids.size(); ++slot) {
        if (lease_ids[slot] == 0U) continue;
        const auto result = release_lease(client_id, lease_ids[slot]);
        if (first == Error::OK && !result) first = result.error();
    }
    return first == Error::OK ? Result<void>::success()
                              : Result<void>::failure(first);
}

Result<void> ReceiverEngine::disconnect_client(std::uint64_t client_id) noexcept
{
    return cleanup_client(client_id);
}

Result<void> ReceiverEngine::cleanup_all() noexcept
{
    std::array<std::uint64_t, kReceiverEngineCapacity> client_ids{};
    std::array<std::uint64_t, kReceiverEngineCapacity> lease_ids{};
    // Same two-phase rule as connection cleanup: revoke all logical stream
    // credentials before the first potentially blocking backend operation.
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (std::size_t slot = 0U; slot < leases_.size(); ++slot) {
            Lease& lease = leases_[slot];
            if (!lease.active) continue;
            client_ids[slot] = lease.client_id;
            lease_ids[slot] = lease.lease_id;
            if (lease.stream_state == StreamState::armed ||
                lease.stream_state == StreamState::attaching ||
                lease.stream_state == StreamState::active)
                lease.stream_state = StreamState::revoked;
            // Revoke every pending ticket and invalidate current before the
            // first potentially blocking backend operation.
            clear_pending_locked(slot);
            invalidate_current_locked(slot);
            set_operation_locked(slot, ReceiverTuneOperation::cleanup,
                                 lease.instance, 0U);
        }
    }
    Error first = Error::OK;
    for (std::size_t slot = 0U; slot < client_ids.size(); ++slot) {
        if (client_ids[slot] == 0U) continue;
        const auto result = release_lease(client_ids[slot], lease_ids[slot]);
        if (first == Error::OK && !result) first = result.error();
    }
    const auto backend_shutdown = backend_.shutdown();
    if (first == Error::OK && !backend_shutdown) first = backend_shutdown.error();
    return first == Error::OK ? Result<void>::success()
                              : Result<void>::failure(first);
}

Result<void> ReceiverEngine::shutdown() noexcept
{
    return cleanup_all();
}

}  // namespace dtv::userland::detail
