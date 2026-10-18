// SPDX-License-Identifier: MIT
// Positive installed-consumer check for the capture-only component.  It must
// compile and link against an install that has the capture component enabled
// and the receiver component disabled, without including the receiver header.
#include <dtv/userland/detail/packet_stream_session.h>

#include <array>
#include <cstdint>
#include <mutex>

int main()
{
    using namespace dtv::userland;
    using namespace dtv::userland::detail;

    PacketStreamSession session;
    Attachment attachment{};
    attachment.owner_client_id = 1U;
    attachment.lease_id = 1U;
    attachment.attachment_id = 1U;
    attachment.receiver = 0U;
    {
        std::lock_guard<std::mutex> lock(session.mutex);
        if (!session.begin_attach_locked(attachment, 4U)) return 1;
        std::array<std::uint8_t, PacketStreamSession::packet_size> packet{};
        packet[0] = 0x47U;
        session.append_locked(ByteView{packet.data(), packet.size()});
    }
    std::array<std::uint8_t, PacketStreamSession::packet_size> output{};
    {
        std::lock_guard<std::mutex> lock(session.mutex);
        const auto ticket = session.begin_read_locked(attachment);
        if (!ticket) return 1;
        const auto read = session.finish_read_locked(
            attachment, ticket.value(), MutableByteView{output.data(), output.size()});
        if (!read || read.value().bytes != output.size()) return 1;
    }
    return output[0] == 0x47U ? 0 : 1;
}
