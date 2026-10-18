// SPDX-License-Identifier: MIT
// Installed-package consumer for the portable identity codecs. It links only
// the installed dtv::core / dtv::identity targets and includes the installed
// detail headers, never any product tree.
#include <dtv/userland/identity.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

int main()
{
    using namespace dtv::userland;
    namespace id = dtv::userland::identity;

    const auto parsed = id::parse_device_selector("usb:1-2.2,1-2.1");
    if (!parsed || parsed.value().topology.count != 2U) {
        return 1;
    }
    id::ResolvedIdentity identity;
    identity.product = id::parse_product("asicen-userland").value();
    identity.kind = id::IdentityKind::usb_topology;
    identity.topology = parsed.value().topology;

    const auto encoded = id::encode_identity_bytes(identity);
    const auto instance = id::auto_instance(identity);
    const auto canonical = id::canonicalize_instance("RoomA");

    const std::array<std::uint8_t, 6> descriptor = {0x06, 0x03, 'H', 0x00, 'i', 0x00};
    const auto decoded =
        id::decode_string_descriptor(ByteView{descriptor.data(), descriptor.size()});

    const bool ok = encoded && encoded.value().length == 48U && instance &&
                    std::string_view(instance.value().bytes.data(), instance.value().length) ==
                        "auto-74f46abcd76c5d254051bb810ffc81ff" &&
                    canonical && canonical.value().length == 5U &&
                    std::memcmp(canonical.value().bytes.data(), "rooma", 5U) == 0 && decoded &&
                    std::string(decoded.value().bytes.data(), decoded.value().length) == "Hi";
    return ok ? 0 : 1;
}
