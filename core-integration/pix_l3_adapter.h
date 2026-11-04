// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef PIX_L3_ADAPTER_H
#define PIX_L3_ADAPTER_H

#include <dtv/l3_owner.h>

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * PIX L3 product adapter.
 *
 * This is the product-owned side of the common contract in dtv/l3_owner.h.
 * The host supplies the USB-only dtv_l3_usb_provider. PIX supplies an
 * dtv_l3_product_adapter and an dtv_l3_adapter_factory whose create/release
 * own one context-local adapter. Product meaning is kept here: DT181/DT295
 * capability differences, firmware bundle handling, route/plan ownership and
 * close retention. The common public API is not extended.
 *
 * This target is optional and is not used by the existing CLI, daemon or
 * custom-FW paths.
 */

typedef enum pix_l3_model {
    PIX_L3_MODEL_DT181 = 181,
    PIX_L3_MODEL_DT295 = 295
} pix_l3_model;

typedef enum pix_l3_support {
    PIX_L3_SUPPORTED = 1,
    PIX_L3_UNSUPPORTED = 2,
    PIX_L3_UNKNOWN = 3
} pix_l3_support;

typedef struct pix_l3_transfer {
    uint32_t kind;
    uint32_t endpoint;
    uint32_t timeout_ms;
    const uint8_t *out_data;
    size_t out_size;
    size_t in_capacity;
    uint32_t claim_index;
    uint8_t request_type;
    uint8_t request;
    uint16_t value;
    uint16_t index;
    uint64_t deadline_ns;
} pix_l3_transfer;

typedef struct pix_l3_transfer_result {
    dtv_l3_result result;
    const uint8_t *data;
    size_t size;
    size_t actual_length;
    uint8_t actual_length_known;
} pix_l3_transfer_result;

/*
 * USB-only backend seam. The product already owns the USB session; claim
 * reports that the caller's single interface is held. release reports
 * quiescence after the product stop/shutdown path. A DT295 stop that returns
 * ABANDONED must report DTV_L3_RELEASE_QUIESCENCE_UNKNOWN with a retained
 * residual so the common owner keeps the cleanup obligation.
 */
typedef struct pix_l3_usb_backend_ops {
    void *context;
    dtv_l3_claim_result (*claim)(void *context, uint32_t claim_index);
    dtv_l3_release_result (*release_claim)(void *context, uint32_t claim_index);
    /*
     * For IN transfers write into in_buffer and point completion->data there.
     * The adapter keeps it alive through the terminal callback. For OUT
     * transfers copy request->out_data only.
     */
    dtv_l3_result (*transfer)(void *context, uint64_t transfer_id,
                              const pix_l3_transfer *request,
                              uint8_t *in_buffer, size_t in_capacity,
                              pix_l3_transfer_result *completion);
    dtv_l3_result (*cancel)(void *context, uint64_t transfer_id);
} pix_l3_usb_backend_ops;

/*
 * Product-owned semantics. configure receives the firmware bytes that the
 * adapter copied from the open config; it may retain another copy for the
 * product session. tune stays the route/plan-aware product entry point, so a
 * rejected route surfaces as a non-OK tune result.
 */
typedef struct pix_l3_product_ops {
    void *context;
    dtv_l3_result (*configure)(void *context, uint32_t model,
                               uint64_t selector, const uint8_t *firmware,
                               size_t firmware_size);
    dtv_l3_result (*tune)(void *context, dtv_l3_provider_binding *binding,
                          uint32_t receiver_id, uint64_t requested_hz,
                          uint64_t *programmed_hz);
    /* Optional; NULL defaults to one exclusive receiver 0 with bridge_id 0. */
    dtv_l3_result (*scope_query)(void *context, uint32_t receiver_id,
                                 dtv_l3_scope_info *scope);
} pix_l3_product_ops;

/*
 * One factory instance describes one product USB session. The caller keeps
 * the backend contexts alive for every open context and must not use the same
 * backend context for concurrent owners.
 */
typedef struct pix_l3_factory_config {
    pix_l3_model model;
    pix_l3_usb_backend_ops usb;
    pix_l3_product_ops product;
} pix_l3_factory_config;

/* PIX-specific details that the common capabilities struct cannot express. */
typedef struct pix_l3_product_capabilities {
    pix_l3_model model;
    uint8_t tuner_count;
    uint8_t reader_count;
    uint8_t lnb_power;
    uint8_t route_owned_by_product;
    uint8_t custom_firmware_candidate;
    uint8_t card_ts_concurrency;
    uint8_t fence_ts_effect;
    uint8_t firmware_required;
    uint8_t selects_satellite_stream_before_tune;
    uint8_t satellite_slot_count;
    uint8_t bridge_shared;
    uint8_t external_apdu;
    uint8_t presence_observation;
    uint8_t reserved[3];
} pix_l3_product_capabilities;

typedef struct pix_l3_factory pix_l3_factory;

dtv_l3_result pix_l3_factory_create(const pix_l3_factory_config *config,
                                    pix_l3_factory **factory_out);
void pix_l3_factory_destroy(pix_l3_factory *factory);
dtv_l3_result pix_l3_factory_capabilities(const pix_l3_factory *factory,
                                          dtv_l3_capabilities *capabilities);
dtv_l3_result pix_l3_factory_details(
    const pix_l3_factory *factory,
    pix_l3_product_capabilities *capabilities);
dtv_l3_result pix_l3_factory_usb_provider(pix_l3_factory *factory,
                                          dtv_l3_usb_provider *provider);
dtv_l3_result pix_l3_factory_adapter_factory(
    pix_l3_factory *factory, dtv_l3_adapter_factory *adapter_factory);

#ifdef __cplusplus
}
#endif

#endif
