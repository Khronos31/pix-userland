// SPDX-License-Identifier: GPL-2.0-or-later
#include "pix_l3_dt181_backend.h"

extern "C" {
#include "usb_backend.h"
void pix_test_usb_queue(const unsigned char *bytes, size_t length);
unsigned pix_test_usb_cancellations(void);
}

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>

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

bool wait_op(dtv_l3_op *op, dtv_l3_op_result *result, usb_backend *backend)
{
    for (int attempt = 0; attempt < 2000; ++attempt) {
        (void)usb_poll(backend, 10U);
        if (dtv_l3_op_poll(op, result) != DTV_L3_OK) {
            return false;
        }
        if (result->state == DTV_L3_OP_DONE ||
            result->state == DTV_L3_OP_FAILED) {
            return true;
        }
    }
    return false;
}

}  // namespace

int main()
{
    const int fd = open("/dev/null", O_RDWR);
    if (fd < 0) {
        return 1;
    }
    usb_backend *usb_backend_value = nullptr;
    const int opened =
        usb_open_model(&usb_backend_value, nullptr, fd, 0, USB_MODEL_DT181);
    close(fd);
    if (opened != 0 || usb_backend_value == nullptr) {
        return 2;
    }
    pix_l3_dt181_backend *backend =
        pix_l3_dt181_backend_create(usb_backend_value, 1, tune, nullptr);
    if (backend == nullptr) {
        usb_close(usb_backend_value);
        return 3;
    }

    pix_l3_factory_config config{};
    config.model = PIX_L3_MODEL_DT181;
    config.usb = pix_l3_dt181_backend_usb_ops(backend);
    config.product = pix_l3_dt181_backend_product_ops(backend);
    if (config.usb.cancel_scope != DTV_L3_CANCEL_SCOPE_PIPE ||
        config.usb.cancel_supported != 1U ||
        config.usb.submit == nullptr || config.usb.cancel_async == nullptr) {
        pix_l3_dt181_backend_destroy(backend);
        return 4;
    }
    pix_l3_factory *factory = nullptr;
    if (pix_l3_factory_create(&config, &factory) != DTV_L3_OK) {
        pix_l3_dt181_backend_destroy(backend);
        return 5;
    }
    dtv_l3_usb_provider usb{};
    dtv_l3_adapter_factory adapter_factory{};
    if (pix_l3_factory_usb_provider(factory, &usb) != DTV_L3_OK ||
        pix_l3_factory_adapter_factory(factory, &adapter_factory) !=
            DTV_L3_OK) {
        pix_l3_factory_destroy(factory);
        pix_l3_dt181_backend_destroy(backend);
        return 6;
    }

    const std::uint8_t firmware[1] = {0x01U};
    dtv_l3_firmware firmware_view{};
    firmware_view.source = DTV_L3_FIRMWARE_BYTES;
    firmware_view.bytes = firmware;
    firmware_view.size = sizeof(firmware);
    dtv_l3_open_config open_config{};
    open_config.model = PIX_L3_MODEL_DT181;
    open_config.selector = 0U;
    open_config.firmware = &firmware_view;
    dtv_l3_open_result owner =
        dtv_l3_open_factory(&usb, &adapter_factory, &open_config, 1U);
    if (owner.result != DTV_L3_ACCEPTED || owner.owner == nullptr) {
        pix_l3_factory_destroy(factory);
        pix_l3_dt181_backend_destroy(backend);
        return 7;
    }
    if (dtv_l3_op_wait(owner.op, 2000U) != DTV_L3_OK) {
        pix_l3_factory_destroy(factory);
        pix_l3_dt181_backend_destroy(backend);
        return 8;
    }
    dtv_l3_op_release(owner.op);

    std::uint8_t wire[188];
    std::memset(wire, 0x47, sizeof(wire));
    pix_test_usb_queue(wire, sizeof(wire));

    dtv_l3_transfer_request request{};
    request.kind = DTV_L3_TRANSFER_BULK;
    request.endpoint = 0x81U;
    request.timeout_ms = 100U;
    request.in_capacity = 188U;
    dtv_l3_op *op = nullptr;
    if (dtv_l3_transfer_start(owner.owner, &request, &op) != DTV_L3_ACCEPTED ||
        op == nullptr) {
        pix_l3_factory_destroy(factory);
        pix_l3_dt181_backend_destroy(backend);
        return 9;
    }
    dtv_l3_op_result result{};
    if (!wait_op(op, &result, usb_backend_value) ||
        result.state != DTV_L3_OP_DONE || result.result != DTV_L3_OK ||
        result.data_size != sizeof(wire) || result.data == nullptr ||
        result.data[0] != 0x47U) {
        pix_l3_factory_destroy(factory);
        pix_l3_dt181_backend_destroy(backend);
        return 10;
    }
    dtv_l3_op_release(op);

    if (dtv_l3_transfer_start(owner.owner, &request, &op) != DTV_L3_ACCEPTED ||
        op == nullptr) {
        pix_l3_factory_destroy(factory);
        pix_l3_dt181_backend_destroy(backend);
        return 11;
    }
    if (dtv_l3_context_release(owner.owner) != DTV_L3_BUSY) {
        return 16;
    }
    if (dtv_l3_op_cancel(op) != DTV_L3_ACCEPTED) {
        pix_l3_factory_destroy(factory);
        pix_l3_dt181_backend_destroy(backend);
        return 12;
    }
    if (dtv_l3_op_wait(op, 2000U) != DTV_L3_OK ||
        dtv_l3_op_poll(op, &result) != DTV_L3_OK ||
        result.state != DTV_L3_OP_FAILED ||
        result.result != DTV_L3_CANCELLED) {
        pix_l3_factory_destroy(factory);
        pix_l3_dt181_backend_destroy(backend);
        return 13;
    }
    if (pix_test_usb_cancellations() == 0U) {
        pix_l3_factory_destroy(factory);
        pix_l3_dt181_backend_destroy(backend);
        return 14;
    }
    dtv_l3_op_release(op);

    dtv_l3_close(owner.owner, 1000U);
    if (dtv_l3_context_release(owner.owner) != DTV_L3_OK) {
        pix_l3_factory_destroy(factory);
        pix_l3_dt181_backend_destroy(backend);
        return 15;
    }
    pix_l3_factory_destroy(factory);
    pix_l3_dt181_backend_destroy(backend);
    return 0;
}
