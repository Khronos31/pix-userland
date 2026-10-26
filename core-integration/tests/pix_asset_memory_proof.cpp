#include "pix_runtime.h"

#include <array>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#ifndef _WIN32
#include <fcntl.h>
#include <unistd.h>
#endif

int main(int argc, char** argv)
{
#ifndef _WIN32
    char diagnostic_root[] = "/tmp/pix-asset-diagnostic-XXXXXX";
    const bool made_root = ::mkdtemp(diagnostic_root) != nullptr;
    assert(made_root);
    pix::runtime::RuntimeFailureDiagnostic missing_diagnostic{};
    const auto missing_assets = pix::runtime::load_assets(
        diagnostic_root, &missing_diagnostic);
    const bool missing_ok = !missing_assets &&
        std::string(missing_diagnostic.asset.data()) == "fpga.bin" &&
        std::string(missing_diagnostic.detail.data()) == "not found";
    const std::string short_firmware = std::string(diagnostic_root) + "/fpga.bin";
    const int short_fd = ::open(short_firmware.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
    const std::uint8_t short_data[] = {0x41U, 0x42U, 0x43U};
    bool wrote_short = short_fd >= 0 && ::write(short_fd, short_data, sizeof(short_data)) ==
        static_cast<ssize_t>(sizeof(short_data));
    if (short_fd >= 0 && ::close(short_fd) != 0) wrote_short = false;
    pix::runtime::RuntimeFailureDiagnostic size_diagnostic{};
    const auto wrong_size_assets = pix::runtime::load_assets(
        diagnostic_root, &size_diagnostic);
    const bool size_ok = !wrong_size_assets &&
        std::string(size_diagnostic.asset.data()) == "fpga.bin" &&
        std::string(size_diagnostic.detail.data()) == "file size does not match" &&
        size_diagnostic.byte_counts_valid && size_diagnostic.expected_bytes == 699673U &&
        size_diagnostic.actual_bytes == sizeof(short_data);
    const std::string dt295_bundle = std::string(diagnostic_root) + "/dt295.assets";
    const int bundle_fd = ::open(dt295_bundle.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
    const std::uint8_t bad_bundle[] = {0x01U};
    bool wrote_bundle = bundle_fd >= 0 &&
        ::write(bundle_fd, bad_bundle, sizeof(bad_bundle)) ==
            static_cast<ssize_t>(sizeof(bad_bundle));
    if (bundle_fd >= 0 && ::close(bundle_fd) != 0) wrote_bundle = false;
    pix::runtime::LocatedDevice dt295_device{};
    dt295_device.model = pix::runtime::Model::dt295;
    pix::runtime::RuntimeFailureDiagnostic bundle_diagnostic{};
    const auto invalid_bundle = pix::runtime::Family::create(
        dt295_device, diagnostic_root, &bundle_diagnostic);
    const bool bundle_ok = !invalid_bundle &&
        invalid_bundle.error() == dtv::userland::Error::INVALID_ARGUMENT &&
        std::string(bundle_diagnostic.asset.data()) == "dt295.assets" &&
        std::string(bundle_diagnostic.detail.data()) ==
            "invalid DT295 asset bundle header";
    const bool removed_bundle = ::unlink(dt295_bundle.c_str()) == 0;
    const bool removed_file = ::unlink(short_firmware.c_str()) == 0;
    const bool removed_root = ::rmdir(diagnostic_root) == 0;
    assert(missing_ok && wrote_short && size_ok && wrote_bundle && bundle_ok &&
           removed_bundle && removed_file && removed_root);
#endif

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
