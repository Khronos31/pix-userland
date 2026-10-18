// SPDX-License-Identifier: MIT
#include "dtv/userland/error.h"

namespace dtv::userland {

const char* error_symbol(std::uint32_t code) noexcept
{
    switch (code) {
    case 0U: return "OK";
    case 1U: return "INVALID_ARGUMENT";
    case 2U: return "VERSION_MISMATCH";
    case 3U: return "NOT_FOUND";
    case 4U: return "BUSY";
    case 5U: return "NOT_READY";
    case 6U: return "TIMEOUT";
    case 7U: return "USB_IO";
    case 8U: return "DISCONNECTED";
    case 9U: return "PROTOCOL_ERROR";
    case 10U: return "FIRMWARE_REJECTED";
    case 11U: return "UNSUPPORTED";
    case 12U: return "NO_CARD";
    case 13U: return "CARD_REMOVED";
    case 14U: return "BUFFER_TOO_SMALL";
    case 15U: return "SLOW_CONSUMER";
    case 16U: return "STALE_HANDLE";
    case 17U: return "RESOURCE_LIMIT";
    case 18U: return "CANCELLED";
    case 19U: return "ACCESS_DENIED";
    case 20U: return "INVALID_STATE";
    case 255U: return "INTERNAL";
    default: return nullptr;
    }
}

const char* error_string(Error error) noexcept
{
    switch (static_cast<std::uint8_t>(error)) {
    case 0U: case 1U: case 2U: case 3U: case 4U: case 5U: case 6U: case 7U:
    case 8U: case 9U: case 10U: case 11U: case 12U: case 13U: case 14U:
    case 15U: case 17U: case 255U:
        return error_symbol(static_cast<std::uint8_t>(error));
    default:
        break;
    }
    return "UNKNOWN";
}

}  // namespace dtv::userland
