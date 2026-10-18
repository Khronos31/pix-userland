// SPDX-License-Identifier: MIT
// Installed-package consumer for the typed wire records. It links only the
// installed dtv::core / dtv::wire targets and never includes product headers.
#include <dtv/userland/records.h>
#include <dtv/userland/wire.h>

#include <cstdint>

int main()
{
    using namespace dtv::userland;
    using namespace dtv::userland::wire;

    StartResponse start;
    start.epoch[0] = 0x11U;
    start.stream_id = 0x0102030405060708ULL;
    start.attach_token[0] = 0x22U;
    start.expires_after_ms = 5000U;

    std::uint8_t payload[64]{};
    const auto encoded = encode_start_response(start, {payload, sizeof(payload)});
    const auto decoded = decode_start_response({payload, encoded ? encoded.value() : 0U});

    ReceiverDescriptor descriptor;
    descriptor.id = 3U;
    descriptor.systems.mask = 1U;
    const Error descriptor_error = validate_receiver_descriptor(descriptor);

    ErrorRecord record;
    record.code = 255U;
    const Error record_error = validate_error_record(record);

    StopRequest stop;
    stop.epoch[0] = 0x11U;
    stop.lease_id = 1U;
    stop.stream_id = 2U;
    std::uint8_t stop_bytes[32]{};
    const auto stop_encoded = encode_stop_request(stop, {stop_bytes, sizeof(stop_bytes)});

    return encoded && encoded.value() == 44U && decoded && decoded.value().stream_id == start.stream_id &&
                   descriptor_error == Error::OK && record_error == Error::OK && stop_encoded &&
                   stop_encoded.value() == 32U
               ? 0
               : 1;
}
