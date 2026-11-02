/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef PIX_DT295_SAVED_CS_FIXTURE_H
#define PIX_DT295_SAVED_CS_FIXTURE_H

#include <stdint.h>

#define DT295_SAVED_CS08 0
#define DT295_SAVED_CS22 1
#define DT295_SAVED_CS_PACKET_COUNT 6u

#ifdef __cplusplus
extern "C" {
#endif

uint16_t dt295_saved_cs_target(int mux);
uint16_t dt295_saved_cs_program(int mux, unsigned pmt_index);
int dt295_saved_cs_packet(int mux, unsigned index, uint8_t packet[188]);
int dt295_saved_cs_primary_ca_packet(int mux, uint8_t packet[188]);
int dt295_saved_cs_pmt_ecm_update_packet(int mux, uint8_t packet[188]);

#ifdef __cplusplus
}
#endif

#endif
