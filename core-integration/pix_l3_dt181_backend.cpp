// SPDX-License-Identifier: GPL-2.0-or-later
#include "pix_l3_dt181_backend.h"

#include "pixy.h"

#include <cstddef>
#include <cstdint>
#include <vector>

struct pix_l3_dt181_backend {
    usb_backend *backend = nullptr;
    int owns_backend = 0;
    int released = 0;
    pix_l3_dt181_tune_fn tune = nullptr;
    void *tune_context = nullptr;
};

namespace {

dtv_l3_claim_result claim(void *context, std::uint32_t claim_index) noexcept
{
    (void)context;
    (void)claim_index;
    return {DTV_L3_OK, DTV_L3_CLAIM_RETAINED};
}

dtv_l3_release_result release_claim(void *context,
                                    std::uint32_t claim_index) noexcept
{
    (void)claim_index;
    pix_l3_dt181_backend *value =
        static_cast<pix_l3_dt181_backend *>(context);
    if (value == nullptr) {
        return {DTV_L3_RELEASE_QUIESCENCE_UNKNOWN, DTV_L3_CLAIM_NONE,
                DTV_L3_RESTORE_NOT_ATTEMPTED, DTV_L3_INVALID};
    }
    if (value->owns_backend != 0 && value->released == 0) {
        /* usb_stream_stop drains callbacks/logger before usb_close. */
        const int stopped = usb_stream_stop(value->backend);
        usb_close(value->backend);
        value->backend = nullptr;
        value->released = 1;
        if (stopped != 0) {
            return {DTV_L3_RELEASE_QUIESCENCE_UNKNOWN,
                    DTV_L3_CLAIM_RETAINED, DTV_L3_RESTORE_NOT_ATTEMPTED,
                    DTV_L3_FAILED};
        }
    }
    return {DTV_L3_RELEASE_QUIESCED, DTV_L3_CLAIM_NONE,
            DTV_L3_RESTORE_NOT_ATTEMPTED, DTV_L3_OK};
}

dtv_l3_result transfer(void *context, std::uint64_t transfer_id,
                       const pix_l3_transfer *request,
                       std::uint8_t *in_buffer, std::size_t in_capacity,
                       pix_l3_transfer_result *completion) noexcept
{
    (void)transfer_id;
    pix_l3_dt181_backend *value =
        static_cast<pix_l3_dt181_backend *>(context);
    if (value == nullptr || value->backend == nullptr || request == nullptr ||
        completion == nullptr) {
        return DTV_L3_INVALID;
    }
    if (request->kind != DTV_L3_TRANSFER_BULK) {
        /* usb_backend exposes only the bulk pixy_io seam. */
        return DTV_L3_UNSUPPORTED;
    }
    if (request->endpoint == 0U) {
        return DTV_L3_INVALID;
    }

    const bool is_in = request->in_capacity != 0U;
    if (is_in) {
        if (request->out_size != 0U) {
            return DTV_L3_INVALID;
        }
    } else if (request->out_data == nullptr || request->out_size == 0U) {
        return DTV_L3_INVALID;
    }

    if (request->deadline_ns != DTV_L3_DEADLINE_NONE) {
        const std::uint64_t now_ns = now_ms() * 1000000ULL;
        if (now_ns >= request->deadline_ns) {
            completion->result = DTV_L3_TIMEOUT;
            return DTV_L3_OK;
        }
    }

    const std::size_t capacity = is_in ? in_capacity : request->out_size;
    if (is_in && (in_buffer == nullptr || in_capacity < request->in_capacity)) {
        return DTV_L3_INVALID;
    }
    std::vector<std::uint8_t> out_buffer(is_in ? 0U : capacity);
    std::uint8_t *buffer = is_in ? in_buffer : out_buffer.data();
    if (request->out_size != 0U) {
        for (std::size_t i = 0U; i < request->out_size; ++i) {
            buffer[i] = request->out_data[i];
        }
    }

    const pixy_io io = usb_io(value->backend);
    std::size_t actual = capacity;
    const int rc = io.bulk(io.user, static_cast<std::uint8_t>(request->endpoint),
                           buffer, &actual, request->timeout_ms);
    if (rc != 0) {
        completion->result = DTV_L3_FAILED;
        return DTV_L3_OK;
    }
    completion->result = DTV_L3_OK;
    completion->data = is_in ? buffer : nullptr;
    completion->size = is_in ? actual : 0U;
    completion->actual_length = actual;
    completion->actual_length_known = 1U;
    return DTV_L3_OK;
}

dtv_l3_result cancel(void *context, std::uint64_t transfer_id) noexcept
{
    (void)context;
    (void)transfer_id;
    /* usb_backend has no per-transfer cancel seam. */
    return DTV_L3_UNSUPPORTED;
}

dtv_l3_result tune(void *context, std::uint32_t receiver_id,
                   std::uint64_t requested_hz,
                   std::uint64_t *programmed_hz) noexcept
{
    pix_l3_dt181_backend *value =
        static_cast<pix_l3_dt181_backend *>(context);
    if (value == nullptr || programmed_hz == nullptr) {
        return DTV_L3_INVALID;
    }
    if (value->tune == nullptr) {
        return DTV_L3_UNSUPPORTED;
    }
    return value->tune(value->tune_context, receiver_id, requested_hz,
                       programmed_hz);
}

}  // namespace

extern "C" pix_l3_dt181_backend *pix_l3_dt181_backend_create(
    usb_backend *backend, int owns_backend, pix_l3_dt181_tune_fn tune_fn,
    void *tune_context)
{
    if (backend == nullptr) {
        return nullptr;
    }
    pix_l3_dt181_backend *value = new pix_l3_dt181_backend;
    value->backend = backend;
    value->owns_backend = owns_backend != 0 ? 1 : 0;
    value->tune = tune_fn;
    value->tune_context = tune_context;
    return value;
}

extern "C" void pix_l3_dt181_backend_destroy(pix_l3_dt181_backend *value)
{
    if (value == nullptr) {
        return;
    }
    if (value->owns_backend != 0 && value->released == 0) {
        usb_stream_stop(value->backend);
        usb_close(value->backend);
    }
    delete value;
}

extern "C" pix_l3_backend_ops pix_l3_dt181_backend_ops(
    pix_l3_dt181_backend *value)
{
    pix_l3_backend_ops ops{};
    ops.context = value;
    ops.claim = claim;
    ops.release_claim = release_claim;
    ops.transfer = transfer;
    ops.cancel = cancel;
    ops.tune = tune;
    return ops;
}
