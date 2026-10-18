// SPDX-License-Identifier: MIT
#include <dtv/userland/identity.h>

#include <dtv/userland/detail/sha256.h>

#include <cstring>

namespace dtv::userland::identity {
namespace {

constexpr std::uint8_t kDescriptorTypeString = 3U;

bool ascii_alnum(char c) noexcept
{
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
}

bool valid_serial_text(const SerialText& value) noexcept
{
    if (value.length == 0U || value.length > detail::kMaxSerialBytes) {
        return false;
    }
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(value.bytes.data());
    if (!detail::strict_utf8(bytes, value.length)) {
        return false;
    }
    for (std::size_t i = 0U; i < value.length; ++i) {
        const std::uint8_t byte = bytes[i];
        if (byte == 0U || byte < 0x20U || byte == 0x7FU) {
            return false;
        }
    }
    return true;
}

bool parse_decimal(const char* text, std::size_t size, std::size_t& index,
                   std::uint32_t& value) noexcept
{
    if (index >= size) {
        return false;
    }
    const char first = text[index];
    if (first < '1' || first > '9') {  // positive, no leading zero
        return false;
    }
    std::uint64_t accumulator = 0U;
    while (index < size && text[index] >= '0' && text[index] <= '9') {
        accumulator = (accumulator * 10U) + static_cast<std::uint64_t>(text[index] - '0');
        if (accumulator > 0xffffffffULL) {
            return false;
        }
        ++index;
    }
    value = static_cast<std::uint32_t>(accumulator);
    return true;
}

// bus-port[.port...], ASCII decimal, each number positive and u32.
bool valid_usb_path(const char* text, std::size_t size) noexcept
{
    if (size == 0U || size > detail::kMaxUsbPathBytes) {
        return false;
    }
    std::size_t index = 0U;
    std::uint32_t value = 0U;
    if (!parse_decimal(text, size, index, value)) {
        return false;
    }
    if (index >= size || text[index] != '-') {
        return false;
    }
    ++index;
    if (!parse_decimal(text, size, index, value)) {
        return false;
    }
    while (index < size) {
        if (text[index] != '.') {
            return false;
        }
        ++index;
        if (!parse_decimal(text, size, index, value)) {
            return false;
        }
    }
    return true;
}

void append_utf8(std::uint32_t codepoint, std::array<char, detail::kMaxDecodedStringBytes>& out,
                 std::size_t& length) noexcept
{
    if (codepoint < 0x80U) {
        out[length++] = static_cast<char>(codepoint);
    } else if (codepoint < 0x800U) {
        out[length++] = static_cast<char>(0xC0U | (codepoint >> 6U));
        out[length++] = static_cast<char>(0x80U | (codepoint & 0x3FU));
    } else if (codepoint < 0x10000U) {
        out[length++] = static_cast<char>(0xE0U | (codepoint >> 12U));
        out[length++] = static_cast<char>(0x80U | ((codepoint >> 6U) & 0x3FU));
        out[length++] = static_cast<char>(0x80U | (codepoint & 0x3FU));
    } else {
        out[length++] = static_cast<char>(0xF0U | (codepoint >> 18U));
        out[length++] = static_cast<char>(0x80U | ((codepoint >> 12U) & 0x3FU));
        out[length++] = static_cast<char>(0x80U | ((codepoint >> 6U) & 0x3FU));
        out[length++] = static_cast<char>(0x80U | (codepoint & 0x3FU));
    }
}

// Validates and canonically sorts a resolved topology; duplicates are an error,
// never silently removed.
bool prepare_topology(const UsbTopology& input, UsbTopology& output) noexcept
{
    if (input.count == 0U || input.count > detail::kMaxFunctions) {
        return false;
    }
    output.count = input.count;
    for (std::size_t i = 0U; i < input.count; ++i) {
        const UsbPath& path = input.paths[i];
        if (path.length == 0U || path.length > detail::kMaxUsbPathBytes ||
            !valid_usb_path(path.bytes.data(), path.length)) {
            return false;
        }
        output.paths[i] = path;
    }
    for (std::size_t i = 0U; i < output.count; ++i) {
        for (std::size_t j = 0U; j < i; ++j) {
            if (output.paths[j].length == output.paths[i].length &&
                std::memcmp(output.paths[j].bytes.data(), output.paths[i].bytes.data(),
                            output.paths[i].length) == 0) {
                return false;
            }
        }
    }
    // ASCII byte insertion sort.
    for (std::size_t i = 1U; i < output.count; ++i) {
        UsbPath key = output.paths[i];
        std::size_t j = i;
        while (j > 0U) {
            const UsbPath& previous = output.paths[j - 1U];
            const std::size_t shorter = previous.length < key.length ? previous.length : key.length;
            const int order = std::memcmp(previous.bytes.data(), key.bytes.data(), shorter);
            const bool greater = order > 0 || (order == 0 && previous.length > key.length);
            if (!greater) {
                break;
            }
            output.paths[j] = previous;
            --j;
        }
        output.paths[j] = key;
    }
    return true;
}

void store_u16(std::uint8_t* out, std::uint16_t value) noexcept
{
    out[0] = static_cast<std::uint8_t>(value & 0xffU);
    out[1] = static_cast<std::uint8_t>((value >> 8U) & 0xffU);
}

void store_u32(std::uint8_t* out, std::uint32_t value) noexcept
{
    for (std::size_t i = 0U; i < 4U; ++i) {
        out[i] = static_cast<std::uint8_t>((value >> (8U * i)) & 0xffU);
    }
}

}  // namespace

const char* product_slug(Product product) noexcept
{
    switch (product) {
    case Product::px4:
        return "px4-userland";
    case Product::asicen:
        return "asicen-userland";
    case Product::pix:
        return "pix-userland";
    case Product::siano:
        return "siano-userland";
    }
    return nullptr;
}

Result<Product> parse_product(std::string_view slug) noexcept
{
    if (slug == "px4-userland") {
        return Result<Product>::success(Product::px4);
    }
    if (slug == "asicen-userland") {
        return Result<Product>::success(Product::asicen);
    }
    if (slug == "pix-userland") {
        return Result<Product>::success(Product::pix);
    }
    if (slug == "siano-userland") {
        return Result<Product>::success(Product::siano);
    }
    return Result<Product>::failure(Error::INVALID_ARGUMENT);
}

Result<SerialText> make_serial_selector(std::string_view utf8) noexcept
{
    if (utf8.empty() || utf8.size() > detail::kMaxSerialBytes) {
        return Result<SerialText>::failure(Error::INVALID_ARGUMENT);
    }
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(utf8.data());
    if (!detail::strict_utf8(bytes, utf8.size())) {
        return Result<SerialText>::failure(Error::INVALID_ARGUMENT);
    }
    SerialText value;
    value.length = static_cast<std::uint16_t>(utf8.size());
    for (std::size_t i = 0U; i < utf8.size(); ++i) {
        if (bytes[i] == 0U || bytes[i] < 0x20U || bytes[i] == 0x7FU) {
            return Result<SerialText>::failure(Error::INVALID_ARGUMENT);
        }
        value.bytes[i] = utf8[i];
    }
    return Result<SerialText>::success(value);
}

Result<InstanceText> canonicalize_instance(std::string_view text) noexcept
{
    if (text.empty() || text.size() > detail::kMaxInstanceBytes) {
        return Result<InstanceText>::failure(Error::INVALID_ARGUMENT);
    }
    InstanceText value;
    value.length = static_cast<std::uint16_t>(text.size());
    for (std::size_t i = 0U; i < text.size(); ++i) {
        char c = text[i];
        if (static_cast<unsigned char>(c) >= 0x80U) {
            return Result<InstanceText>::failure(Error::INVALID_ARGUMENT);
        }
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c + 32);
        }
        value.bytes[i] = c;
    }
    if (!ascii_alnum(value.bytes[0])) {
        return Result<InstanceText>::failure(Error::INVALID_ARGUMENT);
    }
    for (std::size_t i = 1U; i < text.size(); ++i) {
        const char c = value.bytes[i];
        if (!(ascii_alnum(c) || c == '_' || c == '.' || c == '-')) {
            return Result<InstanceText>::failure(Error::INVALID_ARGUMENT);
        }
    }
    if (value.bytes[text.size() - 1U] == '.') {
        return Result<InstanceText>::failure(Error::INVALID_ARGUMENT);
    }
    std::size_t stem = 0U;
    while (stem < text.size() && value.bytes[stem] != '.') {
        ++stem;
    }
    const auto stem_equals = [&](const char* word, std::size_t length) {
        return stem == length && std::memcmp(value.bytes.data(), word, length) == 0;
    };
    const bool reserved_word =
        stem_equals("con", 3U) || stem_equals("prn", 3U) || stem_equals("aux", 3U) ||
        stem_equals("nul", 3U);
    const bool reserved_device =
        stem == 4U && (std::memcmp(value.bytes.data(), "com", 3U) == 0 ||
                       std::memcmp(value.bytes.data(), "lpt", 3U) == 0) &&
        value.bytes[3] >= '1' && value.bytes[3] <= '9';
    if (reserved_word || reserved_device) {
        return Result<InstanceText>::failure(Error::INVALID_ARGUMENT);
    }
    return Result<InstanceText>::success(value);
}

Result<Selector> parse_device_selector(std::string_view text) noexcept
{
    if (text.size() >= 7U && text.substr(0U, 7U) == "serial:") {
        const auto serial = make_serial_selector(text.substr(7U));
        if (!serial) {
            return Result<Selector>::failure(serial.error());
        }
        Selector selector;
        selector.kind = SelectorKind::serial;
        selector.serial = serial.value();
        return Result<Selector>::success(selector);
    }
    if (text.size() >= 4U && text.substr(0U, 4U) == "usb:") {
        const std::string_view rest = text.substr(4U);
        // Bound the whole suffix before scanning: the largest legal set is
        // kMaxFunctions paths plus the separating commas.
        if (rest.empty() || rest.size() > detail::kMaxUsbSelectorSuffixBytes) {
            return Result<Selector>::failure(Error::INVALID_ARGUMENT);
        }
        std::array<std::string_view, detail::kMaxFunctions> tokens{};
        std::size_t count = 0U;
        std::size_t start = 0U;
        for (std::size_t i = 0U; i <= rest.size(); ++i) {
            if (i != rest.size() && rest[i] != ',') {
                continue;
            }
            const std::string_view token = rest.substr(start, i - start);
            start = i + 1U;
            if (token.empty() || token.size() > detail::kMaxUsbPathBytes ||
                !valid_usb_path(token.data(), token.size()) || count >= detail::kMaxFunctions) {
                return Result<Selector>::failure(Error::INVALID_ARGUMENT);
            }
            for (std::size_t j = 0U; j < count; ++j) {
                if (tokens[j] == token) {
                    return Result<Selector>::failure(Error::INVALID_ARGUMENT);
                }
            }
            tokens[count++] = token;
        }
        for (std::size_t i = 1U; i < count; ++i) {
            std::string_view key = tokens[i];
            std::size_t j = i;
            while (j > 0U && tokens[j - 1U] > key) {
                tokens[j] = tokens[j - 1U];
                --j;
            }
            tokens[j] = key;
        }
        Selector selector;
        selector.kind = SelectorKind::usb_topology;
        selector.topology.count = static_cast<std::uint16_t>(count);
        for (std::size_t i = 0U; i < count; ++i) {
            selector.topology.paths[i].length = static_cast<std::uint16_t>(tokens[i].size());
            std::memcpy(selector.topology.paths[i].bytes.data(), tokens[i].data(), tokens[i].size());
        }
        return Result<Selector>::success(selector);
    }
    return Result<Selector>::failure(Error::INVALID_ARGUMENT);
}

Result<std::uint16_t> decode_langid_first(ByteView descriptor) noexcept
{
    if (descriptor.data == nullptr && descriptor.size != 0U) {
        return Result<std::uint16_t>::failure(Error::INVALID_ARGUMENT);
    }
    if (descriptor.size < 2U) {
        return Result<std::uint16_t>::failure(Error::INVALID_ARGUMENT);
    }
    const std::uint8_t length = descriptor.data[0];
    if (descriptor.data[1] != kDescriptorTypeString) {
        return Result<std::uint16_t>::failure(Error::INVALID_ARGUMENT);
    }
    if (length != descriptor.size || descriptor.size < 4U || (descriptor.size % 2U) != 0U) {
        return Result<std::uint16_t>::failure(Error::INVALID_ARGUMENT);
    }
    const std::uint16_t langid = static_cast<std::uint16_t>(descriptor.data[2]) |
                                 static_cast<std::uint16_t>(descriptor.data[3] << 8U);
    return Result<std::uint16_t>::success(langid);
}

Result<DecodedString> decode_string_descriptor(ByteView descriptor) noexcept
{
    if (descriptor.data == nullptr && descriptor.size != 0U) {
        return Result<DecodedString>::failure(Error::INVALID_ARGUMENT);
    }
    if (descriptor.size < 2U) {
        return Result<DecodedString>::failure(Error::INVALID_ARGUMENT);
    }
    const std::uint8_t length = descriptor.data[0];
    if (descriptor.data[1] != kDescriptorTypeString) {
        return Result<DecodedString>::failure(Error::INVALID_ARGUMENT);
    }
    if (length != descriptor.size || ((descriptor.size - 2U) % 2U) != 0U) {
        return Result<DecodedString>::failure(Error::INVALID_ARGUMENT);
    }
    const std::size_t units = (descriptor.size - 2U) / 2U;
    DecodedString value;
    std::size_t out_length = 0U;
    for (std::size_t unit = 0U; unit < units; ++unit) {
        const std::uint16_t code = static_cast<std::uint16_t>(descriptor.data[2U + (unit * 2U)]) |
                                   static_cast<std::uint16_t>(descriptor.data[3U + (unit * 2U)]
                                                              << 8U);
        std::uint32_t codepoint = code;
        if (code >= 0xD800U && code <= 0xDBFFU) {
            if (unit + 1U >= units) {
                return Result<DecodedString>::failure(Error::INVALID_ARGUMENT);
            }
            const std::uint16_t low =
                static_cast<std::uint16_t>(descriptor.data[2U + ((unit + 1U) * 2U)]) |
                static_cast<std::uint16_t>(descriptor.data[3U + ((unit + 1U) * 2U)] << 8U);
            if (low < 0xDC00U || low > 0xDFFFU) {
                return Result<DecodedString>::failure(Error::INVALID_ARGUMENT);
            }
            codepoint = 0x10000U + ((static_cast<std::uint32_t>(code) - 0xD800U) << 10U) +
                        (static_cast<std::uint32_t>(low) - 0xDC00U);
            ++unit;
        } else if (code >= 0xDC00U && code <= 0xDFFFU) {
            return Result<DecodedString>::failure(Error::INVALID_ARGUMENT);
        }
        const std::size_t needed = codepoint < 0x80U    ? 1U
                                   : codepoint < 0x800U ? 2U
                                   : codepoint < 0x10000U ? 3U
                                                          : 4U;
        if (out_length + needed > detail::kMaxDecodedStringBytes) {
            return Result<DecodedString>::failure(Error::INVALID_ARGUMENT);
        }
        append_utf8(codepoint, value.bytes, out_length);
    }
    value.length = static_cast<std::uint16_t>(out_length);
    return Result<DecodedString>::success(value);
}

Result<ResolvedIdentity> resolve_selector(Product product, const Selector& selector) noexcept
{
    // Reuses the same product/serial/topology validators as the encoder; this is
    // a validated conversion, not a trust or grouping decision.
    if (product_slug(product) == nullptr) {
        return Result<ResolvedIdentity>::failure(Error::INVALID_ARGUMENT);
    }
    ResolvedIdentity identity;
    identity.product = product;
    switch (selector.kind) {
    case SelectorKind::serial:
        if (!valid_serial_text(selector.serial)) {
            return Result<ResolvedIdentity>::failure(Error::INVALID_ARGUMENT);
        }
        identity.kind = IdentityKind::serial;
        identity.serial = selector.serial;
        break;
    case SelectorKind::usb_topology:
        if (!prepare_topology(selector.topology, identity.topology)) {
            return Result<ResolvedIdentity>::failure(Error::INVALID_ARGUMENT);
        }
        identity.kind = IdentityKind::usb_topology;
        break;
    default:
        return Result<ResolvedIdentity>::failure(Error::INVALID_ARGUMENT);
    }
    return Result<ResolvedIdentity>::success(identity);
}

Result<std::size_t> encode_identity(const ResolvedIdentity& value, MutableByteView output) noexcept
{
    if (output.data == nullptr && output.size != 0U) {
        return Result<std::size_t>::failure(Error::INVALID_ARGUMENT);
    }
    // Only the two adopted discriminators exist.
    if (value.kind != IdentityKind::serial && value.kind != IdentityKind::usb_topology) {
        return Result<std::size_t>::failure(Error::INVALID_ARGUMENT);
    }
    const char* slug = product_slug(value.product);
    if (slug == nullptr) {
        return Result<std::size_t>::failure(Error::INVALID_ARGUMENT);
    }
    const std::size_t slug_length = std::strlen(slug);
    std::size_t required = 4U + 2U + 4U + slug_length + 1U;
    UsbTopology sorted;
    if (value.kind == IdentityKind::serial) {
        if (!valid_serial_text(value.serial)) {
            return Result<std::size_t>::failure(Error::INVALID_ARGUMENT);
        }
        required += 4U + value.serial.length;
    } else {
        if (!prepare_topology(value.topology, sorted)) {
            return Result<std::size_t>::failure(Error::INVALID_ARGUMENT);
        }
        required += 4U;
        for (std::size_t i = 0U; i < sorted.count; ++i) {
            required += 4U + sorted.paths[i].length;
        }
    }
    if (required > kMaxIdentityBytes) {
        return Result<std::size_t>::failure(Error::RESOURCE_LIMIT);
    }
    if (output.size < required) {
        return Result<std::size_t>::failure(Error::BUFFER_TOO_SMALL);
    }
    std::uint8_t* out = output.data;
    std::size_t position = 0U;
    out[position++] = 'D';
    out[position++] = 'T';
    out[position++] = 'V';
    out[position++] = 'I';
    store_u16(out + position, 1U);
    position += 2U;
    store_u32(out + position, static_cast<std::uint32_t>(slug_length));
    position += 4U;
    std::memcpy(out + position, slug, slug_length);
    position += slug_length;
    out[position++] = static_cast<std::uint8_t>(value.kind);
    if (value.kind == IdentityKind::serial) {
        store_u32(out + position, value.serial.length);
        position += 4U;
        std::memcpy(out + position, value.serial.bytes.data(), value.serial.length);
        position += value.serial.length;
    } else {
        store_u32(out + position, sorted.count);
        position += 4U;
        for (std::size_t i = 0U; i < sorted.count; ++i) {
            store_u32(out + position, sorted.paths[i].length);
            position += 4U;
            std::memcpy(out + position, sorted.paths[i].bytes.data(), sorted.paths[i].length);
            position += sorted.paths[i].length;
        }
    }
    return Result<std::size_t>::success(position);
}

Result<EncodedIdentity> encode_identity_bytes(const ResolvedIdentity& value) noexcept
{
    EncodedIdentity encoded;
    const auto written = encode_identity(
        value, MutableByteView{encoded.bytes.data(), encoded.bytes.size()});
    if (!written) {
        return Result<EncodedIdentity>::failure(written.error());
    }
    encoded.length = static_cast<std::uint16_t>(written.value());
    return Result<EncodedIdentity>::success(encoded);
}

Result<InstanceText> auto_instance(const ResolvedIdentity& value) noexcept
{
    const auto encoded = encode_identity_bytes(value);
    if (!encoded) {
        return Result<InstanceText>::failure(encoded.error());
    }
    detail::Sha256 hash;
    hash.update(encoded.value().bytes.data(), encoded.value().length);
    const std::array<std::uint8_t, 32U> digest = hash.finish();
    static constexpr char kHex[] = "0123456789abcdef";
    InstanceText instance;
    instance.bytes[0] = 'a';
    instance.bytes[1] = 'u';
    instance.bytes[2] = 't';
    instance.bytes[3] = 'o';
    instance.bytes[4] = '-';
    for (std::size_t i = 0U; i < 16U; ++i) {
        instance.bytes[5U + (i * 2U)] = kHex[digest[i] >> 4U];
        instance.bytes[6U + (i * 2U)] = kHex[digest[i] & 0x0FU];
    }
    instance.length = 5U + 32U;
    return Result<InstanceText>::success(instance);
}

}  // namespace dtv::userland::identity
