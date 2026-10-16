/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Dave Hammer
 */
/*
 * Bridge application: Zigbee data model (heartbeat endpoint plus the endpoint pool), slot table
 * persistence, the Claim and SetSlot commands, arm monitoring, and routing a decoded Pico press to a
 * Zigbee command. The decisions themselves live in pico_logic.c. See docs/protocol.md.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint32_t claims;        /* Claim commands accepted */
    uint32_t ignored;       /* commands rejected for source address or manufacturer code */
    uint32_t setslot_ok;
    uint32_t setslot_bad;
    uint32_t sent;          /* Zigbee commands handed to the stack */
    uint32_t send_failed;
    uint32_t suppressed;    /* disarmed: would have sent */
    uint32_t unknown;       /* serial in no slot */
    uint32_t not_mapped;
} bridge_stats_t;

extern volatile bridge_stats_t g_bridge_stats;

/* Load the slot table from NVS and reset arm state (boot is always armed). After nvs_flash_init(). */
void bridge_init_state(void);

/* Register endpoint 1 (heartbeat) and the endpoint pool, plus the command handlers. Before esp_zigbee_start(). */
void bridge_register_data_model(void);

/* Start the worker (post-claim Stop) and the arm monitor. After the Zigbee stack is started. */
void bridge_start_tasks(void);

/* One-line summary: armed state, slots in use, counters. */
void bridge_status(char *buf, size_t cap);
