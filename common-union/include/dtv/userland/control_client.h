// SPDX-License-Identifier: MIT
#ifndef DTV_USERLAND_CONTROL_CLIENT_H
#define DTV_USERLAND_CONTROL_CLIENT_H

#include <dtv/userland/records.h>

namespace dtv::userland::wire {

class ClientTransport {
public:
    virtual ~ClientTransport() noexcept = default;
    virtual Result<void> write_all(ByteView bytes, Timeout timeout) noexcept = 0;
    virtual Result<std::size_t> read_some(MutableByteView bytes, Timeout timeout) noexcept = 0;
    virtual std::uint64_t monotonic_ms() noexcept = 0;
    virtual void close() noexcept = 0;
};

struct StatusSnapshot final {
    ListResponse list;
    StatusResponse status;
};

struct StatusFetchResult final {
    StatusSnapshot snapshot;
    Error error = Error::OK;
    std::uint32_t remote_error_code = 0U;
    explicit operator bool() const noexcept { return error == Error::OK; }
};

// Uses one transport and one total monotonic deadline for HELLO, LIST and
// STATUS. Every failure closes the connection; requests are never retried.
StatusFetchResult fetch_status(ClientTransport& transport, Timeout timeout) noexcept;

}  // namespace dtv::userland::wire

#endif  // DTV_USERLAND_CONTROL_CLIENT_H
