// SPDX-License-Identifier: MIT
#ifndef DTV_USERLAND_DETAIL_TEXT_H
#define DTV_USERLAND_DETAIL_TEXT_H

#include <cstddef>
#include <cstdint>

// Single shared capacity and text-validation definitions for the wireless
// records and the identity codecs. These values were previously duplicated as
// literals; keep the one definition here.
namespace dtv::userland::detail {

// Complete public USB function set for one enclosure.
inline constexpr std::size_t kMaxFunctions = 8U;
// Canonical USB bus-port path string ("bus-port[.port...]"), ASCII bytes.
inline constexpr std::size_t kMaxUsbPathBytes = 64U;
// Largest legal `usb:` suffix: kMaxFunctions paths plus the separating commas.
inline constexpr std::size_t kMaxUsbSelectorSuffixBytes =
    (kMaxFunctions * kMaxUsbPathBytes) + (kMaxFunctions - 1U);
// Enclosure serial string, strict UTF-8 bytes.
inline constexpr std::size_t kMaxSerialBytes = 512U;
// Connection namespace instance token, ASCII bytes.
inline constexpr std::size_t kMaxInstanceBytes = 64U;
// Lossless UTF-8 output of one decoded UTF-16LE USB string descriptor. The
// largest USB descriptor is 255 bytes, so at most 126 UTF-16 code units and at
// most 378 UTF-8 bytes; this bound leaves margin without allocation.
inline constexpr std::size_t kMaxDecodedStringBytes = 512U;
// Full product slug, e.g. "asicen-userland" (15 bytes).
inline constexpr std::size_t kMaxProductSlugBytes = 16U;

// Strict UTF-8 validation. Rejects overlong encodings, UTF-16 surrogates,
// code points above U+10FFFF and truncated sequences. An embedded NUL is a
// valid UTF-8 scalar and is left to the caller's field rule, so this is the
// wire-general validator: serial-specific control rejection stays in the
// serial selector codec.
inline bool strict_utf8(const std::uint8_t* data, std::size_t size) noexcept
{
    std::size_t index = 0U;
    while (index < size) {
        const std::uint8_t lead = data[index];
        std::size_t extra = 0U;
        std::uint32_t codepoint = 0U;
        std::uint32_t lower = 0U;
        if (lead < 0x80U) {
            ++index;
            continue;
        }
        if ((lead & 0xE0U) == 0xC0U) {
            extra = 1U;
            codepoint = static_cast<std::uint32_t>(lead & 0x1FU);
            lower = 0x80U;
        } else if ((lead & 0xF0U) == 0xE0U) {
            extra = 2U;
            codepoint = static_cast<std::uint32_t>(lead & 0x0FU);
            lower = 0x800U;
        } else if ((lead & 0xF8U) == 0xF0U) {
            extra = 3U;
            codepoint = static_cast<std::uint32_t>(lead & 0x07U);
            lower = 0x10000U;
        } else {
            return false;
        }
        if (size - index < extra + 1U) {
            return false;
        }
        for (std::size_t step = 1U; step <= extra; ++step) {
            if ((data[index + step] & 0xC0U) != 0x80U) {
                return false;
            }
            codepoint = (codepoint << 6U) | static_cast<std::uint32_t>(data[index + step] & 0x3FU);
        }
        if (codepoint < lower || codepoint > 0x10FFFFU) {
            return false;
        }
        if (codepoint >= 0xD800U && codepoint <= 0xDFFFU) {
            return false;
        }
        index += extra + 1U;
    }
    return true;
}

}  // namespace dtv::userland::detail

#endif  // DTV_USERLAND_DETAIL_TEXT_H
