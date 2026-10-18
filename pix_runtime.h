// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef PIX_RUNTIME_H
#define PIX_RUNTIME_H

#include "pix-dt181-consumer.h"
#include "pix-dt181-receiver-adapter.h"
#include "px4/enclosure_context.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace pix::runtime {

namespace dt181 = ::pix::capture::dt181;

struct LocatedDevice final {
    usb_selector selector{};
    std::uint8_t bus = 0U;
    std::uint8_t address = 0U;
    std::array<std::uint8_t, USB_SELECTOR_MAX_PORTS> ports{};
    std::uint8_t port_count = 0U;
    std::string path;
};

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

dtv::userland::Result<std::unique_ptr<Assets>> load_assets(const char* directory) noexcept;

class Family final : public px4::userland::ipc::posix::EnclosureFamily {
public:
    static dtv::userland::Result<std::unique_ptr<Family>> create(
        const LocatedDevice&, const char* assets_directory) noexcept;
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
    const LocatedDevice&, const std::string& instance,
    const std::string& version,
    const std::array<std::uint8_t, px4::userland::ipc::kNonceLength>& epoch,
    px4::userland::ipc::posix::ControlContext&) noexcept;

}  // namespace pix::runtime

#endif
