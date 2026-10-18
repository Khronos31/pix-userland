// SPDX-License-Identifier: MIT
#include "dtv/userland/detail/card_ownership_engine.h"

#include <cstddef>
#include <cstdint>
#include <limits>

namespace dtv::userland::detail {

// Compatibility implementation retains the recorded operation order.

template <typename Snapshot>
CardOwnershipEngine<Snapshot>::~CardOwnershipEngine() noexcept
{
    (void)shutdown();
}

template <typename Snapshot>
void CardOwnershipEngine<Snapshot>::invalidate_session() noexcept
{
    session_.invalidate();
    cached_atr_ = Snapshot{};
    transaction_handle_ = 0U;
}

template <typename Snapshot>
void CardOwnershipEngine<Snapshot>::invalidate_bound_handles() noexcept
{
    // Sticky per-handle invalidation cannot accidentally match a later card
    // after reader_generation wraps. Legacy handles retain transparent recovery.
    for (HandleRecord& record : handles_) {
        if (record.generation_bound) record.stale = true;
    }
}

template <typename Snapshot>
void CardOwnershipEngine<Snapshot>::abandon_disconnected_hardware() noexcept
{
    // Once the backend reports removal, drop service-side state without I/O.
    powered_ = false;
    uart_initialized_ = false;
    invalidate_bound_handles();
    invalidate_session();
}

template <typename Snapshot>
Result<void> CardOwnershipEngine<Snapshot>::power_up() noexcept
{
    if (powered_) {
        return Result<void>::success();
    }
    const auto power = backend_.set_power(true);
    if (!power) {
        if (power.error() == Error::DISCONNECTED) {
            abandon_disconnected_hardware();
            return power;
        }
        // A failed power request may still have reached the backend.
        const auto off = backend_.set_power(false);
        return off ? power : off;
    }
    powered_ = true;
    const auto uart = backend_.initialize_uart();
    if (!uart) {
        if (uart.error() == Error::DISCONNECTED) {
            abandon_disconnected_hardware();
            return uart;
        }
        const auto off = backend_.set_power(false);
        powered_ = false;
        uart_initialized_ = false;
        return off ? uart : off;
    }
    uart_initialized_ = true;
    return Result<void>::success();
}

template <typename Snapshot>
Result<void> CardOwnershipEngine<Snapshot>::power_down() noexcept
{
    invalidate_session();
    uart_initialized_ = false;
    if (!powered_) {
        return Result<void>::success();
    }
    // Clear the logical state before I/O so a failed OFF is never mistaken for
    // a reusable powered card session.
    powered_ = false;
    return backend_.set_power(false);
}

template <typename Snapshot>
Result<bool> CardOwnershipEngine<Snapshot>::detect_and_record(bool report_initial_change,
                                            CardOwnershipPresenceChange* change) noexcept
{
    if (!powered_ || !uart_initialized_) {
        return Result<bool>::failure(Error::NOT_READY);
    }
    const auto detected = backend_.detect_card();
    if (!detected) {
        if (detected.error() == Error::NO_CARD ||
            detected.error() == Error::CARD_REMOVED) invalidate_bound_handles();
        if (detected.error() == Error::DISCONNECTED) {
            abandon_disconnected_hardware();
        } else {
            invalidate_session();
        }
        return Result<bool>::failure(detected.error());
    }

    const bool was_known = presence_known_;
    const bool previous = present_;
    presence_known_ = true;
    present_ = detected.value();
    if (!present_) invalidate_bound_handles();
    const bool changed = was_known ? previous != present_ : report_initial_change;
    if (changed) {
        ++reader_generation_;
        pending_presence_change_ =
            CardOwnershipPresenceChange{true, present_, reader_generation_};
        presence_change_pending_ = true;
        if (!present_) {
            invalidate_session();
        }
    }
    if (change != nullptr) {
        *change = CardOwnershipPresenceChange{changed, present_, reader_generation_};
    }
    return Result<bool>::success(present_);
}

template <typename Snapshot>
Result<void> CardOwnershipEngine<Snapshot>::ensure_session() noexcept
{
    const auto power = power_up();
    if (!power) {
        return power;
    }
    if (session_.initialized()) {
        const auto detected = detect_and_record(false, nullptr);
        if (!detected) {
            return Result<void>::failure(detected.error());
        }
        if (!detected.value()) {
            return Result<void>::failure(Error::CARD_REMOVED);
        }
        return Result<void>::success();
    }
    const auto initialized = session_.initialize();
    if (!initialized) {
        if (initialized.error() == Error::NO_CARD ||
            initialized.error() == Error::CARD_REMOVED) invalidate_bound_handles();
        if (initialized.error() == Error::DISCONNECTED) {
            abandon_disconnected_hardware();
        } else {
            invalidate_session();
        }
        return initialized;
    }
    if (presence_known_ && !present_) {
        ++reader_generation_;
        pending_presence_change_ =
            CardOwnershipPresenceChange{true, true, reader_generation_};
        presence_change_pending_ = true;
    }
    presence_known_ = true;
    present_ = true;
    cached_atr_ = session_.atr();
    return Result<void>::success();
}

template <typename Snapshot>
Result<void> CardOwnershipEngine<Snapshot>::ensure_handle_session(std::size_t index) noexcept
{
    const HandleRecord& record = handles_[index];
    if (record.stale) return Result<void>::failure(Error::CARD_REMOVED);
    // An uninitialized protocol session may follow a previous I/O failure.
    // Detect an absent card before reinitializing it for a bound handle.
    if (record.generation_bound && !session_.initialized()) {
        const auto power = power_up();
        if (!power) return power;
        const auto detected = detect_and_record(false, nullptr);
        if (!detected) return Result<void>::failure(detected.error());
        if (!detected.value()) return Result<void>::failure(Error::CARD_REMOVED);
    }
    const auto ready = ensure_session();
    if (!ready) return ready;
    return record.stale ? Result<void>::failure(Error::CARD_REMOVED) :
                          Result<void>::success();
}

template <typename Snapshot>
Result<Snapshot> CardOwnershipEngine<Snapshot>::reset_session(bool preserve_transaction) noexcept
{
    const std::uint64_t transaction = preserve_transaction ? transaction_handle_ : 0U;
    invalidate_session();
    const auto initialized = ensure_session();
    if (!initialized) {
        return Result<Snapshot>::failure(initialized.error());
    }
    transaction_handle_ = transaction;
    return Result<Snapshot>::success(cached_atr_);
}

template <typename Snapshot>
Result<CardOwnershipStatus<Snapshot>> CardOwnershipEngine<Snapshot>::status() noexcept
{
    const bool temporary_power = !powered_;
    const auto power = power_up();
    if (!power) {
        return Result<CardOwnershipStatus<Snapshot>>::failure(power.error());
    }
    const auto detected = detect_and_record(false, nullptr);
    Error error = detected ? Error::OK : detected.error();
    CardOwnershipStatus<Snapshot> result{
        detected ? detected.value() : false,
        detected && detected.value() && session_.initialized(),
        reader_generation_,
        detected && detected.value() && session_.initialized() ? cached_atr_ : Snapshot{}};
    if (temporary_power) {
        const auto off = power_down();
        if (!off) {
            error = off.error();
        }
        result.initialized = false;
        result.atr = Snapshot{};
    }
    return error == Error::OK ? Result<CardOwnershipStatus<Snapshot>>::success(result) :
                               Result<CardOwnershipStatus<Snapshot>>::failure(error);
}

template <typename Snapshot>
Result<CardOwnershipStatus<Snapshot>> CardOwnershipEngine<Snapshot>::handle_status(std::uint64_t client,
                                                     std::uint64_t handle) noexcept
{
    const auto found = find_handle(client, handle);
    if (!found) return Result<CardOwnershipStatus<Snapshot>>::failure(found.error());
    if (handles_[found.value()].stale)
        return Result<CardOwnershipStatus<Snapshot>>::failure(Error::CARD_REMOVED);
    const auto snapshot = status();
    // status() may observe loss and invalidate the handle. This whole call is
    // serialized on the same card lane as connect/transmit/removal polling.
    if (handles_[found.value()].stale)
        return Result<CardOwnershipStatus<Snapshot>>::failure(Error::CARD_REMOVED);
    if (!snapshot) return snapshot;
    if (!snapshot.value().present)
        return Result<CardOwnershipStatus<Snapshot>>::failure(Error::CARD_REMOVED);
    if (!snapshot.value().initialized)
        return Result<CardOwnershipStatus<Snapshot>>::failure(Error::NOT_READY);
    return snapshot;
}

template <typename Snapshot>
Result<CardOwnershipPresenceChange> CardOwnershipEngine<Snapshot>::poll_presence() noexcept
{
    if (!powered_ || handles_.empty()) {
        if (presence_change_pending_) {
            presence_change_pending_ = false;
            return Result<CardOwnershipPresenceChange>::success(pending_presence_change_);
        }
        return Result<CardOwnershipPresenceChange>::success(
            CardOwnershipPresenceChange{false, present_, reader_generation_});
    }
    CardOwnershipPresenceChange change{};
    const auto detected = detect_and_record(false, &change);
    if (!detected) {
        return Result<CardOwnershipPresenceChange>::failure(detected.error());
    }
    if (presence_change_pending_) {
        presence_change_pending_ = false;
        return Result<CardOwnershipPresenceChange>::success(pending_presence_change_);
    }
    return Result<CardOwnershipPresenceChange>::success(change);
}

template <typename Snapshot>
bool CardOwnershipEngine<Snapshot>::sharing_allowed(CardShareMode requested,
                                  std::size_t excluded_index) const noexcept
{
    if (requested != CardShareMode::shared &&
        requested != CardShareMode::exclusive) {
        return false;
    }
    for (std::size_t index = 0U; index < handles_.size(); ++index) {
        if (index == excluded_index) {
            continue;
        }
        if (requested == CardShareMode::exclusive ||
            handles_[index].share_mode == CardShareMode::exclusive) {
            return false;
        }
    }
    return true;
}

template <typename Snapshot>
std::uint64_t CardOwnershipEngine<Snapshot>::allocate_handle() noexcept
{
    while (true) {
        const std::uint64_t candidate = next_handle_++;
        if (next_handle_ == 0U) {
            next_handle_ = 1U;
        }
        if (candidate == 0U) {
            continue;
        }
        bool used = false;
        for (const HandleRecord& record : handles_) {
            used = used || record.handle == candidate;
        }
        if (!used) {
            return candidate;
        }
    }
}

template <typename Snapshot>
Result<CardOwnershipConnectResult<Snapshot>> CardOwnershipEngine<Snapshot>::connect(
    std::uint64_t client, CardShareMode share_mode, bool generation_bound) noexcept
{
    if (client == 0U ||
        (share_mode != CardShareMode::shared &&
         share_mode != CardShareMode::exclusive)) {
        return Result<CardOwnershipConnectResult<Snapshot>>::failure(Error::INVALID_ARGUMENT);
    }
    if (!sharing_allowed(share_mode, std::numeric_limits<std::size_t>::max())) {
        return Result<CardOwnershipConnectResult<Snapshot>>::failure(
            Error::BUSY);
    }
    const bool first_handle = handles_.empty();
    const auto initialized = ensure_session();
    if (!initialized) {
        if (first_handle) {
            const auto off = power_down();
            if (!off) {
                return Result<CardOwnershipConnectResult<Snapshot>>::failure(off.error());
            }
        }
        return Result<CardOwnershipConnectResult<Snapshot>>::failure(initialized.error());
    }
    const std::uint64_t handle = allocate_handle();
    handles_.push_back(HandleRecord{handle, client, share_mode, generation_bound, false});
    return Result<CardOwnershipConnectResult<Snapshot>>::success(
        CardOwnershipConnectResult<Snapshot>{handle, cached_atr_});
}

template <typename Snapshot>
Result<std::size_t> CardOwnershipEngine<Snapshot>::find_handle(std::uint64_t client,
                                             std::uint64_t handle) const noexcept
{
    if (client == 0U || handle == 0U) {
        return Result<std::size_t>::failure(Error::INVALID_ARGUMENT);
    }
    for (std::size_t index = 0U; index < handles_.size(); ++index) {
        if (handles_[index].handle == handle) {
            return handles_[index].client == client ?
                       Result<std::size_t>::success(index) :
                       Result<std::size_t>::failure(Error::NOT_FOUND);
        }
    }
    return Result<std::size_t>::failure(Error::NOT_FOUND);
}

template <typename Snapshot>
bool CardOwnershipEngine<Snapshot>::transaction_allows(std::uint64_t handle) const noexcept
{
    return transaction_handle_ == 0U || transaction_handle_ == handle;
}

template <typename Snapshot>
Result<void> CardOwnershipEngine<Snapshot>::apply_disposition(CardDisposition disposition) noexcept
{
    if (disposition == CardDisposition::leave) {
        return Result<void>::success();
    }
    if (disposition != CardDisposition::reset) {
        return Result<void>::failure(Error::INVALID_ARGUMENT);
    }
    const auto reset = reset_session();
    return reset ? Result<void>::success() : Result<void>::failure(reset.error());
}

template <typename Snapshot>
Result<Snapshot> CardOwnershipEngine<Snapshot>::reconnect(std::uint64_t client, std::uint64_t handle,
                                       CardShareMode share_mode,
                                       CardDisposition disposition) noexcept
{
    if ((share_mode != CardShareMode::shared &&
         share_mode != CardShareMode::exclusive) ||
        (disposition != CardDisposition::leave &&
         disposition != CardDisposition::reset)) {
        return Result<Snapshot>::failure(Error::INVALID_ARGUMENT);
    }
    const auto found = find_handle(client, handle);
    if (!found) {
        return Result<Snapshot>::failure(found.error());
    }
    if (!transaction_allows(handle)) {
        return Result<Snapshot>::failure(Error::BUSY);
    }
    if (!sharing_allowed(share_mode, found.value())) {
        return Result<Snapshot>::failure(Error::BUSY);
    }
    Result<void> ready = Result<void>::success();
    if (disposition == CardDisposition::reset) {
        const auto reset = reset_session(transaction_handle_ == handle);
        ready = reset ? Result<void>::success() :
                        Result<void>::failure(reset.error());
    } else {
        ready = ensure_session();
    }
    if (!ready) {
        return Result<Snapshot>::failure(ready.error());
    }
    handles_[found.value()].share_mode = share_mode;
    handles_[found.value()].stale = false;
    return Result<Snapshot>::success(cached_atr_);
}

template <typename Snapshot>
Result<void> CardOwnershipEngine<Snapshot>::release_handle(std::size_t index) noexcept
{
    if (index >= handles_.size()) {
        return Result<void>::failure(Error::INVALID_ARGUMENT);
    }
    if (transaction_handle_ == handles_[index].handle) {
        transaction_handle_ = 0U;
    }
    handles_.erase(handles_.begin() + static_cast<std::ptrdiff_t>(index));
    return handles_.empty() ? power_down() : Result<void>::success();
}

template <typename Snapshot>
Result<void> CardOwnershipEngine<Snapshot>::disconnect(std::uint64_t client, std::uint64_t handle,
                                     CardDisposition disposition) noexcept
{
    if (disposition != CardDisposition::leave &&
        disposition != CardDisposition::reset) {
        return Result<void>::failure(Error::INVALID_ARGUMENT);
    }
    const auto found = find_handle(client, handle);
    if (!found) {
        return Result<void>::failure(found.error());
    }
    if (!handles_[found.value()].stale && disposition == CardDisposition::reset &&
        !transaction_allows(handle)) {
        return Result<void>::failure(Error::BUSY);
    }
    Error first = Error::OK;
    // Cleanup of a stale handle must not reset a replacement card, including
    // when a different handle currently owns its transaction.
    const auto applied = handles_[found.value()].stale ? Result<void>::success() :
                                                       apply_disposition(disposition);
    if (!applied) {
        first = applied.error();
    }
    const auto released = release_handle(found.value());
    if (!released) {
        first = released.error();
    }
    return first == Error::OK ? Result<void>::success() : Result<void>::failure(first);
}

template <typename Snapshot>
Result<Snapshot> CardOwnershipEngine<Snapshot>::reset(std::uint64_t client,
                                   std::uint64_t handle) noexcept
{
    const auto found = find_handle(client, handle);
    if (!found) {
        return Result<Snapshot>::failure(found.error());
    }
    if (!transaction_allows(handle)) {
        return Result<Snapshot>::failure(Error::BUSY);
    }
    const auto result = reset_session(transaction_handle_ == handle);
    if (result) handles_[found.value()].stale = false;
    return result;
}

template <typename Snapshot>
Result<std::size_t> CardOwnershipEngine<Snapshot>::transmit(std::uint64_t client,
                                          std::uint64_t handle, ByteSpan apdu,
                                          MutableByteSpan response) noexcept
{
    if (apdu.data == nullptr || apdu.size == 0U ||
        apdu.size > max_apdu_size_ || response.data == nullptr ||
        response.size == 0U) {
        return Result<std::size_t>::failure(Error::INVALID_ARGUMENT);
    }
    const auto found = find_handle(client, handle);
    if (!found) {
        return Result<std::size_t>::failure(found.error());
    }
    if (handles_[found.value()].stale) {
        return Result<std::size_t>::failure(Error::CARD_REMOVED);
    }
    if (!transaction_allows(handle)) {
        return Result<std::size_t>::failure(Error::BUSY);
    }
    const auto initialized = ensure_handle_session(found.value());
    if (!initialized) {
        transaction_handle_ = 0U;
        return Result<std::size_t>::failure(initialized.error());
    }
    const auto result = session_.transmit(apdu, response);
    if (!result) {
        if (result.error() == Error::NO_CARD ||
            result.error() == Error::CARD_REMOVED) invalidate_bound_handles();
        if (result.error() == Error::DISCONNECTED) {
            abandon_disconnected_hardware();
        } else {
            invalidate_session();
        }
        return Result<std::size_t>::failure(result.error());
    }
    return result;
}

template <typename Snapshot>
Result<void> CardOwnershipEngine<Snapshot>::begin_transaction(std::uint64_t client,
                                            std::uint64_t handle) noexcept
{
    const auto found = find_handle(client, handle);
    if (!found) {
        return Result<void>::failure(found.error());
    }
    if (handles_[found.value()].stale) return Result<void>::failure(Error::CARD_REMOVED);
    if (transaction_handle_ != 0U) {
        return Result<void>::failure(Error::BUSY);
    }
    const auto initialized = ensure_handle_session(found.value());
    if (!initialized) {
        return initialized;
    }
    transaction_handle_ = handle;
    return Result<void>::success();
}

template <typename Snapshot>
Result<void> CardOwnershipEngine<Snapshot>::end_transaction(std::uint64_t client,
                                          std::uint64_t handle,
                                          CardDisposition disposition) noexcept
{
    const auto found = find_handle(client, handle);
    if (!found) {
        return Result<void>::failure(found.error());
    }
    if (handles_[found.value()].stale) return Result<void>::failure(Error::CARD_REMOVED);
    if (transaction_handle_ != handle) {
        return Result<void>::failure(Error::BUSY);
    }
    transaction_handle_ = 0U;
    return apply_disposition(disposition);
}

template <typename Snapshot>
Result<void> CardOwnershipEngine<Snapshot>::release_connection(std::uint64_t client) noexcept
{
    if (client == 0U) {
        return Result<void>::failure(Error::INVALID_ARGUMENT);
    }
    for (std::size_t index = handles_.size(); index > 0U; --index) {
        if (handles_[index - 1U].client == client) {
            if (transaction_handle_ == handles_[index - 1U].handle) {
                transaction_handle_ = 0U;
            }
            handles_.erase(handles_.begin() +
                           static_cast<std::ptrdiff_t>(index - 1U));
        }
    }
    return handles_.empty() ? power_down() : Result<void>::success();
}

template <typename Snapshot>
Result<void> CardOwnershipEngine<Snapshot>::shutdown() noexcept
{
    handles_.clear();
    transaction_handle_ = 0U;
    return power_down();
}

}  // namespace dtv::userland::detail
