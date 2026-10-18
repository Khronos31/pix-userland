// SPDX-License-Identifier: MIT
#ifndef DTV_USERLAND_DETAIL_CARD_OWNERSHIP_ENGINE_H
#define DTV_USERLAND_DETAIL_CARD_OWNERSHIP_ENGINE_H

#include "dtv/userland/error.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace dtv::userland::detail {

enum class CardShareMode : std::uint8_t { invalid, shared, exclusive };
enum class CardDisposition : std::uint8_t { invalid, leave, reset };

struct ByteSpan final {
    const std::uint8_t* data = nullptr;
    std::size_t size = 0U;
};

struct MutableByteSpan final {
    std::uint8_t* data = nullptr;
    std::size_t size = 0U;
};

class CardOwnershipBackend {
public:
    virtual ~CardOwnershipBackend() noexcept = default;
    virtual Result<void> set_power(bool on) noexcept = 0;
    virtual Result<void> initialize_uart() noexcept = 0;
    virtual Result<bool> detect_card() noexcept = 0;
};

template <typename AtrSnapshot>
class CardOwnershipSession {
public:
    virtual ~CardOwnershipSession() noexcept = default;
    virtual Result<void> initialize() noexcept = 0;
    virtual Result<std::size_t> transmit(ByteSpan apdu,
                                         MutableByteSpan response) noexcept = 0;
    virtual bool initialized() const noexcept = 0;
    virtual const AtrSnapshot& atr() const noexcept = 0;
    virtual void invalidate() noexcept = 0;
};

template <typename AtrSnapshot>
struct CardOwnershipStatus final {
    bool present = false;
    bool initialized = false;
    std::uint64_t reader_generation = 1U;
    AtrSnapshot atr{};
};

template <typename AtrSnapshot>
struct CardOwnershipConnectResult final {
    std::uint64_t handle = 0U;
    AtrSnapshot atr{};
};

struct CardOwnershipPresenceChange final {
    bool changed = false;
    bool present = false;
    std::uint64_t reader_generation = 1U;
};

// Internal compatibility engine for the legacy serialized card owner. ATR
// snapshots are opaque values: the engine only defaults, copies, and returns
// them. This is not the adopted v1 card or deadline contract.
template <typename AtrSnapshot>
class CardOwnershipEngine final {
public:
    CardOwnershipEngine(CardOwnershipBackend& backend,
                        CardOwnershipSession<AtrSnapshot>& session,
                        std::size_t max_apdu_size) noexcept
        : backend_(backend), session_(session), max_apdu_size_(max_apdu_size)
    {
    }
    ~CardOwnershipEngine() noexcept;

    CardOwnershipEngine(const CardOwnershipEngine&) = delete;
    CardOwnershipEngine& operator=(const CardOwnershipEngine&) = delete;

    Result<CardOwnershipStatus<AtrSnapshot>> status() noexcept;
    Result<CardOwnershipStatus<AtrSnapshot>> handle_status(
        std::uint64_t client, std::uint64_t handle) noexcept;
    Result<CardOwnershipPresenceChange> poll_presence() noexcept;
    Result<CardOwnershipConnectResult<AtrSnapshot>> connect(
        std::uint64_t client, CardShareMode share_mode,
        bool generation_bound = false) noexcept;
    Result<AtrSnapshot> reconnect(std::uint64_t client, std::uint64_t handle,
                                  CardShareMode share_mode,
                                  CardDisposition disposition) noexcept;
    Result<void> disconnect(std::uint64_t client, std::uint64_t handle,
                            CardDisposition disposition) noexcept;
    Result<AtrSnapshot> reset(std::uint64_t client, std::uint64_t handle) noexcept;
    Result<std::size_t> transmit(std::uint64_t client, std::uint64_t handle,
                                 ByteSpan apdu, MutableByteSpan response) noexcept;
    Result<void> begin_transaction(std::uint64_t client,
                                   std::uint64_t handle) noexcept;
    Result<void> end_transaction(std::uint64_t client, std::uint64_t handle,
                                 CardDisposition disposition) noexcept;
    Result<void> release_connection(std::uint64_t client) noexcept;
    Result<void> shutdown() noexcept;

    bool has_handles() const noexcept { return !handles_.empty(); }
    std::size_t handle_count() const noexcept { return handles_.size(); }
    bool powered() const noexcept { return powered_; }
    std::uint64_t reader_generation() const noexcept { return reader_generation_; }

private:
    struct HandleRecord final {
        std::uint64_t handle;
        std::uint64_t client;
        CardShareMode share_mode;
        bool generation_bound;
        bool stale;
    };

    Result<void> power_up() noexcept;
    Result<void> power_down() noexcept;
    Result<void> ensure_session() noexcept;
    Result<void> ensure_handle_session(std::size_t index) noexcept;
    Result<AtrSnapshot> reset_session(bool preserve_transaction = false) noexcept;
    Result<bool> detect_and_record(bool report_initial_change,
                                   CardOwnershipPresenceChange* change) noexcept;
    Result<void> apply_disposition(CardDisposition disposition) noexcept;
    Result<void> release_handle(std::size_t index) noexcept;
    Result<std::size_t> find_handle(std::uint64_t client,
                                    std::uint64_t handle) const noexcept;
    bool sharing_allowed(CardShareMode requested,
                         std::size_t excluded_index) const noexcept;
    bool transaction_allows(std::uint64_t handle) const noexcept;
    std::uint64_t allocate_handle() noexcept;
    void invalidate_session() noexcept;
    void invalidate_bound_handles() noexcept;
    void abandon_disconnected_hardware() noexcept;

    CardOwnershipBackend& backend_;
    CardOwnershipSession<AtrSnapshot>& session_;
    std::size_t max_apdu_size_;
    std::vector<HandleRecord> handles_;
    AtrSnapshot cached_atr_{};
    std::uint64_t next_handle_ = 1U;
    std::uint64_t transaction_handle_ = 0U;
    std::uint64_t reader_generation_ = 1U;
    bool powered_ = false;
    bool uart_initialized_ = false;
    bool presence_known_ = false;
    bool present_ = false;
    bool presence_change_pending_ = false;
    CardOwnershipPresenceChange pending_presence_change_{};
};

}  // namespace dtv::userland::detail

#include "dtv/userland/detail/card_ownership_engine.tpp"

#endif  // DTV_USERLAND_DETAIL_CARD_OWNERSHIP_ENGINE_H
