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
 * PIX L3 product adapter skeleton.
 *
 * The common Core contract lives in dtv/l3_owner.h. This header keeps the PIX
 * product meaning (DT181/DT295 capability differences, route/plan ownership,
 * custom-firmware candidates and close retention) out of Core. It is an
 * optional target and is not used by the existing CLI, daemon or custom-FW
 * paths.
 *
 * A caller supplies a backend seam that already owns the product USB session.
 * The adapter never opens a device itself; it folds the existing DT181
 * usb_backend or DT295 dt295_io transfer path into the common provider
 * callbacks and reports the product capability differences as data.
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

/* Products keep route/plan, firmware bootstrap and decoded TS output. */
typedef struct pix_l3_tuner_capability {
    uint32_t receiver_id;
    uint8_t terrestrial;
    uint8_t satellite;
    uint8_t satellite_slot_count;
    /* DT181 starts the stream before tune; DT295 tunes first. */
    uint8_t selects_satellite_stream_before_tune;
    /* One physical tuner per PIX model; no shared multi-receiver bridge. */
    uint8_t bridge_shared;
    uint8_t reserved[3];
} pix_l3_tuner_capability;

/*
 * Both PIX models observe the internal card but expose no external APDU path.
 * DT181 reports presence through dt181_session status; DT295 through
 * dt295_card_observe. Decoded TS output is not evidence of external APDU.
 */
typedef struct pix_l3_reader_capability {
    uint32_t reader_id;
    uint8_t present;
    uint8_t presence_observation;
    uint8_t external_apdu;
    uint8_t protocol_t1;
    uint8_t reserved[4];
} pix_l3_reader_capability;

typedef enum pix_l3_card_ts_concurrency {
    PIX_L3_CARD_TS_UNVERIFIED = 0
} pix_l3_card_ts_concurrency;

typedef enum pix_l3_fence_ts_effect {
    PIX_L3_FENCE_TS_UNKNOWN = 0
} pix_l3_fence_ts_effect;

typedef struct pix_l3_capabilities {
    pix_l3_model model;
    uint8_t tuner_count;
    uint8_t reader_count;
    /* No LNB power scope on either model (DT295 hardware correction). */
    uint8_t lnb_power;
    /* Route/plan and PSI gating stay in the product backend, not L3. */
    uint8_t route_owned_by_product;
    /* Candidate custom firmware is not a 1.0 capability. */
    uint8_t custom_firmware_candidate;
    uint8_t card_ts_concurrency;
    uint8_t fence_ts_effect;
    uint8_t reserved[1];
    pix_l3_tuner_capability tuners[1];
    pix_l3_reader_capability readers[1];
} pix_l3_capabilities;

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
 * The product backend already owns the USB session. claim reports that the
 * caller's single interface is held; release reports quiescence after the
 * product stop/shutdown path. A DT295 stop that returns ABANDONED must report
 * DTV_L3_RELEASE_QUIESCENCE_UNKNOWN with a retained residual so the common
 * owner keeps the cleanup obligation instead of treating the stream as gone.
 */
typedef struct pix_l3_backend_ops {
    void *context;
    dtv_l3_claim_result (*claim)(void *context, uint32_t claim_index);
    dtv_l3_release_result (*release_claim)(void *context, uint32_t claim_index);
    /*
     * Must fill a terminal result. For IN transfers write into in_buffer and
     * point completion->data there; the adapter keeps it alive through the
     * terminal callback. For OUT transfers only copy request->out_data.
     */
    dtv_l3_result (*transfer)(void *context, uint64_t transfer_id,
                              const pix_l3_transfer *request,
                              uint8_t *in_buffer, size_t in_capacity,
                              pix_l3_transfer_result *completion);
    dtv_l3_result (*cancel)(void *context, uint64_t transfer_id);
    /* Route/plan rejection surfaces here as a non-OK tune result. */
    dtv_l3_result (*tune)(void *context, uint32_t receiver_id,
                          uint64_t requested_hz, uint64_t *programmed_hz);
} pix_l3_backend_ops;

typedef struct pix_l3_factory_config {
    pix_l3_model model;
    pix_l3_backend_ops backend;
} pix_l3_factory_config;

typedef struct pix_l3_factory pix_l3_factory;

dtv_l3_result pix_l3_factory_create(const pix_l3_factory_config *config,
                                    pix_l3_factory **factory_out);
void pix_l3_factory_destroy(pix_l3_factory *factory);
dtv_l3_result pix_l3_factory_capabilities(const pix_l3_factory *factory,
                                          pix_l3_capabilities *capabilities);
/*
 * The provider borrows the factory; keep the factory alive for the whole
 * dtv_l3_context lifetime and until context_release succeeds.
 */
dtv_l3_result pix_l3_factory_provider(pix_l3_factory *factory,
                                      dtv_l3_provider *provider);

#ifdef __cplusplus
}
#endif

#endif
