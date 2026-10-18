// SPDX-License-Identifier: MIT
#include <dtv/userland/detail/card_ownership_engine.h>

namespace {
using namespace dtv::userland;
using namespace dtv::userland::detail;
struct Snapshot final { unsigned value = 0U; };
class Backend final : public CardOwnershipBackend {
public:
    Result<void> set_power(bool on) noexcept override { powered = on; return Result<void>::success(); }
    Result<void> initialize_uart() noexcept override { return Result<void>::success(); }
    Result<bool> detect_card() noexcept override { return Result<bool>::success(true); }
    bool powered = false;
};
class Session final : public CardOwnershipSession<Snapshot> {
public:
    Result<void> initialize() noexcept override { ready = true; snapshot.value = 7U; return Result<void>::success(); }
    Result<std::size_t> transmit(ByteSpan, MutableByteSpan) noexcept override { return Result<std::size_t>::success(0U); }
    bool initialized() const noexcept override { return ready; }
    const Snapshot& atr() const noexcept override { return snapshot; }
    void invalidate() noexcept override { ready = false; }
    Snapshot snapshot{};
    bool ready = false;
};
}

int main()
{
    Backend backend;
    Session session;
    CardOwnershipEngine<Snapshot> engine(backend, session, 4096U);
    const auto handle = engine.connect(1U, CardShareMode::shared);
    return handle && handle.value().atr.value == 7U && engine.shutdown() && !backend.powered ? 0 : 1;
}
