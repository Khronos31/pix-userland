// SPDX-License-Identifier: MIT
#ifndef PIX_FIRMWARE_BUNDLE_H
#define PIX_FIRMWARE_BUNDLE_H

#include <cstddef>
#include <cstdint>

namespace pix::runtime {

enum class FirmwareModel : std::uint8_t { dt181, dt295 };

struct FirmwareBundleView final {
    const std::uint8_t* bytes = nullptr;
    std::size_t size = 0U;
    const std::uint8_t* fpga = nullptr;
    std::size_t fpga_size = 0U;
    const std::uint8_t* bootstrap = nullptr;
    std::size_t bootstrap_size = 0U;
    const std::uint8_t* validation = nullptr;
    std::size_t validation_size = 0U;
};

bool verify_firmware_bundle(FirmwareModel model, const std::uint8_t* bytes,
                            std::size_t size, FirmwareBundleView& view,
                            char* error, std::size_t error_size) noexcept;

}  // namespace pix::runtime

#endif
