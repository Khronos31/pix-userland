// SPDX-License-Identifier: MIT
#ifndef PIX_RUNTIME_H
#define PIX_RUNTIME_H

#include "pix-dt181-consumer.h"
#include "pix-dt181-receiver-adapter.h"
#include "dtv/userland/identity.h"
#include "dtv/userland/host_runtime/family.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <array>
#include <memory>
#include <string>

namespace pix::runtime {

namespace dt181 = ::pix::capture::dt181;

enum class Model : std::uint8_t { dt181 = 0U, dt295 = 1U };

struct LocatedDevice final {
    Model model = Model::dt181;
    int fd = -1;
    usb_selector selector{};
    std::uint8_t bus = 0U;
    std::uint8_t address = 0U;
    std::array<std::uint8_t, USB_SELECTOR_MAX_PORTS> ports{};
    std::uint8_t port_count = 0U;
    std::string path;
    bool serial_present = false;
    bool serial_absent = false;
    bool serial_unique = false;
    bool serial_uniqueness_known = false;
    dtv::userland::identity::SerialText serial{};
};

dtv::userland::Result<LocatedDevice> locate(const std::string& selector) noexcept;
dtv::userland::Result<LocatedDevice> locate_fd(
    int fd, const std::string* selector = nullptr) noexcept;
dtv::userland::Result<LocatedDevice> locate_dt181(const std::string& selector) noexcept;

struct Assets final {
    std::uint8_t* firmware = nullptr;
    std::array<std::uint8_t, 16U> bootstrap{};
    std::array<std::uint8_t, 64U> validation{};
    ~Assets() noexcept;
    Assets() noexcept = default;
    Assets(const Assets&) = delete;
    Assets& operator=(const Assets&) = delete;
};

struct RuntimeFailureDiagnostic final {
    std::array<char, 32U> asset{};
    std::array<char, 192U> detail{};
    std::size_t expected_bytes = 0U;
    std::size_t actual_bytes = 0U;
    bool byte_counts_valid = false;
    bool actual_is_lower_bound = false;
};

/* Copy exact-size borrowed bytes into the existing owned, secure-clearable Assets object. */
dtv::userland::Result<std::unique_ptr<Assets>> load_assets_bytes(
    const std::uint8_t* firmware, std::size_t firmware_size,
    const std::uint8_t* bootstrap, std::size_t bootstrap_size,
    const std::uint8_t* validation, std::size_t validation_size,
    RuntimeFailureDiagnostic* diagnostic = nullptr) noexcept;

class Family final : public dtv::userland::host_runtime::EnclosureFamily {
public:
    static dtv::userland::Result<std::unique_ptr<Family>> create(
        const LocatedDevice&, const char* firmware_path,
        RuntimeFailureDiagnostic* diagnostic = nullptr) noexcept;
    ~Family() noexcept override;
    Family(const Family&) = delete;
    Family& operator=(const Family&) = delete;

    px4::userland::TunerServiceBackend& tuner_backend() noexcept override;
    px4::userland::CardServiceBackend& card_backend() noexcept override;
    px4::userland::CardProtocolSession& card_protocol() noexcept override;
    px4::userland::TunerServiceTime& time() noexcept override;
    px4::userland::TunerStreamControl* stream_control() noexcept override;
    px4::userland::Result<void> shutdown() noexcept override;

private:
    Family() noexcept = default;
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

dtv::userland::Result<void> build_list(
    const LocatedDevice&, const std::string& version,
    const std::array<std::uint8_t, 16U>& epoch,
    dtv::userland::wire::ListResponse&) noexcept;

}  // namespace pix::runtime

#endif
