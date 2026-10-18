// SPDX-License-Identifier: MIT
#ifndef DTV_USERLAND_IDENTITY_H
#define DTV_USERLAND_IDENTITY_H

#include <dtv/userland/detail/text.h>
#include <dtv/userland/error.h>
#include <dtv/userland/transport.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

// Portable identity codecs for the adopted device selector grammar, connection
// instance names, USB string descriptors and the versioned auto-identity byte
// representation. The codecs are pure and bounded: they never allocate, never
// touch USB/OS state and never decide serial trust or uniqueness. Physical
// discovery, permissions/ACL and USB FD ownership stay in the OS adapters.
namespace dtv::userland::identity {

using dtv::userland::ByteView;
using dtv::userland::Error;
using dtv::userland::MutableByteView;
using dtv::userland::Result;

// The four adopted product slugs. No model/VID:PID/random fallback exists.
enum class Product : std::uint8_t {
    px4 = 1U,
    asicen = 2U,
    pix = 3U,
    siano = 4U,
};

const char* product_slug(Product product) noexcept;
Result<Product> parse_product(std::string_view slug) noexcept;

// Identity kind, matching the wire encoding_version 1 discriminator.
enum class IdentityKind : std::uint8_t { serial = 1U, usb_topology = 2U };

template <std::size_t N>
struct Text final {
    std::uint16_t length = 0U;
    std::array<char, N> bytes{};
};

using SerialText = Text<detail::kMaxSerialBytes>;
using UsbPath = Text<detail::kMaxUsbPathBytes>;
using InstanceText = Text<detail::kMaxInstanceBytes>;
using DecodedString = Text<detail::kMaxDecodedStringBytes>;

// Canonical, ASCII byte-sorted, duplicate-free USB topology. The order is the
// path order, never the input function-role numbering.
struct UsbTopology final {
    std::uint16_t count = 0U;
    std::array<UsbPath, detail::kMaxFunctions> paths{};
};

enum class SelectorKind : std::uint8_t { serial = 1U, usb_topology = 2U };

struct Selector final {
    SelectorKind kind = SelectorKind::serial;
    SerialText serial;
    UsbTopology topology;
};

// Parses the adopted `serial:`/`usb:` device selector. Serial is one nonempty
// strict UTF-8 suffix up to 512 bytes with no NUL or ASCII control; spaces,
// colons, commas and non-ASCII are preserved verbatim. USB is a comma-separated
// set of 1..8 `bus-port[.port...]` paths (positive ASCII decimal, no leading
// zeros, u32 range), each at most 64 bytes, duplicate-free and sorted. This
// proves syntax only: it never proves enclosure grouping or OS presence.
Result<Selector> parse_device_selector(std::string_view text) noexcept;

// Canonicalizes a connection instance token: 1..64 ASCII characters, first
// alphanumeric, rest alphanumeric/underscore/dot/hyphen, lowercased. Rejects a
// terminal dot and the reserved first-dot stems con/prn/aux/nul/com1..com9/
// lpt1..lpt9 on every OS.
Result<InstanceText> canonicalize_instance(std::string_view text) noexcept;

// First LANGID of a validated LANGID table descriptor (bLength == span, type 3,
// even length, at least one language).
Result<std::uint16_t> decode_langid_first(ByteView descriptor) noexcept;

// Lossless UTF-16LE -> UTF-8 decode of a USB string descriptor. Requires an
// exact span, type 3 and (bLength - 2) even; rejects isolated or invalid
// surrogate pairs. An empty string is valid here; whether a decoded serial is a
// usable selector is a separate check.
Result<DecodedString> decode_string_descriptor(ByteView descriptor) noexcept;

// Applies the serial selector rules to an already decoded UTF-8 string:
// nonempty, at most 512 bytes, strict UTF-8, no NUL or ASCII control.
Result<SerialText> make_serial_selector(std::string_view utf8) noexcept;

// Resolved identity handed to the encoder. Already resolved means the adapter
// has decided serial versus topology; the encoder still validates syntax,
// rejects duplicate topology paths (never silently deduplicating) and sorts.
struct ResolvedIdentity final {
    Product product = Product::px4;
    IdentityKind kind = IdentityKind::serial;
    SerialText serial;
    UsbTopology topology;
};

// Builds a resolved identity from a parsed selector and product.
Result<ResolvedIdentity> resolve_selector(Product product, const Selector& selector) noexcept;

// DTVI encoding_version 1 little-endian bytes. Preflights the whole record: a
// short output is BUFFER_TOO_SMALL and no byte is written.
Result<std::size_t> encode_identity(const ResolvedIdentity& value, MutableByteView output) noexcept;

// "auto-" plus the first 16 SHA-256 bytes of the encoded identity as 32
// lowercase hex characters.
Result<InstanceText> auto_instance(const ResolvedIdentity& value) noexcept;

// Largest encoded identity: "DTVI" + u16 + LP(slug) + u8 + u32 count + 8 LP(64).
inline constexpr std::size_t kMaxIdentityBytes =
    4U + 2U + 4U + detail::kMaxProductSlugBytes + 1U + 4U +
    (detail::kMaxFunctions * (4U + detail::kMaxUsbPathBytes));

struct EncodedIdentity final {
    std::uint16_t length = 0U;
    std::array<std::uint8_t, kMaxIdentityBytes> bytes{};
};

// Convenience for tests/consumers: encode into a local bounded record.
Result<EncodedIdentity> encode_identity_bytes(const ResolvedIdentity& value) noexcept;

}  // namespace dtv::userland::identity

#endif  // DTV_USERLAND_IDENTITY_H
