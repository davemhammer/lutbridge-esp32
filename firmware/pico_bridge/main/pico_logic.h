/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Dave Hammer
 */
/*
 * Slot table, arm state and press routing. Portable C99 with no ESP-IDF dependency, so the same
 * code is unit-tested on the host (firmware/pico_bridge/test/logic_test.c) and linked into the firmware.
 * See docs/protocol.md.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PICO_SLOT_COUNT 24 /* endpoint pool size: endpoints PICO_EP_BASE .. PICO_EP_BASE + 23 */
#define PICO_EP_BASE 10
#define PICO_CLAIM_WINDOW_MS 45000 /* three missed 15 s heartbeats */

/* Button masks used in the slot table. */
#define PICO_BTN_ON 0x01
#define PICO_BTN_OFF 0x02
#define PICO_BTN_RAISE 0x04
#define PICO_BTN_LOWER 0x08
#define PICO_BTN_FAVORITE 0x10
#define PICO_BTN_ALL 0x1F

typedef struct {
    uint32_t serial; /* 0 = empty */
    uint8_t mask;
} pico_slot_t;

typedef struct {
    pico_slot_t slot[PICO_SLOT_COUNT];
} pico_slots_t;

/* body[10] of a Pico frame (on 0x02, favorite 0x03, off 0x04, raise 0x05, lower 0x06) to a mask bit; 0 if unknown. */
uint8_t pico_button_to_mask(uint8_t body_button_byte);

/* Index of the first slot whose serial matches and whose mask contains `mask`, or -1. */
int pico_slots_find(const pico_slots_t *t, uint32_t serial, uint8_t mask);
bool pico_slots_has_serial(const pico_slots_t *t, uint32_t serial);

/* Write one slot. serial 0 clears it. Returns false (table unchanged) if the index is out of range,
 * the mask has bits outside PICO_BTN_ALL, a non-empty slot has an empty mask, or another slot with
 * the same serial already claims one of these buttons (ambiguous routing). */
bool pico_slots_set(pico_slots_t *t, unsigned idx, uint32_t serial, uint8_t mask);

/* ---- arm state, per slot ----------------------------------------------------
 * Boot is armed. A claim disarms the slots named in its `handled` bitmask (bit i = slot i) for
 * PICO_CLAIM_WINDOW_MS; slots it leaves out keep whatever deadline they already had, so a slot whose
 * source Home Assistant stops claiming arms on its own 45 s after its last claim while the others
 * stay disarmed. A claim with no mask (the original 1-byte form) handles every slot.
 */

#define PICO_ALL_SLOTS ((1u << PICO_SLOT_COUNT) - 1u)

typedef struct {
    int64_t deadline_ms[PICO_SLOT_COUNT]; /* 0 = never claimed, so armed */
    uint32_t claims;
} pico_arm_t;

void pico_arm_init(pico_arm_t *a);
bool pico_arm_slot_armed(const pico_arm_t *a, unsigned slot, int64_t now_ms);
/* Bitmask of slots that are armed now (bit i = slot i), over all PICO_SLOT_COUNT slots. */
uint32_t pico_arm_armed_mask(const pico_arm_t *a, int64_t now_ms);
/* True if any slot is armed. */
bool pico_arm_armed(const pico_arm_t *a, int64_t now_ms);
void pico_arm_claim(pico_arm_t *a, int64_t now_ms, uint32_t handled);

/* ---- routing -------------------------------------------------------------- */

typedef enum {
    PICO_CMD_NONE,
    PICO_CMD_ON,
    PICO_CMD_OFF,
    PICO_CMD_MOVE_UP,
    PICO_CMD_MOVE_DOWN,
    PICO_CMD_STOP,
} pico_cmd_t;

typedef enum {
    PICO_WHY_SEND,           /* send `cmd` from `endpoint` */
    PICO_WHY_DISARMED,       /* would have sent `cmd` (Home Assistant is healthy) */
    PICO_WHY_UNKNOWN_SERIAL, /* serial is in no slot */
    PICO_WHY_NOT_MAPPED,     /* serial has slots, but none claims this button */
    PICO_WHY_LOG_ONLY,       /* favorite, or a button we do not act on */
    PICO_WHY_NOTHING,        /* release of on/off, or a release with no dim running */
} pico_why_t;

typedef struct {
    pico_cmd_t cmd;
    pico_why_t why;
    int slot;         /* -1 if none */
    uint8_t endpoint; /* PICO_EP_BASE + slot, valid when slot >= 0 */
} pico_decision_t;

/*
 * Decide what a press or release does. The slot found for the press must be armed to send. `moving` is a bitmask
 * of slots with a dim in progress; it is updated here (set on a sent Move, cleared on Stop). A Stop for a dim already running is sent even
 * when disarmed, because leaving a bulb dimming is worse than one extra Stop.
 */
pico_decision_t pico_decide(const pico_slots_t *t, const pico_arm_t *arm, int64_t now_ms, uint32_t serial,
                            uint8_t body_button_byte, bool press, uint32_t *moving);

/* Pop one slot with a dim in progress whose bit is also in `only` (clearing its bit), or -1. Used when a claim
 * arrives, with the slots that claim handles. */
int pico_moving_pop(uint32_t *moving, uint32_t only);

const char *pico_cmd_name(pico_cmd_t c);
const char *pico_why_name(pico_why_t w);
