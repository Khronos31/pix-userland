// SPDX-License-Identifier: GPL-2.0-or-later
#include "pix_l3_dt181_backend.h"

namespace {

struct OpaqueUsbBackend {
    int unused;
};

}  // namespace

int main()
{
    /*
     * The seam only stores the pointer until owns_backend is set; this test
     * checks the advertised scope and omitted async hooks without I/O.
     */
    OpaqueUsbBackend opaque{};
    usb_backend *backend = reinterpret_cast<usb_backend *>(&opaque);
    pix_l3_dt181_backend *seam =
        pix_l3_dt181_backend_create(backend, 0, nullptr, nullptr);
    if (seam == nullptr) {
        return 1;
    }
    const pix_l3_usb_backend_ops usb = pix_l3_dt181_backend_usb_ops(seam);
    if (usb.cancel_scope != DTV_L3_CANCEL_SCOPE_PIPE ||
        usb.cancel_supported != 0U ||
        usb.transfer == nullptr || usb.cancel == nullptr ||
        usb.submit != nullptr || usb.cancel_async != nullptr) {
        return 2;
    }
    const pix_l3_product_ops product = pix_l3_dt181_backend_product_ops(seam);
    if (product.configure == nullptr || product.tune == nullptr ||
        product.bind == nullptr || product.initialize != nullptr ||
        product.stream_attach == nullptr || product.stream_stop == nullptr) {
        return 3;
    }
    pix_l3_dt181_backend_destroy(seam);
    return 0;
}
