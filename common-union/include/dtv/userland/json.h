// SPDX-License-Identifier: MIT
#ifndef DTV_USERLAND_JSON_H
#define DTV_USERLAND_JSON_H

#include <dtv/userland/records.h>

namespace dtv::userland::wire {

// Both functions validate the complete response against its canonical context.
// Keep value, context, canonical list and their referenced storage unchanged for
// the entire call. Encode output must not overlap any of those inputs. The
// returned size includes the final newline; encoded bytes have no NUL terminator.
Result<std::size_t> status_json_size(const StatusResponse& value,
                                     const StatusContext& context) noexcept;

// Writes one compact UTF-8 JSON object. Validation, size-counting and capacity
// failures leave every byte in output unchanged; short capacity returns
// Error::BUFFER_TOO_SMALL.
Result<std::size_t> encode_status_json(const StatusResponse& value,
                                       const StatusContext& context,
                                       MutableByteView output) noexcept;

}  // namespace dtv::userland::wire

#endif  // DTV_USERLAND_JSON_H
