// SPDX-License-Identifier: GPL-2.0-or-later
#include "pix_l3_dt295_io_backend.h"

#include <cstddef>
#include <cstdint>
#include <vector>

struct pix_l3_dt295_io_backend {
    dt295_io io{};
    pix_l3_dt295_tune_fn tune = nullptr;
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
    (void)context;
    (void)claim_index;
    return {DTV_L3_RELEASE_QUIESCED, DTV_L3_CLAIM_NONE,
            DTV_L3_RESTORE_NOT_ATTEMPTED, DTV_L3_OK};
}

dtv_l3_result transfer(void *context, std::uint64_t transfer_id,
                       const pix_l3_transfer *request,
                       std::uint8_t *in_buffer, std::size_t in_capacity,
                       pix_l3_transfer_result *completion) noexcept
{
    (void)transfer_id;
    pix_l3_dt295_io_backend *backend =
        static_cast<pix_l3_dt295_io_backend *>(context);
    if (backend == nullptr || request == nullptr || completion == nullptr) {
        return DTV_L3_INVALID;
    }
    if (backend->io.bulk == nullptr || backend->io.control == nullptr) {
        return DTV_L3_UNSUPPORTED;
    }
    if (request->kind != DTV_L3_TRANSFER_BULK &&
        request->kind != DTV_L3_TRANSFER_CONTROL) {
        return DTV_L3_UNSUPPORTED;
    }
    if (request->kind == DTV_L3_TRANSFER_CONTROL &&
        request->endpoint != 0U) {
        return DTV_L3_INVALID;
    }
    if (request->kind == DTV_L3_TRANSFER_BULK &&
        request->endpoint == 0U) {
        return DTV_L3_INVALID;
    }

    const bool is_in = request->kind == DTV_L3_TRANSFER_CONTROL
                           ? (request->request_type & 0x80U) != 0U
                           : request->in_capacity != 0U;
    if (is_in) {
        if (request->out_size != 0U) {
            return DTV_L3_INVALID;
        }
    } else if (request->out_data == nullptr || request->out_size == 0U) {
        return DTV_L3_INVALID;
    }

    if (request->deadline_ns != DTV_L3_DEADLINE_NONE &&
        backend->io.now_ms != nullptr) {
        const std::uint64_t now_ns = backend->io.now_ms(backend->io.user) *
                                     1000000ULL;
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

    std::size_t actual = 0U;
    int rc = 0;
    if (request->kind == DTV_L3_TRANSFER_CONTROL) {
        rc = backend->io.control(backend->io.user, request->request_type,
                                 request->request, request->value,
                                 request->index, buffer, capacity,
                                 request->timeout_ms, &actual);
    } else {
        rc = backend->io.bulk(backend->io.user,
                              static_cast<std::uint8_t>(request->endpoint),
                              buffer, capacity, request->timeout_ms, &actual);
    }
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
    /* The existing dt295_io has no transfer cancellation seam. */
    return DTV_L3_UNSUPPORTED;
}

dtv_l3_result tune(void *context, std::uint32_t receiver_id,
                   std::uint64_t requested_hz,
                   std::uint64_t *programmed_hz) noexcept
{
    pix_l3_dt295_io_backend *backend =
        static_cast<pix_l3_dt295_io_backend *>(context);
    if (backend == nullptr || programmed_hz == nullptr) {
        return DTV_L3_INVALID;
    }
    if (backend->tune == nullptr) {
        return DTV_L3_UNSUPPORTED;
    }
    return backend->tune(backend->tune_context, receiver_id, requested_hz,
                         programmed_hz);
}

}  // namespace

extern "C" pix_l3_dt295_io_backend *pix_l3_dt295_io_backend_create(
    const dt295_io *io, pix_l3_dt295_tune_fn tune_fn, void *tune_context)
{
    if (io == nullptr) {
        return nullptr;
    }
    pix_l3_dt295_io_backend *backend = new pix_l3_dt295_io_backend;
    backend->io = *io;
    backend->tune = tune_fn;
    backend->tune_context = tune_context;
    return backend;
}

extern "C" void pix_l3_dt295_io_backend_destroy(
    pix_l3_dt295_io_backend *backend)
{
    delete backend;
}

extern "C" pix_l3_backend_ops pix_l3_dt295_io_backend_ops(
    pix_l3_dt295_io_backend *backend)
{
    pix_l3_backend_ops ops{};
    ops.context = backend;
    ops.claim = claim;
    ops.release_claim = release_claim;
    ops.transfer = transfer;
    ops.cancel = cancel;
    ops.tune = tune;
    return ops;
}
