/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "pix_l3_adapter.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static dtv_l3_claim_result fake_claim(void *context, uint32_t claim_index)
{
    (void)context;
    (void)claim_index;
    dtv_l3_claim_result result = {DTV_L3_OK, DTV_L3_CLAIM_RETAINED};
    return result;
}

static dtv_l3_release_result fake_release(void *context, uint32_t claim_index)
{
    (void)context;
    (void)claim_index;
    dtv_l3_release_result result = {DTV_L3_RELEASE_QUIESCED, DTV_L3_CLAIM_NONE,
                                    DTV_L3_RESTORE_NOT_ATTEMPTED, DTV_L3_OK};
    return result;
}

static dtv_l3_result fake_transfer(void *context, uint64_t transfer_id,
                                   const pix_l3_transfer *request,
                                   uint8_t *in_buffer, size_t in_capacity,
                                   pix_l3_transfer_result *completion)
{
    (void)context;
    (void)transfer_id;
    (void)in_buffer;
    (void)in_capacity;
    (void)request;
    (void)completion;
    return DTV_L3_UNSUPPORTED;
}

static dtv_l3_result fake_cancel(void *context, uint64_t transfer_id)
{
    (void)context;
    (void)transfer_id;
    return DTV_L3_UNSUPPORTED;
}

static dtv_l3_result fake_configure(void *context, uint32_t model,
                                    uint64_t selector, const uint8_t *firmware,
                                    size_t firmware_size)
{
    (void)context;
    (void)model;
    (void)selector;
    (void)firmware;
    (void)firmware_size;
    return DTV_L3_OK;
}

static dtv_l3_result fake_tune(void *context, dtv_l3_provider_binding *binding,
                               uint32_t receiver_id, uint64_t requested_hz,
                               uint64_t *programmed_hz)
{
    (void)context;
    (void)binding;
    (void)receiver_id;
    *programmed_hz = requested_hz;
    return DTV_L3_OK;
}

int main(void)
{
    pix_l3_factory_config config;
    memset(&config, 0, sizeof(config));
    config.model = PIX_L3_MODEL_DT295;
    config.usb.claim = fake_claim;
    config.usb.release_claim = fake_release;
    config.usb.transfer = fake_transfer;
    config.usb.cancel = fake_cancel;
    config.product.configure = fake_configure;
    config.product.tune = fake_tune;

    pix_l3_factory *factory = NULL;
    if (pix_l3_factory_create(&config, &factory) != DTV_L3_OK ||
        factory == NULL) {
        fprintf(stderr, "factory create failed\n");
        return 1;
    }

    dtv_l3_capabilities capabilities;
    if (pix_l3_factory_capabilities(factory, &capabilities) != DTV_L3_OK ||
        capabilities.receiver_count != 1U || capabilities.reader_count != 1U ||
        capabilities.lnb_power != 0U ||
        capabilities.cancel_scope != DTV_L3_CANCEL_SCOPE_TRANSFER) {
        fprintf(stderr, "common capability mismatch\n");
        return 1;
    }

    pix_l3_product_capabilities details;
    if (pix_l3_factory_details(factory, &details) != DTV_L3_OK ||
        details.model != PIX_L3_MODEL_DT295 ||
        details.firmware_required != 1U ||
        details.external_apdu != PIX_L3_UNSUPPORTED ||
        details.presence_observation != PIX_L3_SUPPORTED ||
        details.lnb_power != PIX_L3_UNSUPPORTED ||
        details.selects_satellite_stream_before_tune != 0U) {
        fprintf(stderr, "detail capability mismatch\n");
        return 1;
    }

    dtv_l3_usb_provider usb;
    dtv_l3_adapter_factory adapter_factory;
    if (pix_l3_factory_usb_provider(factory, &usb) != DTV_L3_OK ||
        usb.claim == NULL || usb.submit == NULL ||
        pix_l3_factory_adapter_factory(factory, &adapter_factory) !=
            DTV_L3_OK ||
        adapter_factory.create == NULL || adapter_factory.release == NULL) {
        fprintf(stderr, "provider or adapter factory build failed\n");
        return 1;
    }

    const uint8_t firmware[4] = {0x10, 0x20, 0x30, 0x40};
    const dtv_l3_firmware firmware_view = {
        .source = DTV_L3_FIRMWARE_BYTES,
        .path = NULL,
        .bytes = firmware,
        .size = sizeof(firmware),
        .read = NULL,
        .read_context = NULL};
    const dtv_l3_open_config open_config = {
        .model = PIX_L3_MODEL_DT295,
        .selector = 7U,
        .firmware = &firmware_view};

    dtv_l3_open_result opened =
        dtv_l3_open_factory(&usb, &adapter_factory, &open_config, 1U);
    if (opened.result != DTV_L3_ACCEPTED || opened.owner == NULL) {
        fprintf(stderr, "owner open failed\n");
        return 1;
    }
    if (dtv_l3_op_wait(opened.op, 1000U) != DTV_L3_OK) {
        fprintf(stderr, "open wait failed\n");
        return 1;
    }
    dtv_l3_op_result polled;
    memset(&polled, 0, sizeof(polled));
    if (dtv_l3_op_poll(opened.op, &polled) != DTV_L3_OK ||
        polled.state != DTV_L3_OP_DONE) {
        fprintf(stderr, "open poll failed\n");
        return 1;
    }
    if (dtv_l3_capabilities_get(opened.owner, &capabilities) != DTV_L3_OK ||
        capabilities.receiver_count != 1U) {
        fprintf(stderr, "owner capability mismatch\n");
        return 1;
    }
    dtv_l3_reader *reader = NULL;
    if (dtv_l3_reader_acquire(opened.owner, 0U, &reader) !=
        DTV_L3_UNSUPPORTED) {
        fprintf(stderr, "PIX external APDU must stay unsupported\n");
        return 1;
    }
    dtv_l3_lnb_state lnb_state;
    memset(&lnb_state, 0, sizeof(lnb_state));
    if (dtv_l3_lnb_status(opened.owner, 0U, &lnb_state) !=
        DTV_L3_UNSUPPORTED) {
        fprintf(stderr, "PIX LNB must stay unsupported\n");
        return 1;
    }
    dtv_l3_op_release(opened.op);
    dtv_l3_close(opened.owner, 1000U);
    if (dtv_l3_context_release(opened.owner) != DTV_L3_OK) {
        fprintf(stderr, "owner release failed\n");
        return 1;
    }
    pix_l3_factory_destroy(factory);
    return 0;
}
