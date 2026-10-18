// SPDX-License-Identifier: MIT
#include <dtv/userland/detail/sha256.h>
#include <dtv/userland/identity.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>

namespace {

namespace id = dtv::userland::identity;
using dtv::userland::ByteView;
using dtv::userland::Error;
using dtv::userland::MutableByteView;

int g_failures = 0;
int g_checks = 0;

#define CHECK(expr)                                                              \
    do {                                                                         \
        ++g_checks;                                                              \
        if (!(expr)) {                                                           \
            ++g_failures;                                                        \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, \
                         #expr);                                                 \
        }                                                                        \
    } while (0)

std::string hex_of(const std::uint8_t* data, std::size_t size)
{
    static const char digits[] = "0123456789abcdef";
    std::string out;
    out.reserve(size * 2U);
    for (std::size_t i = 0U; i < size; ++i) {
        out.push_back(digits[data[i] >> 4U]);
        out.push_back(digits[data[i] & 0x0FU]);
    }
    return out;
}

bool hex_equals(const std::uint8_t* data, std::size_t size, const char* expected)
{
    return hex_of(data, size) == expected;
}

id::SerialText serial_text(std::string_view text)
{
    const auto parsed = id::make_serial_selector(text);
    return parsed.value();
}

id::UsbTopology topology_of(const std::array<std::string_view, 8>& paths, std::size_t count)
{
    id::UsbTopology topology;
    topology.count = static_cast<std::uint16_t>(count);
    for (std::size_t i = 0U; i < count; ++i) {
        topology.paths[i].length = static_cast<std::uint16_t>(paths[i].size());
        std::memcpy(topology.paths[i].bytes.data(), paths[i].data(), paths[i].size());
    }
    return topology;
}

id::ResolvedIdentity serial_identity(std::string_view slug, std::string_view serial)
{
    id::ResolvedIdentity identity;
    identity.product = id::parse_product(slug).value();
    identity.kind = id::IdentityKind::serial;
    identity.serial = serial_text(serial);
    return identity;
}

id::ResolvedIdentity topology_identity(std::string_view slug,
                                       const std::array<std::string_view, 8>& paths,
                                       std::size_t count)
{
    id::ResolvedIdentity identity;
    identity.product = id::parse_product(slug).value();
    identity.kind = id::IdentityKind::usb_topology;
    identity.topology = topology_of(paths, count);
    return identity;
}

std::string instance_of(const id::ResolvedIdentity& identity)
{
    const auto instance = id::auto_instance(identity);
    return std::string(instance.value().bytes.data(), instance.value().length);
}

void test_vectors()
{
    {
        const auto encoded = id::encode_identity_bytes(serial_identity("px4-userland", "synthetic-serial-A"));
        CHECK(encoded.has_value());
        CHECK(hex_equals(encoded.value().bytes.data(), encoded.value().length,
                         "4454564901000c0000007078342d757365726c616e64011200000073796e7468"
                         "657469632d73657269616c2d41"));
        CHECK(instance_of(serial_identity("px4-userland", "synthetic-serial-A")) ==
              "auto-d265392b52b51111264e186936606102");
    }
    {
        const auto encoded = id::encode_identity_bytes(serial_identity("px4-userland", "synthetic-serial-B"));
        CHECK(encoded.has_value());
        CHECK(hex_equals(encoded.value().bytes.data(), encoded.value().length,
                         "4454564901000c0000007078342d757365726c616e64011200000073796e7468"
                         "657469632d73657269616c2d42"));
        CHECK(instance_of(serial_identity("px4-userland", "synthetic-serial-B")) ==
              "auto-7cd6704daa83b9c076a55c3977f92ee7");
    }
    const std::array<std::string_view, 8> reversed = {"1-2.2", "1-2.1", "", "", "", "", "", ""};
    const std::array<std::string_view, 8> ascending = {"1-2.1", "1-2.2", "", "", "", "", "", ""};
    for (const auto& input : {reversed, ascending}) {
        const auto identity = topology_identity("asicen-userland", input, 2U);
        const auto encoded = id::encode_identity_bytes(identity);
        CHECK(encoded.has_value());
        CHECK(hex_equals(encoded.value().bytes.data(), encoded.value().length,
                         "4454564901000f00000061736963656e2d757365726c616e640202000000050000"
                         "00312d322e3105000000312d322e32"));
        CHECK(instance_of(identity) == "auto-74f46abcd76c5d254051bb810ffc81ff");
    }
    {
        const std::string serial = "\xe8\xa9\xa6\xe9\xa8\x93 serial:with,comma";
        const auto encoded = id::encode_identity_bytes(serial_identity("pix-userland", serial));
        CHECK(encoded.has_value());
        CHECK(hex_equals(encoded.value().bytes.data(), encoded.value().length,
                         "4454564901000c0000007069782d757365726c616e640118000000e8a9a6e9a893"
                         "2073657269616c3a776974682c636f6d6d61"));
        CHECK(instance_of(serial_identity("pix-userland", serial)) ==
              "auto-04bbc46ffdb43912dc5f0a720a69f004");
    }
}

void test_identity_variations()
{
    // A one-character serial change, a product change and a kind change each
    // produce an independent instance.
    const std::string base = instance_of(serial_identity("px4-userland", "serial-X"));
    CHECK(base != instance_of(serial_identity("px4-userland", "serial-Y")));
    CHECK(base != instance_of(serial_identity("pix-userland", "serial-X")));
    CHECK(base != instance_of(serial_identity("asicen-userland", "serial-X")));
    const std::array<std::string_view, 8> paths = {"1-2.1", "", "", "", "", "", "", ""};
    CHECK(base != instance_of(topology_identity("px4-userland", paths, 1U)));
}

void test_serial_selector()
{
    CHECK(id::parse_device_selector("serial:abc").has_value());
    CHECK(id::parse_device_selector("serial:").error() == Error::INVALID_ARGUMENT);
    CHECK(id::parse_device_selector("bogus:1").error() == Error::INVALID_ARGUMENT);
    CHECK(id::parse_device_selector("").error() == Error::INVALID_ARGUMENT);
    // Space, colon and comma are serial bytes.
    CHECK(id::parse_device_selector("serial: a:b,c ").value().serial.length == 7U);
    // NUL and ASCII controls are rejected.
    CHECK(id::make_serial_selector(std::string_view("a\0b", 3U)).error() == Error::INVALID_ARGUMENT);
    CHECK(id::make_serial_selector(std::string_view("a\x01", 2U)).error() == Error::INVALID_ARGUMENT);
    CHECK(id::make_serial_selector(std::string_view("a\x7f", 2U)).error() == Error::INVALID_ARGUMENT);
    // Invalid and overlong UTF-8 are rejected; valid non-ASCII and combining are kept.
    CHECK(id::make_serial_selector("\xc3\x28").error() == Error::INVALID_ARGUMENT);
    const auto combining = id::make_serial_selector("e\xcc\x81");
    CHECK(combining.has_value() && combining.value().length == 3U);
    // 512 bytes accepted, 513 rejected.
    std::string max_serial(512U, 'a');
    CHECK(id::make_serial_selector(max_serial).has_value());
    max_serial.push_back('a');
    CHECK(id::make_serial_selector(max_serial).error() == Error::INVALID_ARGUMENT);
}

void test_usb_selector()
{
    const auto parsed = id::parse_device_selector("usb:1-2.2,1-2.1");
    CHECK(parsed.has_value() && parsed.value().kind == id::SelectorKind::usb_topology);
    CHECK(parsed.value().topology.count == 2U);
    CHECK(std::string(parsed.value().topology.paths[0].bytes.data(),
                      parsed.value().topology.paths[0].length) == "1-2.1");
    CHECK(std::string(parsed.value().topology.paths[1].bytes.data(),
                      parsed.value().topology.paths[1].length) == "1-2.2");

    CHECK(id::parse_device_selector("usb:").error() == Error::INVALID_ARGUMENT);
    CHECK(id::parse_device_selector("usb:1-2,").error() == Error::INVALID_ARGUMENT);
    CHECK(id::parse_device_selector("usb:1-2,,1-3").error() == Error::INVALID_ARGUMENT);
    CHECK(id::parse_device_selector("usb:1-2,1-2").error() == Error::INVALID_ARGUMENT);   // duplicate
    CHECK(id::parse_device_selector("usb:01-2").error() == Error::INVALID_ARGUMENT);      // leading zero
    CHECK(id::parse_device_selector("usb:1-0").error() == Error::INVALID_ARGUMENT);       // zero port
    CHECK(id::parse_device_selector("usb:1-2-3").error() == Error::INVALID_ARGUMENT);     // no hyphen grammar
    CHECK(id::parse_device_selector("usb:1-2.").error() == Error::INVALID_ARGUMENT);      // trailing dot
    CHECK(id::parse_device_selector("usb:1-4294967296").error() == Error::INVALID_ARGUMENT);  // overflow
    CHECK(id::parse_device_selector("usb:1-4294967295").has_value());                     // u32 max
    CHECK(id::parse_device_selector("usb:1-2,1-3,1-4,1-5,1-6,1-7,1-8,1-9,1-10")
              .error() == Error::INVALID_ARGUMENT);                                       // nine paths
    CHECK(id::parse_device_selector("usb:1-2,1-3,1-4,1-5,1-6,1-7,1-8,1-9").has_value());
    // 64-byte path accepted, 65 rejected.
    std::string path64 = "1-1";
    while (path64.size() < 61U) {
        path64 += ".1";
    }
    path64 += ".12";
    CHECK(path64.size() == 64U);
    CHECK(id::parse_device_selector("usb:" + path64).has_value());
    CHECK(id::parse_device_selector("usb:" + path64 + "1").error() == Error::INVALID_ARGUMENT);
}

void test_instance()
{
    CHECK(id::canonicalize_instance("livingroom").has_value());
    const auto upper = id::canonicalize_instance("RoomA");
    CHECK(upper.has_value() && upper.value().length == 5U &&
          std::string(upper.value().bytes.data(), upper.value().length) == "rooma");
    CHECK(id::canonicalize_instance("a.b-c_d1").has_value());
    CHECK(id::canonicalize_instance("a" + std::string(63U, 'b')).has_value());   // 64
    CHECK(id::canonicalize_instance("a" + std::string(64U, 'b')).error() == Error::INVALID_ARGUMENT);
    CHECK(id::canonicalize_instance("").error() == Error::INVALID_ARGUMENT);
    CHECK(id::canonicalize_instance("-abc").error() == Error::INVALID_ARGUMENT);
    CHECK(id::canonicalize_instance("_abc").error() == Error::INVALID_ARGUMENT);
    CHECK(id::canonicalize_instance("abc.").error() == Error::INVALID_ARGUMENT);
    CHECK(id::canonicalize_instance(".").error() == Error::INVALID_ARGUMENT);
    CHECK(id::canonicalize_instance("..").error() == Error::INVALID_ARGUMENT);
    CHECK(id::canonicalize_instance("a b").error() == Error::INVALID_ARGUMENT);
    CHECK(id::canonicalize_instance("a/b").error() == Error::INVALID_ARGUMENT);
    CHECK(id::canonicalize_instance("a:b").error() == Error::INVALID_ARGUMENT);
    CHECK(id::canonicalize_instance("caf\xc3\xa9").error() == Error::INVALID_ARGUMENT);  // non-ASCII
    // Reserved first-dot stems on every OS.
    CHECK(id::canonicalize_instance("con").error() == Error::INVALID_ARGUMENT);
    CHECK(id::canonicalize_instance("con.x").error() == Error::INVALID_ARGUMENT);
    CHECK(id::canonicalize_instance("PRN").error() == Error::INVALID_ARGUMENT);
    CHECK(id::canonicalize_instance("aux.1").error() == Error::INVALID_ARGUMENT);
    CHECK(id::canonicalize_instance("nul").error() == Error::INVALID_ARGUMENT);
    CHECK(id::canonicalize_instance("com1").error() == Error::INVALID_ARGUMENT);
    CHECK(id::canonicalize_instance("com9.x").error() == Error::INVALID_ARGUMENT);
    CHECK(id::canonicalize_instance("lpt1").error() == Error::INVALID_ARGUMENT);
    CHECK(id::canonicalize_instance("com10").has_value());
    CHECK(id::canonicalize_instance("console").has_value());
    CHECK(id::canonicalize_instance("lpt0").has_value());
}

void test_descriptors()
{
    // LANGID table: two languages, first 0x0409.
    const std::array<std::uint8_t, 6> langids = {0x06, 0x03, 0x09, 0x04, 0x09, 0x0C};
    const auto first = id::decode_langid_first(ByteView{langids.data(), langids.size()});
    CHECK(first.has_value() && first.value() == 0x0409U);
    const std::array<std::uint8_t, 2> too_short = {0x02, 0x03};
    CHECK(id::decode_langid_first(ByteView{too_short.data(), too_short.size()}).error() ==
          Error::INVALID_ARGUMENT);
    const std::array<std::uint8_t, 6> wrong_type = {0x06, 0x04, 0x09, 0x04, 0x09, 0x0C};
    CHECK(id::decode_langid_first(ByteView{wrong_type.data(), wrong_type.size()}).error() ==
          Error::INVALID_ARGUMENT);
    std::array<std::uint8_t, 5> odd = {0x05, 0x03, 0x09, 0x04, 0x00};
    CHECK(id::decode_langid_first(ByteView{odd.data(), odd.size()}).error() == Error::INVALID_ARGUMENT);
    std::array<std::uint8_t, 6> bad_length = langids;
    bad_length[0] = 0x05U;
    CHECK(id::decode_langid_first(ByteView{bad_length.data(), bad_length.size()}).error() ==
          Error::INVALID_ARGUMENT);

    // "Hi" in UTF-16LE.
    const std::array<std::uint8_t, 6> hi = {0x06, 0x03, 'H', 0x00, 'i', 0x00};
    const auto hi_decoded = id::decode_string_descriptor(ByteView{hi.data(), hi.size()});
    CHECK(hi_decoded.has_value());
    CHECK(std::string(hi_decoded.value().bytes.data(), hi_decoded.value().length) == "Hi");
    // Empty string descriptor is valid here.
    const std::array<std::uint8_t, 2> empty = {0x02, 0x03};
    const auto empty_decoded = id::decode_string_descriptor(ByteView{empty.data(), empty.size()});
    CHECK(empty_decoded.has_value() && empty_decoded.value().length == 0U);
    CHECK(id::make_serial_selector("").error() == Error::INVALID_ARGUMENT);
    // Non-BMP U+1F600 (surrogate pair) decodes losslessly to 4 UTF-8 bytes.
    const std::array<std::uint8_t, 6> emoji = {0x06, 0x03, 0x3D, 0xD8, 0x00, 0xDE};
    const auto emoji_decoded = id::decode_string_descriptor(ByteView{emoji.data(), emoji.size()});
    CHECK(emoji_decoded.has_value() && emoji_decoded.value().length == 4U);
    CHECK(static_cast<std::uint8_t>(emoji_decoded.value().bytes[0]) == 0xF0U &&
          static_cast<std::uint8_t>(emoji_decoded.value().bytes[3]) == 0x80U);
    // Combining sequence U+0065 U+0301 preserved verbatim.
    const std::array<std::uint8_t, 6> combining = {0x06, 0x03, 'e', 0x00, 0x01, 0x03};
    const auto combining_decoded = id::decode_string_descriptor(
        ByteView{combining.data(), combining.size()});
    CHECK(combining_decoded.has_value() && combining_decoded.value().length == 3U);
    // Invalid: isolated high, isolated low, odd payload, wrong type, truncation.
    const std::array<std::uint8_t, 4> isolated_high = {0x04, 0x03, 0x00, 0xD8};
    CHECK(id::decode_string_descriptor(ByteView{isolated_high.data(), isolated_high.size()}).error() ==
          Error::INVALID_ARGUMENT);
    const std::array<std::uint8_t, 4> isolated_low = {0x04, 0x03, 0x00, 0xDC};
    CHECK(id::decode_string_descriptor(ByteView{isolated_low.data(), isolated_low.size()}).error() ==
          Error::INVALID_ARGUMENT);
    const std::array<std::uint8_t, 5> odd_payload = {0x05, 0x03, 'H', 0x00, 0x00};
    CHECK(id::decode_string_descriptor(ByteView{odd_payload.data(), odd_payload.size()}).error() ==
          Error::INVALID_ARGUMENT);
    const std::array<std::uint8_t, 4> bad_type = {0x04, 0x04, 'H', 0x00};
    CHECK(id::decode_string_descriptor(ByteView{bad_type.data(), bad_type.size()}).error() ==
          Error::INVALID_ARGUMENT);
    const std::array<std::uint8_t, 3> truncated = {0x03, 0x03, 'H'};
    CHECK(id::decode_string_descriptor(ByteView{truncated.data(), truncated.size()}).error() ==
          Error::INVALID_ARGUMENT);
    const std::array<std::uint8_t, 1> single = {0x02};
    CHECK(id::decode_string_descriptor(ByteView{single.data(), single.size()}).error() ==
          Error::INVALID_ARGUMENT);
}

void test_duplicate_resolved_and_short_buffer()
{
    const std::array<std::string_view, 8> dup = {"1-2", "1-2", "", "", "", "", "", ""};
    const auto identity = topology_identity("asicen-userland", dup, 2U);
    CHECK(id::encode_identity_bytes(identity).error() == Error::INVALID_ARGUMENT);

    const auto probe = id::auto_instance(serial_identity("px4-userland", "synthetic-serial-A"));
    CHECK(probe.has_value());
    const auto encoded = id::encode_identity_bytes(serial_identity("px4-userland", "synthetic-serial-A"));
    CHECK(encoded.has_value());
    std::array<std::uint8_t, id::kMaxIdentityBytes> buffer{};
    buffer.fill(0xaaU);
    const auto short_written = id::encode_identity(
        serial_identity("px4-userland", "synthetic-serial-A"),
        MutableByteView{buffer.data(), encoded.value().length - 1U});
    CHECK(short_written.error() == Error::BUFFER_TOO_SMALL);
    bool untouched = true;
    for (std::size_t i = 0U; i < buffer.size(); ++i) {
        if (buffer[i] != 0xaaU) {
            untouched = false;
        }
    }
    CHECK(untouched);
}

void test_invalid_kinds_and_resolve()
{
    // Only kind 1 and 2 may be encoded/hashed. A valid topology with an
    // undefined discriminator is rejected by every entrypoint with no output.
    for (const std::uint8_t raw : {0U, 3U, 255U}) {
        id::ResolvedIdentity identity = topology_identity("px4-userland",
                                                          std::array<std::string_view, 8>{"1-2", "", "", "", "", "", "", ""}, 1U);
        identity.kind = static_cast<id::IdentityKind>(raw);
        std::array<std::uint8_t, id::kMaxIdentityBytes> buffer{};
        buffer.fill(0xaaU);
        CHECK(id::encode_identity(identity, MutableByteView{buffer.data(), buffer.size()}).error() ==
              Error::INVALID_ARGUMENT);
        bool untouched = true;
        for (std::size_t i = 0U; i < buffer.size(); ++i) {
            if (buffer[i] != 0xaaU) {
                untouched = false;
            }
        }
        CHECK(untouched);
        CHECK(id::encode_identity_bytes(identity).error() == Error::INVALID_ARGUMENT);
        CHECK(id::auto_instance(identity).error() == Error::INVALID_ARGUMENT);
    }

    // resolve_selector validates and canonicalizes instead of coercing.
    id::Selector bad_kind;
    bad_kind.kind = static_cast<id::SelectorKind>(3U);
    bad_kind.topology.count = 1U;
    bad_kind.topology.paths[0].length = 3U;
    std::memcpy(bad_kind.topology.paths[0].bytes.data(), "1-2", 3U);
    CHECK(id::resolve_selector(id::Product::px4, bad_kind).error() == Error::INVALID_ARGUMENT);
    CHECK(id::resolve_selector(static_cast<id::Product>(9U), bad_kind).error() ==
          Error::INVALID_ARGUMENT);

    // Serial payload validation.
    id::Selector serial_selector;
    serial_selector.kind = id::SelectorKind::serial;
    CHECK(id::resolve_selector(id::Product::px4, serial_selector).error() ==
          Error::INVALID_ARGUMENT);  // empty length
    serial_selector.serial.length = 3U;
    serial_selector.serial.bytes[0] = 'a';
    serial_selector.serial.bytes[1] = '\x01';
    serial_selector.serial.bytes[2] = 'b';
    CHECK(id::resolve_selector(id::Product::px4, serial_selector).error() == Error::INVALID_ARGUMENT);
    serial_selector.serial.length = 600U;  // oversized, rejected before any read
    CHECK(id::resolve_selector(id::Product::px4, serial_selector).error() == Error::INVALID_ARGUMENT);
    serial_selector.serial.length = 2U;
    serial_selector.serial.bytes[0] = 'a';
    serial_selector.serial.bytes[1] = 'b';
    const auto serial_ok = id::resolve_selector(id::Product::px4, serial_selector);
    CHECK(serial_ok.has_value() && serial_ok.value().kind == id::IdentityKind::serial &&
          serial_ok.value().serial.length == 2U);

    // Topology payload validation and canonical sorting.
    id::Selector topology_selector;
    topology_selector.kind = id::SelectorKind::usb_topology;
    topology_selector.topology.count = 0U;
    CHECK(id::resolve_selector(id::Product::px4, topology_selector).error() ==
          Error::INVALID_ARGUMENT);
    topology_selector.topology.count = 2U;
    topology_selector.topology.paths[0].length = 5U;
    std::memcpy(topology_selector.topology.paths[0].bytes.data(), "1-2.2", 5U);
    topology_selector.topology.paths[1].length = 5U;
    std::memcpy(topology_selector.topology.paths[1].bytes.data(), "1-2.1", 5U);
    const auto topology_ok = id::resolve_selector(id::Product::asicen, topology_selector);
    CHECK(topology_ok.has_value() && topology_ok.value().kind == id::IdentityKind::usb_topology &&
          topology_ok.value().topology.count == 2U &&
          std::string(topology_ok.value().topology.paths[0].bytes.data(),
                      topology_ok.value().topology.paths[0].length) == "1-2.1");
    topology_selector.topology.paths[1].length = 5U;
    std::memcpy(topology_selector.topology.paths[1].bytes.data(), "1-2.2", 5U);
    CHECK(id::resolve_selector(id::Product::asicen, topology_selector).error() ==
          Error::INVALID_ARGUMENT);  // duplicate
    topology_selector.topology.count = 9U;
    CHECK(id::resolve_selector(id::Product::asicen, topology_selector).error() ==
          Error::INVALID_ARGUMENT);  // excessive count
    id::Selector zero_length_path;
    zero_length_path.kind = id::SelectorKind::usb_topology;
    zero_length_path.topology.count = 1U;
    zero_length_path.topology.paths[0].length = 0U;
    CHECK(id::resolve_selector(id::Product::asicen, zero_length_path).error() ==
          Error::INVALID_ARGUMENT);
    id::Selector oversized_path;
    oversized_path.kind = id::SelectorKind::usb_topology;
    oversized_path.topology.count = 1U;
    oversized_path.topology.paths[0].length = 65U;
    CHECK(id::resolve_selector(id::Product::asicen, oversized_path).error() ==
          Error::INVALID_ARGUMENT);
}

void test_usb_suffix_bound()
{
    // Long single token is rejected without scanning beyond its own cap.
    CHECK(id::parse_device_selector("usb:" + std::string(200U, '1')).error() ==
          Error::INVALID_ARGUMENT);
    // Build eight distinct 64-byte paths: 8*64 + 7 = 519, the exact maximum.
    std::string path64 = "1-1";
    while (path64.size() < 61U) {
        path64 += ".1";
    }
    path64 += ".12";
    CHECK(path64.size() == 64U);
    std::string max_suffix;
    for (std::size_t i = 0U; i < 8U; ++i) {
        std::string path = path64;
        path[0] = static_cast<char>('1' + i);
        if (!max_suffix.empty()) {
            max_suffix += ",";
        }
        max_suffix += path;
    }
    CHECK(max_suffix.size() == (8U * 64U) + 7U);
    CHECK(id::parse_device_selector("usb:" + max_suffix).has_value());
    CHECK(id::parse_device_selector("usb:" + max_suffix + "x").error() == Error::INVALID_ARGUMENT);
}

void test_sha256()
{
    const auto digest_of = [](std::string_view text) {
        dtv::userland::detail::Sha256 hash;
        hash.update(reinterpret_cast<const std::uint8_t*>(text.data()), text.size());
        return hex_of(hash.finish().data(), 32U);
    };
    CHECK(digest_of("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK(digest_of("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");

    const std::array<std::pair<std::size_t, const char*>, 5> vectors = {{
        {55U, "463eb28e72f82e0a96c0a4cc53690c571281131f672aa229e0d45ae59b598b59"},
        {56U, "da2ae4d6b36748f2a318f23e7ab1dfdf45acdc9d049bd80e59de82a60895f562"},
        {63U, "29af2686fd53374a36b0846694cc342177e428d1647515f078784d69cdb9e488"},
        {64U, "fdeab9acf3710362bd2658cdc9a29e8f9c757fcf9811603a8c447cd1d9151108"},
        {65U, "4bfd2c8b6f1eec7a2afeb48b934ee4b2694182027e6d0fc075074f2fabb31781"},
    }};
    std::array<std::uint8_t, 256> data{};
    for (std::size_t i = 0U; i < data.size(); ++i) {
        data[i] = static_cast<std::uint8_t>(i);
    }
    for (const auto& entry : vectors) {
        const auto& [size, expected] = entry;
        dtv::userland::detail::Sha256 whole;
        whole.update(data.data(), size);
        const auto digest = whole.finish();
        CHECK(hex_of(digest.data(), 32U) == expected);
        // Split updates at several boundaries must match the single update.
        for (std::size_t split = 0U; split <= size; ++split) {
            dtv::userland::detail::Sha256 part;
            part.update(data.data(), split);
            part.update(data.data() + split, size - split);
            CHECK(hex_of(part.finish().data(), 32U) == expected);
        }
    }
}

}  // namespace

int main()
{
    test_vectors();
    test_identity_variations();
    test_serial_selector();
    test_usb_selector();
    test_instance();
    test_descriptors();
    test_duplicate_resolved_and_short_buffer();
    test_invalid_kinds_and_resolve();
    test_usb_suffix_bound();
    test_sha256();
    if (g_failures != 0) {
        std::fprintf(stderr, "identity_tests: %d of %d checks failed\n", g_failures, g_checks);
        return 1;
    }
    std::printf("identity_tests: %d checks passed\n", g_checks);
    return 0;
}
