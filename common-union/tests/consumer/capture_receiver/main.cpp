// SPDX-License-Identifier: MIT
// Positive installed-consumer check for the capture and receiver components
// together: the shared neutral session types header is installed for both.
#include <dtv/userland/detail/packet_stream_session.h>
#include <dtv/userland/detail/receiver_engine.h>

#include <mutex>

int main()
{
    using namespace dtv::userland::detail;

    PacketStreamSession session;
    Attachment attachment{};
    attachment.attachment_id = 1U;
    {
        std::lock_guard<std::mutex> lock(session.mutex);
        if (!session.begin_attach_locked(attachment, 4U)) return 1;
    }
    // The receiver engine header must remain usable next to the capture header.
    ReceiverState state = ReceiverState::free;
    return state == ReceiverState::free ? 0 : 1;
}
