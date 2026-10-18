// SPDX-License-Identifier: MIT
#ifndef DTV_USERLAND_TRANSPORT_H
#define DTV_USERLAND_TRANSPORT_H

#include "dtv/userland/error.h"

#include <cstddef>
#include <cstdint>

namespace dtv::userland {

struct ByteView final {
    const std::uint8_t* data;
    std::size_t size;
};

struct MutableByteView final {
    std::uint8_t* data;
    std::size_t size;
};

struct Timeout final {
    std::uint32_t milliseconds;
};

struct StreamConfig final {
    std::uint8_t endpoint;
    std::size_t transfer_size;
    std::size_t transfer_count;
};

enum class StreamEventKind : std::uint8_t {
    data = 0,
    short_transfer = 1,
};

struct StreamEvent final {
    StreamEventKind kind;
    const std::uint8_t* data;
    std::size_t size;
};

struct BulkReadObservation final {
    Error completion_error = Error::OK;
    std::size_t transferred = 0U;
};

class Transport {
public:
    virtual ~Transport() noexcept = default;

    virtual Result<std::size_t> bulk_read(std::uint8_t endpoint,
                                          MutableByteView output,
                                          Timeout timeout,
                                          BulkReadObservation* observation = nullptr) noexcept = 0;
    virtual Result<std::size_t> bulk_write(std::uint8_t endpoint,
                                           ByteView input,
                                           Timeout timeout) noexcept = 0;

    virtual Result<void> start_stream(const StreamConfig& config) noexcept = 0;
    // 返却されたデータは、次のストリーム操作まで有効です。
    virtual Result<StreamEvent> wait_stream(Timeout timeout) noexcept = 0;
    virtual Result<void> cancel_stream() noexcept = 0;
    virtual Result<void> stop_stream() noexcept = 0;
    virtual bool stream_active() const noexcept = 0;
};

}  // namespace dtv::userland

#endif  // DTV_USERLAND_TRANSPORT_H
