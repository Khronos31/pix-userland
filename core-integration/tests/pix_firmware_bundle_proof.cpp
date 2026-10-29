// SPDX-License-Identifier: MIT
#include "pix-firmware-bundle.h"

#include <cassert>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace {

std::vector<std::uint8_t> read_file(const char* path)
{
    std::ifstream input(path, std::ios::binary);
    return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(input), {});
}

}  // namespace

int main(int argc, char** argv)
{
    assert(argc == 3);
    const auto dt181 = read_file(argv[1]);
    const auto dt295 = read_file(argv[2]);
    assert(!dt181.empty() && !dt295.empty());
    char error[192]{};
    pix::runtime::FirmwareBundleView view{};
    assert(pix::runtime::verify_firmware_bundle(pix::runtime::FirmwareModel::dt181,
        dt181.data(), dt181.size(), view, error, sizeof(error)));
    assert(view.bytes == dt181.data() && view.size == dt181.size());
    assert(view.fpga_size == 699673U && view.bootstrap_size == 16U &&
           view.validation_size == 64U);
    assert(pix::runtime::verify_firmware_bundle(pix::runtime::FirmwareModel::dt295,
        dt295.data(), dt295.size(), view, error, sizeof(error)));

    assert(!pix::runtime::verify_firmware_bundle(pix::runtime::FirmwareModel::dt295,
        dt181.data(), dt181.size(), view, error, sizeof(error)));
    auto changed_dt181 = dt181;
    changed_dt181.back() ^= 1U;
    assert(!pix::runtime::verify_firmware_bundle(pix::runtime::FirmwareModel::dt181,
        changed_dt181.data(), changed_dt181.size(), view, error, sizeof(error)));
    auto changed_dt295 = dt295;
    changed_dt295[changed_dt295.size() / 2U] ^= 1U;
    assert(!pix::runtime::verify_firmware_bundle(pix::runtime::FirmwareModel::dt295,
        changed_dt295.data(), changed_dt295.size(), view, error, sizeof(error)));
    assert(!pix::runtime::verify_firmware_bundle(pix::runtime::FirmwareModel::dt181,
        dt181.data(), dt181.size() - 1U, view, error, sizeof(error)));
    return 0;
}
