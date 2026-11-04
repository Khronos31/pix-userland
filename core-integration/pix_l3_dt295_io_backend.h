// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef PIX_L3_DT295_IO_BACKEND_H
#define PIX_L3_DT295_IO_BACKEND_H

#include "dt295.h"
#include "pix_l3_adapter.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Optional DT295 seam. The product already owns the USB device session; this
 * adapter only routes the common L3 transfer/tune calls into the existing
 * dt295_io synchronous seam. The tune callback stays supplied by the product
 * so route/plan and firmware checks are not duplicated here. The caller must
 * stop the DT295 stream and keep DT295_STREAM_ABANDONED resources retained
 * before releasing the L3 context; this seam has no stream handle.
 */
typedef dtv_l3_result (*pix_l3_dt295_tune_fn)(
    void *context, uint32_t receiver_id, uint64_t requested_hz,
    uint64_t *programmed_hz);

typedef struct pix_l3_dt295_io_backend pix_l3_dt295_io_backend;

pix_l3_dt295_io_backend *pix_l3_dt295_io_backend_create(
    const dt295_io *io, pix_l3_dt295_tune_fn tune, void *tune_context);
void pix_l3_dt295_io_backend_destroy(pix_l3_dt295_io_backend *backend);
pix_l3_backend_ops pix_l3_dt295_io_backend_ops(
    pix_l3_dt295_io_backend *backend);

#ifdef __cplusplus
}
#endif

#endif
