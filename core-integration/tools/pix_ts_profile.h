// SPDX-License-Identifier: MIT
#ifndef PIX_CORE_INTEGRATION_PIX_TS_PROFILE_H
#define PIX_CORE_INTEGRATION_PIX_TS_PROFILE_H

#include <dtv/host_ts/px4_ts_core.h>
#include <dtv/userland/identity.h>

namespace pix::core_integration {

// Serial selection and optional --system follow the shared PIX product rules.
// The instance spelling is kept because pixd publishes the original token.
inline const px4::userland::cli::TsClientProfile kPixTsProfile{
    "pix-ts",
    "pix-userland",
    dtv::userland::identity::Product::pix,
    true,
    true,
    nullptr,
    true,
};

}  // namespace pix::core_integration

#endif  // PIX_CORE_INTEGRATION_PIX_TS_PROFILE_H
