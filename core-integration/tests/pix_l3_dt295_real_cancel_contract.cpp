// SPDX-License-Identifier: GPL-2.0-or-later
#include "pix_l3_dt295_io_backend.h"

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace {

dtv_l3_result tune(void *, uint32_t receiver_id, uint64_t requested_hz,
                   uint64_t *programmed_hz)
{
    if (receiver_id != 0U) {
        return DTV_L3_UNSUPPORTED;
    }
    *programmed_hz = requested_hz;
    return DTV_L3_OK;
}

bool wait_op(dtv_l3_op *op, dtv_l3_op_result *result)
{
    for (int attempt = 0; attempt < 3000; ++attempt) {
        if (dtv_l3_op_poll(op, result) != DTV_L3_OK) {
            return false;
        }
        if (result->state == DTV_L3_OP_DONE ||
            result->state == DTV_L3_OP_FAILED) {
            return true;
        }
        if (dtv_l3_op_wait(op, 10U) != DTV_L3_OK &&
            result->state != DTV_L3_OP_DONE &&
            result->state != DTV_L3_OP_FAILED) {
            return false;
        }
    }
    return false;
}

}  // namespace

int main()
{
    dt295_stream *stream = dt295_stream_test_create(8U);
    if (stream == nullptr) {
        return 1;
    }
    dt295_io io{};
    pix_l3_dt295_io_backend *backend =
        pix_l3_dt295_io_backend_create(&io, tune, nullptr);
    if (backend == nullptr) {
        dt295_stream_test_mark_stopped(stream);
        dt295_stream_destroy(stream);
        return 2;
    }
    pix_l3_dt295_io_backend_set_stream(backend, stream);

    pix_l3_factory_config config{};
    config.model = PIX_L3_MODEL_DT295;
    config.usb = pix_l3_dt295_io_backend_usb_ops(backend);
    config.product = pix_l3_dt295_io_backend_product_ops(backend);
    if (config.usb.cancel_scope != DTV_L3_CANCEL_SCOPE_PIPE ||
        config.usb.cancel_supported != 1U ||
        config.usb.submit == nullptr || config.usb.cancel_async == nullptr) {
        pix_l3_dt295_io_backend_destroy(backend);
        dt295_stream_test_mark_stopped(stream);
        dt295_stream_destroy(stream);
        return 3;
    }
    pix_l3_factory *factory = nullptr;
    if (pix_l3_factory_create(&config, &factory) != DTV_L3_OK) {
        pix_l3_dt295_io_backend_destroy(backend);
        dt295_stream_test_mark_stopped(stream);
        dt295_stream_destroy(stream);
        return 4;
    }
    dtv_l3_usb_provider usb{};
    dtv_l3_adapter_factory adapter_factory{};
    if (pix_l3_factory_usb_provider(factory, &usb) != DTV_L3_OK ||
        pix_l3_factory_adapter_factory(factory, &adapter_factory) !=
            DTV_L3_OK) {
        return 5;
    }
    const std::uint8_t firmware[1] = {0x01U};
    dtv_l3_firmware firmware_view{};
    firmware_view.source = DTV_L3_FIRMWARE_BYTES;
    firmware_view.bytes = firmware;
    firmware_view.size = sizeof(firmware);
    dtv_l3_open_config open_config{};
    open_config.model = PIX_L3_MODEL_DT295;
    open_config.selector = 0U;
    open_config.firmware = &firmware_view;
    dtv_l3_open_result owner =
        dtv_l3_open_factory(&usb, &adapter_factory, &open_config, 1U);
    if (owner.result != DTV_L3_ACCEPTED || owner.owner == nullptr) {
        return 6;
    }
    if (dtv_l3_op_wait(owner.op, 2000U) != DTV_L3_OK) {
        return 7;
    }
    dtv_l3_op_release(owner.op);

    if (dt295_stream_test_complete(stream, 0U, LIBUSB_TRANSFER_COMPLETED, 188,
                                   0x47U) != 0) {
        return 8;
    }
    dtv_l3_transfer_request request{};
    request.kind = DTV_L3_TRANSFER_BULK;
    request.endpoint = 0x81U;
    request.timeout_ms = 100U;
    request.in_capacity = 188U;
    dtv_l3_op *op = nullptr;
    if (dtv_l3_transfer_start(owner.owner, &request, &op) != DTV_L3_ACCEPTED ||
        op == nullptr) {
        return 9;
    }
    dtv_l3_op_result result{};
    if (!wait_op(op, &result) || result.state != DTV_L3_OP_DONE ||
        result.result != DTV_L3_OK || result.data_size != 188U ||
        result.data == nullptr || result.data[0] != 0x47U) {
        return 10;
    }
    dtv_l3_op_release(op);

    if (dtv_l3_transfer_start(owner.owner, &request, &op) != DTV_L3_ACCEPTED ||
        op == nullptr) {
        return 11;
    }
    if (dtv_l3_context_release(owner.owner) != DTV_L3_BUSY) {
        return 16;
    }
    if (dtv_l3_op_cancel(op) != DTV_L3_ACCEPTED) {
        return 12;
    }
    if (dtv_l3_op_wait(op, 3000U) != DTV_L3_OK ||
        dtv_l3_op_poll(op, &result) != DTV_L3_OK ||
        result.state != DTV_L3_OP_FAILED ||
        result.result != DTV_L3_CANCELLED) {
        return 13;
    }
    dtv_l3_op_release(op);

    dt295_stream_test_mark_stopped(stream);
    dtv_l3_close(owner.owner, 1000U);
    if (dtv_l3_context_release(owner.owner) != DTV_L3_OK) {
        return 14;
    }
    pix_l3_factory_destroy(factory);
    pix_l3_dt295_io_backend_destroy(backend);
    if (dt295_stream_destroy(stream) != 0) {
        return 15;
    }
    return 0;
}
