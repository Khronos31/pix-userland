/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef PIX_CAPTURE_CONSUMER_INTERNAL_H
#define PIX_CAPTURE_CONSUMER_INTERNAL_H

#include "pix-capture-consumer.h"

namespace pix::capture {

struct ConsumerOwner final : std::enable_shared_from_this<ConsumerOwner> {
    struct Run final {
        ConsumerOwner* owner = nullptr;
        dtv::userland::detail::Attachment identity{};
        std::uint64_t incarnation = 0U;
    };

    dtv::userland::Result<void> stop() noexcept;
    bool quiesced() const noexcept;

    dtv::userland::detail::PacketStreamSession session;
    std::unique_ptr<Run> run;
    dt295_capture_host* host = nullptr;
    std::shared_ptr<ConsumerOwner> keep_alive;
};

}  // namespace pix::capture

#endif
