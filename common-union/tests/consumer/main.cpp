#include <dtv/userland/error.h>
#include <dtv/userland/logging.h>
#include <dtv/userland/mock_transport.h>
#include <dtv/userland/wire.h>

int main()
{
    dtv::userland::MockTransport transport({65535U, 1048576U, 0x84U});
    dtv::userland::Logger logger;
    std::uint8_t payload[dtv::userland::wire::kHelloRequestSize]{};
    const dtv::userland::wire::HelloRequest hello{{1U, 0U}, {1U, 0U}, 0U, 0U};
    const auto encoded = dtv::userland::wire::encode_hello_request(
        hello, {payload, sizeof(payload)});
    dtv::userland::wire::Epoch epoch{};
    std::uint8_t acquire_request_bytes[dtv::userland::wire::kAcquireRequestSize]{};
    const auto acquire_request = dtv::userland::wire::encode_acquire_request(
        {epoch, 1U}, {acquire_request_bytes, sizeof(acquire_request_bytes)});
    std::uint8_t acquire_response_bytes[dtv::userland::wire::kAcquireResponseSize]{};
    const auto acquire_response = dtv::userland::wire::encode_acquire_response(
        {epoch, 7U}, {acquire_response_bytes, sizeof(acquire_response_bytes)});
    return dtv::userland::error_string(dtv::userland::Error::OK)[0] == 'O' &&
                   logger.enabled(dtv::userland::LogLevel::info) &&
                   !transport.stream_active() && encoded && encoded.value() == sizeof(payload) &&
                   acquire_request && acquire_request.value() == sizeof(acquire_request_bytes) &&
                   acquire_response && acquire_response.value() == sizeof(acquire_response_bytes)
               ? 0
               : 1;
}
