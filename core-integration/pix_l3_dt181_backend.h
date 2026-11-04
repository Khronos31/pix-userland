// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef PIX_L3_DT181_BACKEND_H
#define PIX_L3_DT181_BACKEND_H

#include "pix_l3_adapter.h"
#include "usb_backend.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Optional DT181 seam over the existing usb_backend. The product opens and
 * owns the USB session; this adapter exposes the existing bulk path only.
 * Control transfers and per-transfer cancel are not part of usb_backend, so
 * they report UNSUPPORTED instead of inventing a second USB path.
 */
typedef dtv_l3_result (*pix_l3_dt181_tune_fn)(
    void *context, uint32_t receiver_id, uint64_t requested_hz,
    uint64_t *programmed_hz);

typedef struct pix_l3_dt181_backend pix_l3_dt181_backend;

pix_l3_dt181_backend *pix_l3_dt181_backend_create(
    usb_backend *backend, int owns_backend, pix_l3_dt181_tune_fn tune,
    void *tune_context);
void pix_l3_dt181_backend_destroy(pix_l3_dt181_backend *backend);
pix_l3_backend_ops pix_l3_dt181_backend_ops(pix_l3_dt181_backend *backend);

#ifdef __cplusplus
}
#endif

#endif
