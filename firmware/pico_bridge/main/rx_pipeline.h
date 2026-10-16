/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Dave Hammer
 */
/*
 * Receive pipeline: CC1101 carrier sense (GDO2) arms an RMT capture on GDO0,
 * the capture is turned into bits and handed to the ported Pico decoder.
 * Receive only. Nothing here transmits.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint32_t glitches;    /* carrier-sense rises that did not last (noise) */
    uint32_t bursts;      /* carrier-sense rises that did */
    uint32_t frames_ok;   /* CRC-good Pico/bridge frames (pico_frames_crc_ok) */
    uint32_t frames_bad;  /* candidate found but CRC failed (pico_frames_crc_fail) */
    uint32_t no_frame;    /* burst with no FA DE packet */
    uint32_t events;      /* groups started (pico_events) */
    uint32_t arm_fail;
    uint32_t rmt_timeout;
} rx_stats_t;

extern volatile rx_stats_t g_rx_stats;

/* Called from the RX task once per press or release group, on the first CRC-good
 * frame (not on the repeats). Pico frames only: set-level, pairing and beacon frames
 * never reach it. button_byte is body[10] (on 0x02, favorite 0x03, off 0x04, raise 0x05,
 * lower 0x06). Implemented by the application. */
void pico_on_event(uint32_t serial, uint8_t button_byte, bool press);
void rx_pipeline_start(void);
