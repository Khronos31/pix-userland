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

static dtv_l3_result fake_tune(void *context, uint32_t receiver_id,
                               uint64_t requested_hz, uint64_t *programmed_hz)
{
    (void)context;
    (void)receiver_id;
    *programmed_hz = requested_hz;
    return DTV_L3_OK;
}

int main(void)
{
    pix_l3_factory_config config;
    memset(&config, 0, sizeof(config));
    config.model = PIX_L3_MODEL_DT295;
    config.backend.claim = fake_claim;
    config.backend.release_claim = fake_release;
    config.backend.transfer = fake_transfer;
    config.backend.cancel = fake_cancel;
    config.backend.tune = fake_tune;

    pix_l3_factory *factory = NULL;
    if (pix_l3_factory_create(&config, &factory) != DTV_L3_OK ||
        factory == NULL) {
        fprintf(stderr, "factory create failed\n");
        return 1;
    }

    pix_l3_capabilities capabilities;
    if (pix_l3_factory_capabilities(factory, &capabilities) != DTV_L3_OK) {
        fprintf(stderr, "capability read failed\n");
        return 1;
    }
    if (capabilities.model != PIX_L3_MODEL_DT295 ||
        capabilities.tuner_count != 1U ||
        capabilities.readers[0].external_apdu != PIX_L3_UNSUPPORTED ||
        capabilities.readers[0].presence_observation != PIX_L3_SUPPORTED ||
        capabilities.lnb_power != PIX_L3_UNSUPPORTED ||
        capabilities.tuners[0].selects_satellite_stream_before_tune != 0U) {
        fprintf(stderr, "capability mismatch\n");
        return 1;
    }

    dtv_l3_provider provider;
    if (pix_l3_factory_provider(factory, &provider) != DTV_L3_OK ||
        provider.claim == NULL || provider.submit == NULL) {
        fprintf(stderr, "provider build failed\n");
        return 1;
    }

    dtv_l3_open_result opened = dtv_l3_open(&provider, 1U);
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
    dtv_l3_op_release(opened.op);
    dtv_l3_close(opened.owner, 1000U);
    if (dtv_l3_context_release(opened.owner) != DTV_L3_OK) {
        fprintf(stderr, "owner release failed\n");
        return 1;
    }
    pix_l3_factory_destroy(factory);
    return 0;
}
