/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "pix-capture-consumer.h"

#include <array>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstring>

int main()
{
    using namespace std::chrono_literals;
    using pix::capture::Consumer;

    Consumer consumer;
    Consumer::Attachment identity{};
    identity.owner_client_id = 11U;
    identity.lease_id = 12U;
    identity.attachment_id = 13U;
    identity.receiver = 0U;
    identity.token[0] = 14U;
    assert(consumer.attach(identity, 4U));

    std::array<std::uint8_t, 376U> packets{};
    for (std::size_t packet = 0U; packet < 2U; ++packet) {
        packets[packet * 188U] = 0x47U;
        packets[packet * 188U + 3U] = static_cast<std::uint8_t>(packet + 1U);
    }
    assert(Consumer::decoded_sink(consumer.sink_context(), packets.data(), packets.size()) == 0);
    assert(consumer.detach_after_worker_join(identity));

    std::array<std::uint8_t, 376U> output{};
    auto read = consumer.read(identity,
                              dtv::userland::MutableByteView{output.data(), output.size()}, 250ms);
    assert(read && read.value().bytes == output.size());
    assert(std::memcmp(output.data(), packets.data(), packets.size()) == 0);
    auto final = consumer.final_snapshot(identity);
    assert(final && final.value().counters.packets == 2U);
    assert(final.value().counters.bytes == packets.size());
    assert(consumer.release_final(identity));
}
