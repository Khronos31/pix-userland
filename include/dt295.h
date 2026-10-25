#ifndef PIXY_DT295_H
#define PIXY_DT295_H

#include "dt295_routes.h"
#include <stddef.h>
#include <stdint.h>

#define DT295_TS_PACKET_SIZE 188
#define DT295_ASSET_MAX_IMAGE (16u * 1024u * 1024u)
#define DT295_MAX_FIELDS 4096

typedef struct dt295_assets dt295_assets;
typedef struct dt295 dt295;

typedef struct {
    uint16_t address;
    uint8_t shift;
    uint8_t width;
    uint16_t value;
} dt295_field;

typedef struct {
    uint16_t selected_tsid;
    uint8_t locked;
    uint8_t agc_raw;
    uint16_t cn_raw;
} dt295_satellite_state;

typedef struct {
    unsigned raw_state;
    uint64_t notification_generation;
    uint64_t activation_generation;
    uint8_t activation_valid;
    uint8_t fresh_notification;
} dt295_card_snapshot;

typedef enum {
    DT295_CARD_RECOVERY_FATAL = -1,
    DT295_CARD_RECOVERY_NOT_NEEDED = 0,
    DT295_CARD_RECOVERY_ACTIVATED = 1,
    DT295_CARD_RECOVERY_PENDING = 2
} dt295_card_recovery_result;

/* USB operations return 0 on success. actual must equal the requested length
 * for control transfers and OUT transfers; a timed-out IN may return bytes. */
typedef struct {
    void *user;
    int (*control)(void *user, uint8_t request_type, uint8_t request, uint16_t value,
                   uint16_t index, uint8_t *data, size_t length, unsigned timeout_ms,
                   size_t *actual);
    int (*bulk)(void *user, uint8_t endpoint, uint8_t *data, size_t length, unsigned timeout_ms,
                size_t *actual);
    int (*interrupt)(void *user, uint8_t endpoint, uint8_t *data, size_t length,
                     unsigned timeout_ms, size_t *actual);
    uint64_t (*now_ms)(void *user);
    void (*sleep_ms)(void *user, unsigned ms);
} dt295_io;

/* Private runtime bundle produced by tools/prepare-dt295-assets.py. */
int dt295_assets_load(const char *path, dt295_assets **out, char *error, size_t error_size);
void dt295_assets_free(dt295_assets *assets);

int dt295_create(dt295 **out, const dt295_io *io, const dt295_assets *assets, char *error,
                 size_t error_size);
void dt295_destroy(dt295 *device);

/* Supported operation is target 0, macOS, terrestrial channels T13..T62. */
int dt295_boot(dt295 *device, char *error, size_t error_size);
int dt295_open_session(dt295 *device, char *error, size_t error_size);
int dt295_card_initialize(dt295 *device, char *error, size_t error_size);
int dt295_card_activate(dt295 *device, unsigned timeout_ms, char *error, size_t error_size);
int dt295_card_observe(dt295 *device, dt295_card_snapshot *snapshot);
int dt295_card_observe_until(dt295 *device, dt295_card_snapshot *snapshot,
                             uint64_t deadline_ms);
dt295_card_recovery_result dt295_card_recover(dt295 *device,
                                              const dt295_card_snapshot *observed,
                                              uint64_t deadline_ms,
                                              dt295_card_snapshot *result,
                                              char *error, size_t error_size);
void dt295_card_invalidate_observation(dt295 *device);
int dt295_tune_terrestrial(dt295 *device, unsigned channel, char *error, size_t error_size);
int dt295_terrestrial_rf_divider(unsigned channel, uint16_t *divider);

/* Satellite IF tuning requires a bundle rebuilt with the driver satellite
 * register table. Pass tsid < 0 to tune without selecting a TSID. */
int dt295_tune_satellite(dt295 *device, uint32_t if_hz, int tsid, char *error, size_t error_size);
int dt295_select_satellite_tsid(dt295 *device, uint16_t tsid, char *error, size_t error_size);
int dt295_satellite_tsid_table(dt295 *device, uint16_t tsids[8], char *error, size_t error_size);
int dt295_satellite_wait_lock(dt295 *device, unsigned timeout_ms, char *error, size_t error_size);
int dt295_select_satellite_slot(dt295 *device, unsigned slot, char *error, size_t error_size);
int dt295_sleep_satellite(dt295 *device, char *error, size_t error_size);
int dt295_satellite_status(dt295 *device, dt295_satellite_state *status, char *error,
                           size_t error_size);

/* Routes must already have passed dt295_routes validation. Fields are the
 * bounded, private TRC configuration supplied in the asset bundle. */
int dt295_prepare(dt295 *device, const dt295_route_op *routes, size_t route_count, char *error,
                  size_t error_size);
int dt295_prepare_plan(dt295 *device, const dt295_route_plan *plan, char *error, size_t error_size);
int dt295_reprepare_plan(dt295 *device, const dt295_route_plan *plan, char *error,
                         size_t error_size);
int dt295_route_write(dt295 *device, const dt295_route_op *op, char *error, size_t error_size);
int dt295_start(dt295 *device, char *error, size_t error_size);
int dt295_renew_if_due(dt295 *device, int *renewed, char *error, size_t error_size);
int dt295_stop(dt295 *device, size_t *drained_bytes, char *error, size_t error_size);
int dt295_shutdown(dt295 *device, char *error, size_t error_size);

/* Returns 1 when decrypted, 0 when clear, and a negative value on malformed
 * packets or unsupported protection. Input is unchanged on errors. */
int dt295_decode_packet(dt295 *device, uint8_t packet[DT295_TS_PACKET_SIZE]);

uint16_t dt295_state(dt295 *device);
int dt295_has_session(const dt295 *device);

#endif
