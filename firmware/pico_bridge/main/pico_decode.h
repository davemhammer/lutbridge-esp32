/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Dave Hammer
 */
/*
 * Lutron Pico (Clear Connect) receive decoder. A port of the decode side of
 * lutron_pico.py, the reference Python decoder (the oracle): crc16, packet_len,
 * action_of, serial_of, button_of, is_set_level, level_word/level_text/fade_ms,
 * read_8n1, decode_bits, _same_press, Grouper, event_action, format_event.
 *
 * Portable C99. No ESP-IDF dependency, so the same file builds on the host for
 * tests (firmware/pico_bridge/test) and into the firmware.
 *
 * Receive only. There is no frame builder here.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PICO_BODY_MAX 0x35 /* pairing packet length, the longest */
#define PICO_GROUP_GAP_US 200000LL   /* GROUP_GAP_S = 0.20 */
#define PICO_HOLD_US 2000000LL       /* HOLD_S = 2.0 */
#define PICO_SEQ_KEEP 8

typedef struct {
    uint8_t body[PICO_BODY_MAX]; /* bytes after FA DE: body + CRC-16 */
    uint8_t len;
    bool crc_ok;
    int64_t t_us; /* monotonic time of the burst, set by the caller */
} pico_frame_t;

typedef enum {
    PICO_ACT_PRESS,
    PICO_ACT_RELEASE,
    PICO_ACT_PAIRING,
    PICO_ACT_OTHER,
} pico_action_t;

/* crc16 over the body without the two CRC bytes (POLY 0xCA0F, custom update). */
uint16_t pico_crc16(const uint8_t *body, size_t n);
size_t pico_packet_len(uint8_t type_byte);
pico_action_t pico_action_of(uint8_t type_byte);

bool pico_is_set_level(const uint8_t *body, size_t n);
uint16_t pico_level_word(const uint8_t *body);
unsigned pico_fade_ms(const uint8_t *body);

/* serial_of: 8 lowercase hex chars plus NUL, or "-" when the body is too short.
 * For a set-level frame this is the target dimmer (bytes 9-12). */
void pico_serial_str(const uint8_t *body, size_t n, char out[9]);
uint32_t pico_serial_u32(const uint8_t *body, size_t n);

/* button_of: "on", "off", "favorite", "raise", "lower", "0xNN", "-" or a level percent. */
void pico_button_str(const uint8_t *body, size_t n, char *out, size_t cap);

/* Find one FA DE packet in a burst given as bits (one per byte, 0 or 1).
 * Tries both polarities and prefers a CRC hit. Returns true if a CRC-good frame
 * was found. With keep_fail, a CRC-bad candidate is returned (crc_ok false)
 * when no CRC-good one exists, and the return value is still false. */
bool pico_decode_bits(const uint8_t *bits, size_t nbits, bool keep_fail, pico_frame_t *out);

/* ---- grouping: one press is several bursts ------------------------------- */

typedef struct {
    pico_frame_t first;
    pico_frame_t last;
    unsigned count;
    uint8_t seqs[PICO_SEQ_KEEP]; /* first PICO_SEQ_KEEP sequence bytes */
    uint8_t last_seq;
    bool all_crc_ok;
} pico_event_t;

typedef struct {
    bool open;
    pico_event_t ev;
} pico_grouper_t;

typedef struct {
    bool new_group; /* this frame started a group: fire the Zigbee send now */
    bool closed;    /* the previous group ended, see the event passed in */
} pico_add_result_t;

void pico_grouper_init(pico_grouper_t *g);
pico_add_result_t pico_grouper_add(pico_grouper_t *g, const pico_frame_t *fr, pico_event_t *closed_out);
/* Close the open group once the air has been quiet past the gap. */
bool pico_grouper_expire(pico_grouper_t *g, int64_t now_us, pico_event_t *closed_out);
bool pico_grouper_flush(pico_grouper_t *g, pico_event_t *closed_out);

/* "press", "release", "hold", "level", "pairing" or "other". */
const char *pico_event_action(const pico_event_t *e);

/* One line, same text as the oracle's format_event in file mode (t=...s). */
void pico_format_event(char *buf, size_t cap, const pico_event_t *e);
