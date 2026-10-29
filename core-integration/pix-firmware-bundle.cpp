// SPDX-License-Identifier: MIT
#include "pix-firmware-bundle.h"

#include "pix_firmware_manifest_generated.h"
extern "C" {
#include "dt295.h"
#include "dt295_crypto.h"
}

#include <array>
#include <cstdio>
#include <cstring>

namespace pix::runtime {
namespace {

constexpr std::array<std::uint8_t, 8U> kDt181Magic{
    'P', '1', '8', '1', 'A', '1', 0U, 0U};

void set_error(char* error, std::size_t error_size, const char* value) noexcept
{
    if (error == nullptr || error_size == 0U) return;
    std::snprintf(error, error_size, "%s", value);
}

bool matches_sha256(const std::uint8_t* bytes, std::size_t size,
                    const std::uint8_t expected[32]) noexcept
{
    std::uint8_t actual[32]{};
    dt295_sha256(bytes, size, actual);
    return std::memcmp(actual, expected, sizeof(actual)) == 0;
}

std::uint32_t read_le32(const std::uint8_t* bytes) noexcept
{
    return static_cast<std::uint32_t>(bytes[0]) |
           (static_cast<std::uint32_t>(bytes[1]) << 8U) |
           (static_cast<std::uint32_t>(bytes[2]) << 16U) |
           (static_cast<std::uint32_t>(bytes[3]) << 24U);
}

}  // namespace

bool verify_firmware_bundle(FirmwareModel model, const std::uint8_t* bytes,
                            std::size_t size, FirmwareBundleView& view,
                            char* error, std::size_t error_size) noexcept
{
    view = FirmwareBundleView{};
    if (bytes == nullptr) {
        set_error(error, error_size, "firmware bundle is missing");
        return false;
    }
    if (model == FirmwareModel::dt181) {
        if (size != PIX_DT181_BUNDLE_SIZE) {
            set_error(error, error_size, "DT181 firmware bundle size is invalid");
            return false;
        }
        if (!matches_sha256(bytes, size, PIX_DT181_BUNDLE_SHA256)) {
            set_error(error, error_size, "unrecognized DT181 firmware bundle SHA-256");
            return false;
        }
        if (size < 20U || std::memcmp(bytes, kDt181Magic.data(), kDt181Magic.size()) != 0 ||
            read_le32(bytes + 8U) != PIX_DT181_FPGA_SIZE ||
            read_le32(bytes + 12U) != PIX_DT181_BOOTSTRAP_SIZE ||
            read_le32(bytes + 16U) != PIX_DT181_VALIDATION_SIZE ||
            size != 20U + PIX_DT181_FPGA_SIZE + PIX_DT181_BOOTSTRAP_SIZE +
                PIX_DT181_VALIDATION_SIZE) {
            set_error(error, error_size, "DT181 firmware bundle format is invalid");
            return false;
        }
        const auto matches_section = [](const std::uint8_t* section, std::size_t length,
                                        const std::uint8_t expected[32]) {
            std::uint8_t actual[32]{};
            dt295_sha256(section, length, actual);
            return std::memcmp(actual, expected, sizeof(actual)) == 0;
        };
        view.bytes = bytes;
        view.size = size;
        view.fpga = bytes + 20U;
        view.fpga_size = PIX_DT181_FPGA_SIZE;
        view.bootstrap = view.fpga + view.fpga_size;
        view.bootstrap_size = PIX_DT181_BOOTSTRAP_SIZE;
        view.validation = view.bootstrap + view.bootstrap_size;
        view.validation_size = PIX_DT181_VALIDATION_SIZE;
        if (!matches_section(view.fpga, view.fpga_size, PIX_DT181_FPGA_SHA256) ||
            !matches_section(view.bootstrap, view.bootstrap_size, PIX_DT181_BOOTSTRAP_SHA256) ||
            !matches_section(view.validation, view.validation_size, PIX_DT181_VALIDATION_SHA256)) {
            view = FirmwareBundleView{};
            set_error(error, error_size, "DT181 firmware section SHA-256 is invalid");
            return false;
        }
        return true;
    }
    if (size != PIX_DT295_BUNDLE_SIZE) {
        set_error(error, error_size, "DT295 firmware bundle size is invalid");
        return false;
    }
    if (!matches_sha256(bytes, size, PIX_DT295_BUNDLE_SHA256)) {
        set_error(error, error_size, "unrecognized DT295 firmware bundle SHA-256");
        return false;
    }
    dt295_assets* assets = nullptr;
    char detail[160]{};
    const int parsed = dt295_assets_load_bytes(bytes, size, &assets, detail, sizeof(detail));
    dt295_assets_free(assets);
    if (parsed != 0) {
        set_error(error, error_size, detail[0] == '\0'
            ? "DT295 firmware bundle format is invalid" : detail);
        return false;
    }
    view.bytes = bytes;
    view.size = size;
    return true;
}

}  // namespace pix::runtime
