// SPDX-License-Identifier: GPL-2.0-or-later
#include "pix_runtime.h"

#include <dtv/userland/records.h>
#include <array>

int main()
{
    pix::runtime::LocatedDevice device{};
    device.bus = 2U;
    device.address = 7U;
    device.path = "2-3.1";
    std::array<std::uint8_t, px4::userland::ipc::kNonceLength> epoch{};
    px4::userland::ipc::posix::ControlContext context{};
    const auto built = pix::runtime::build_list(
        device, "auto-0123456789abcdef0123456789abcdef", "0.0.0-dev", epoch, context);
    if (!built) return 1;
    const auto& list = context.list;
    if (list.functions.count != 1U || list.functions.items[0].vid != 0x06b8U ||
        list.functions.items[0].pid != 0x102bU || !list.receivers.present ||
        list.receivers.value.count != 1U || !list.readers.present ||
        list.readers.value.count != 0U || list.receivers.value.items[0].systems.mask != 3U ||
        list.receivers.value.items[0].capabilities.ts_scope !=
            dtv::userland::wire::TsScope::multiplex) return 2;
    return 0;
}
