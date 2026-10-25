#include "pix_runtime.h"

#include <array>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

int main(int argc, char** argv)
{
    std::vector<std::uint8_t> firmware(699673U, 0x41U);
    std::array<std::uint8_t, 16U> bootstrap{};
    std::array<std::uint8_t, 64U> validation{};
    bootstrap.fill(0x52U);
    validation.fill(0x63U);

    auto result = pix::runtime::load_assets_bytes(
        firmware.data(), firmware.size(), bootstrap.data(), bootstrap.size(),
        validation.data(), validation.size());
    assert(result);
    auto assets = std::move(result.value());
    assert(assets && assets->firmware != nullptr);
    assert(std::memcmp(assets->firmware, firmware.data(), firmware.size()) == 0);
    assert(std::memcmp(assets->bootstrap.data(), bootstrap.data(), bootstrap.size()) == 0);
    assert(std::memcmp(assets->validation.data(), validation.data(), validation.size()) == 0);

    firmware[0] = 0;
    bootstrap[0] = 0;
    validation[0] = 0;
    assert(assets->firmware[0] == 0x41U);
    assert(assets->bootstrap[0] == 0x52U);
    assert(assets->validation[0] == 0x63U);

    assert(!pix::runtime::load_assets_bytes(nullptr, 699673U, bootstrap.data(), bootstrap.size(),
                                             validation.data(), validation.size()));
    assert(!pix::runtime::load_assets_bytes(firmware.data(), 699672U, bootstrap.data(),
                                             bootstrap.size(), validation.data(),
                                             validation.size()));
    assert(!pix::runtime::load_assets_bytes(firmware.data(), firmware.size(), bootstrap.data(),
                                             bootstrap.size() - 1U, validation.data(),
                                             validation.size()));
    assert(argc == 1 || argc == 2);
    if (argc == 2) {
        auto file_result = pix::runtime::load_assets(argv[1]);
        assert(file_result);
        auto file_assets = std::move(file_result.value());
        auto bytes_result = pix::runtime::load_assets_bytes(
            file_assets->firmware, 699673U, file_assets->bootstrap.data(),
            file_assets->bootstrap.size(), file_assets->validation.data(),
            file_assets->validation.size());
        assert(bytes_result);
        auto bytes_assets = std::move(bytes_result.value());
        assert(std::memcmp(file_assets->firmware, bytes_assets->firmware, 699673U) == 0);
        assert(file_assets->bootstrap == bytes_assets->bootstrap);
        assert(file_assets->validation == bytes_assets->validation);
    }
    return 0;
}
