// SPDX-License-Identifier: MIT
#include "dtv/userland/detail/card_ownership_engine.h"

#include <array>
#include <cstdio>

namespace {

using namespace dtv::userland;
using namespace dtv::userland::detail;

struct Snapshot final {
    std::array<std::uint8_t, 4U> data{};
    std::uint32_t opaque = 0U;
};

class Backend final : public CardOwnershipBackend {
public:
    Result<void> set_power(bool on) noexcept override
    {
        powered = on;
        return Result<void>::success();
    }
    Result<void> initialize_uart() noexcept override { return Result<void>::success(); }
    Result<bool> detect_card() noexcept override
    {
        return detect_error == Error::OK ? Result<bool>::success(present) :
                                           Result<bool>::failure(detect_error);
    }
    bool powered = false;
    bool present = true;
    Error detect_error = Error::OK;
};

class Session final : public CardOwnershipSession<Snapshot> {
public:
    Result<void> initialize() noexcept override
    {
        if (initialize_error != Error::OK) return Result<void>::failure(initialize_error);
        ready = true;
        snapshot.data = {0xa1U, 0xb2U, 0xc3U, 0xd4U};
        snapshot.opaque = 0x87654321U;
        return Result<void>::success();
    }
    Result<std::size_t> transmit(ByteSpan, MutableByteSpan) noexcept override
    {
        return ready ? Result<std::size_t>::success(0U) :
                       Result<std::size_t>::failure(Error::NOT_READY);
    }
    bool initialized() const noexcept override { return ready; }
    const Snapshot& atr() const noexcept override { return snapshot; }
    void invalidate() noexcept override { ready = false; }
    Snapshot snapshot{};
    Error initialize_error = Error::OK;
    bool ready = false;
};

bool run()
{
    Backend backend;
    Session session;
    CardOwnershipEngine<Snapshot> engine(backend, session, 4096U);
    const auto first = engine.connect(1U, CardShareMode::shared, true);
    const auto second = engine.connect(2U, CardShareMode::shared);
    if (!first || !second || first.value().atr.data != session.snapshot.data ||
        first.value().atr.opaque != session.snapshot.opaque) return false;
    const auto exclusive = engine.connect(3U, CardShareMode::exclusive);
    if (exclusive || exclusive.error() != Error::BUSY) return false;
    if (!engine.begin_transaction(1U, first.value().handle)) return false;
    const auto competing = engine.begin_transaction(2U, second.value().handle);
    if (competing || competing.error() != Error::BUSY) return false;
    if (!engine.end_transaction(1U, first.value().handle, CardDisposition::leave)) return false;
    backend.present = false;
    const auto removed = engine.poll_presence();
    if (!removed || !removed.value().changed || removed.value().present) return false;
    backend.present = true;
    const auto inserted = engine.poll_presence();
    if (!inserted || !inserted.value().changed || !inserted.value().present ||
        inserted.value().reader_generation != removed.value().reader_generation + 1U) return false;
    const auto stale = engine.begin_transaction(1U, first.value().handle);
    if (stale || stale.error() != Error::CARD_REMOVED) return false;
    const auto rebound = engine.reconnect(1U, first.value().handle,
                                          CardShareMode::shared,
                                          CardDisposition::leave);
    if (!rebound || rebound.value().opaque != 0x87654321U) return false;
    if (!engine.shutdown() || backend.powered) return false;
    return true;
}

bool run_handle_status()
{
    Backend backend;
    Session session;
    session.ready = true;
    CardOwnershipEngine<Snapshot> engine(backend, session, 4096U);
    const auto connected = engine.connect(1U, CardShareMode::shared, true);
    if (!connected) return false;
    const auto owned = engine.handle_status(1U, connected.value().handle);
    if (!owned || !owned.value().present || !owned.value().initialized ||
        owned.value().atr.data != Snapshot{}.data || owned.value().atr.opaque != 0U) return false;
    const auto foreign = engine.handle_status(2U, connected.value().handle);
    if (foreign || foreign.error() != Error::NOT_FOUND) return false;

    session.invalidate();
    const auto uninitialized = engine.handle_status(1U, connected.value().handle);
    if (uninitialized || uninitialized.error() != Error::NOT_READY) return false;

    backend.present = false;
    const auto removed = engine.poll_presence();
    if (!removed || !removed.value().changed || removed.value().present) return false;
    const auto stale = engine.handle_status(1U, connected.value().handle);
    if (stale || stale.error() != Error::CARD_REMOVED) return false;
    return engine.shutdown() && !backend.powered;
}

}  // namespace

int main()
{
    if (!run() || !run_handle_status()) {
        std::fputs("card ownership engine test failed\n", stderr);
        return 1;
    }
    return 0;
}
