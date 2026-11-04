// SPDX-License-Identifier: GPL-2.0-or-later
#include "pix_l3_dt295_io_backend.h"

#include <cstdint>
#include <cstring>

namespace {

struct FakeIo final {
    uint64_t control_calls = 0U;
    uint64_t bulk_calls = 0U;
    uint8_t request_type = 0U;
    uint8_t request = 0U;
    uint16_t value = 0U;
    uint16_t index = 0U;
    uint8_t endpoint = 0U;
    std::uint8_t last_out[4] = {};
    uint64_t tune_value = 0U;
    bool expired = false;
};

int control(void *user, uint8_t request_type, uint8_t request, uint16_t value,
            uint16_t index, uint8_t *data, size_t length, unsigned timeout_ms,
            size_t *actual)
{
    (void)timeout_ms;
    FakeIo *io = static_cast<FakeIo *>(user);
    ++io->control_calls;
    io->request_type = request_type;
    io->request = request;
    io->value = value;
    io->index = index;
    if ((request_type & 0x80U) != 0U) {
        for (size_t i = 0U; i < length; ++i) {
            data[i] = 0x5AU;
        }
        *actual = length;
    } else {
        *actual = length;
    }
    return 0;
}

int bulk(void *user, uint8_t endpoint, uint8_t *data, size_t length,
         unsigned timeout_ms, size_t *actual)
{
    (void)timeout_ms;
    FakeIo *io = static_cast<FakeIo *>(user);
    ++io->bulk_calls;
    io->endpoint = endpoint;
    if ((endpoint & 0x80U) == 0U) {
        for (size_t i = 0U; i < length && i < sizeof(io->last_out); ++i) {
            io->last_out[i] = data[i];
        }
        *actual = length;
    } else {
        for (size_t i = 0U; i < length; ++i) {
            data[i] = 0xA5U;
        }
        *actual = length;
    }
    return 0;
}

uint64_t now_ms(void *user)
{
    FakeIo *io = static_cast<FakeIo *>(user);
    return io->expired ? 1000U : 0U;
}

void sleep_ms(void *user, unsigned ms)
{
    (void)user;
    (void)ms;
}

dtv_l3_result tune(void *context, uint32_t receiver_id, uint64_t requested_hz,
                   uint64_t *programmed_hz)
{
    FakeIo *io = static_cast<FakeIo *>(context);
    if (receiver_id != 0U) {
        return DTV_L3_UNSUPPORTED;
    }
    io->tune_value = requested_hz;
    *programmed_hz = requested_hz;
    return DTV_L3_OK;
}

bool wait_op(dtv_l3_op *op)
{
    if (dtv_l3_op_wait(op, 3000U) != DTV_L3_OK) {
        return false;
    }
    dtv_l3_op_result result{};
    return dtv_l3_op_poll(op, &result) == DTV_L3_OK &&
           (result.state == DTV_L3_OP_DONE ||
            result.state == DTV_L3_OP_FAILED);
}

}  // namespace

int main()
{
    FakeIo io;
    dt295_io seam{};
    seam.user = &io;
    seam.control = control;
    seam.bulk = bulk;
    seam.now_ms = now_ms;
    seam.sleep_ms = sleep_ms;

    pix_l3_dt295_io_backend *backend =
        pix_l3_dt295_io_backend_create(&seam, tune, &io);
    if (backend == nullptr) {
        return 1;
    }

    pix_l3_factory_config config{};
    config.model = PIX_L3_MODEL_DT295;
    config.usb = pix_l3_dt295_io_backend_usb_ops(backend);
    config.product = pix_l3_dt295_io_backend_product_ops(backend);
    if (config.usb.cancel_scope != DTV_L3_CANCEL_SCOPE_PIPE ||
        config.usb.cancel_supported != 0U ||
        config.usb.submit != nullptr || config.usb.cancel_async != nullptr ||
        config.usb.transfer == nullptr || config.usb.cancel == nullptr ||
        config.product.bind == nullptr ||
        config.product.initialize != nullptr ||
        config.product.stream_attach == nullptr ||
        config.product.stream_stop == nullptr) {
        return 19;
    }
    pix_l3_factory *factory = nullptr;
    if (pix_l3_factory_create(&config, &factory) != DTV_L3_OK) {
        return 2;
    }
    dtv_l3_usb_provider usb{};
    dtv_l3_adapter_factory adapter_factory{};
    if (pix_l3_factory_usb_provider(factory, &usb) != DTV_L3_OK ||
        pix_l3_factory_adapter_factory(factory, &adapter_factory) !=
            DTV_L3_OK) {
        return 3;
    }

    const std::uint8_t firmware[4] = {0x10U, 0x20U, 0x30U, 0x40U};
    dtv_l3_firmware firmware_view{};
    firmware_view.source = DTV_L3_FIRMWARE_BYTES;
    firmware_view.bytes = firmware;
    firmware_view.size = sizeof(firmware);
    dtv_l3_open_config open_config{};
    open_config.model = PIX_L3_MODEL_DT295;
    open_config.selector = 9U;
    open_config.firmware = &firmware_view;
    dtv_l3_open_result opened =
        dtv_l3_open_factory(&usb, &adapter_factory, &open_config, 1U);
    if (opened.result != DTV_L3_ACCEPTED || opened.owner == nullptr) {
        return 4;
    }
    if (dtv_l3_op_wait(opened.op, 1000U) != DTV_L3_OK) {
        return 5;
    }
    dtv_l3_op_release(opened.op);

    dtv_l3_lease *lease = nullptr;
    if (dtv_l3_lease_acquire(opened.owner, 0U, &lease) != DTV_L3_OK) {
        return 6;
    }
    uint64_t programmed = 0U;
    uint64_t revision = 0U;
    if (dtv_l3_lease_tune(lease, 522000000U, &programmed, &revision) !=
            DTV_L3_OK ||
        programmed != 522000000U || io.tune_value != 522000000U) {
        return 7;
    }
    if (dtv_l3_lease_start(lease) != DTV_L3_OK) {
        return 20;
    }
    dtv_l3_stream *stream = nullptr;
    if (dtv_l3_lease_attach(lease, &stream) != DTV_L3_OK ||
        stream == nullptr) {
        return 21;
    }
    std::uint8_t ts[188];
    std::memset(ts, 0, sizeof(ts));
    std::size_t ts_bytes = 0U;
    dtv_l3_read_state ts_state = DTV_L3_READ_WAIT;
    if (dtv_l3_stream_read_ts(stream, ts, sizeof(ts), &ts_bytes, &ts_state) !=
            DTV_L3_OK ||
        ts_state != DTV_L3_READ_DATA || ts_bytes != sizeof(ts) ||
        ts[0] != 0xA5U || io.bulk_calls != 1U) {
        return 22;
    }
    if (dtv_l3_stream_stop(stream, 0U) != DTV_L3_OK ||
        dtv_l3_stream_release(stream, DTV_L3_STREAM_RELEASE_DISCARD_UNREAD) !=
            DTV_L3_OK ||
        dtv_l3_stream_destroy(stream) != DTV_L3_OK ||
        dtv_l3_lease_release(lease) != DTV_L3_OK) {
        return 23;
    }

    std::uint8_t out[4] = {1U, 2U, 3U, 4U};
    dtv_l3_transfer_request outbound{};
    outbound.kind = DTV_L3_TRANSFER_BULK;
    outbound.endpoint = 0x01U;
    outbound.timeout_ms = 100U;
    outbound.out_data = out;
    outbound.out_size = sizeof(out);
    dtv_l3_op *op = nullptr;
    if (dtv_l3_transfer_start(opened.owner, &outbound, &op) != DTV_L3_ACCEPTED) {
        return 9;
    }
    if (!wait_op(op)) {
        return 10;
    }
    dtv_l3_op_result result{};
    if (dtv_l3_op_poll(op, &result) != DTV_L3_OK ||
        result.result != DTV_L3_OK || io.bulk_calls != 2U ||
        io.last_out[0] != 1U || io.last_out[3] != 4U) {
        return 11;
    }
    dtv_l3_op_release(op);

    dtv_l3_transfer_request inbound{};
    inbound.kind = DTV_L3_TRANSFER_CONTROL;
    inbound.endpoint = 0U;
    inbound.timeout_ms = 100U;
    inbound.in_capacity = 4U;
    inbound.control.request_type = 0xC0U;
    inbound.control.request = 0x22U;
    inbound.control.value = 0x1234U;
    inbound.control.index = 0x5678U;
    if (dtv_l3_transfer_start(opened.owner, &inbound, &op) != DTV_L3_ACCEPTED) {
        return 12;
    }
    if (!wait_op(op)) {
        return 13;
    }
    if (dtv_l3_op_poll(op, &result) != DTV_L3_OK ||
        result.result != DTV_L3_OK || result.data_size != 4U ||
        result.data == nullptr || result.data[0] != 0x5AU ||
        io.request_type != 0xC0U || io.request != 0x22U ||
        io.value != 0x1234U || io.index != 0x5678U) {
        return 14;
    }
    dtv_l3_op_release(op);

    dtv_l3_transfer_request expired{};
    expired.kind = DTV_L3_TRANSFER_CONTROL;
    expired.endpoint = 0U;
    expired.timeout_ms = 100U;
    expired.in_capacity = 4U;
    expired.control.request_type = 0xC0U;
    expired.deadline_ns = 1U;
    io.expired = true;
    if (dtv_l3_transfer_start(opened.owner, &expired, &op) != DTV_L3_ACCEPTED) {
        return 15;
    }
    if (!wait_op(op)) {
        return 16;
    }
    if (dtv_l3_op_poll(op, &result) != DTV_L3_OK ||
        result.result != DTV_L3_TIMEOUT || io.control_calls != 1U) {
        return 17;
    }
    dtv_l3_op_release(op);

    dtv_l3_close(opened.owner, 1000U);
    if (dtv_l3_context_release(opened.owner) != DTV_L3_OK) {
        return 18;
    }
    pix_l3_factory_destroy(factory);
    pix_l3_dt295_io_backend_destroy(backend);
    return 0;
}
