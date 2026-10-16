/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Dave Hammer
 */
#include "pico_logic.h"

uint8_t pico_button_to_mask(uint8_t b)
{
    switch (b) {
    case 0x02: return PICO_BTN_ON;
    case 0x03: return PICO_BTN_FAVORITE;
    case 0x04: return PICO_BTN_OFF;
    case 0x05: return PICO_BTN_RAISE;
    case 0x06: return PICO_BTN_LOWER;
    default:   return 0;
    }
}

int pico_slots_find(const pico_slots_t *t, uint32_t serial, uint8_t mask)
{
    if (serial == 0 || mask == 0) {
        return -1;
    }
    for (int i = 0; i < PICO_SLOT_COUNT; i++) {
        if (t->slot[i].serial == serial && (t->slot[i].mask & mask)) {
            return i;
        }
    }
    return -1;
}

bool pico_slots_has_serial(const pico_slots_t *t, uint32_t serial)
{
    if (serial == 0) {
        return false;
    }
    for (int i = 0; i < PICO_SLOT_COUNT; i++) {
        if (t->slot[i].serial == serial) {
            return true;
        }
    }
    return false;
}

bool pico_slots_set(pico_slots_t *t, unsigned idx, uint32_t serial, uint8_t mask)
{
    if (idx >= PICO_SLOT_COUNT) {
        return false;
    }
    if (serial == 0) {
        t->slot[idx].serial = 0;
        t->slot[idx].mask = 0;
        return true;
    }
    if (mask == 0 || (mask & ~PICO_BTN_ALL)) {
        return false;
    }
    for (unsigned i = 0; i < PICO_SLOT_COUNT; i++) {
        if (i != idx && t->slot[i].serial == serial && (t->slot[i].mask & mask)) {
            return false; /* two slots would both claim the same button */
        }
    }
    t->slot[idx].serial = serial;
    t->slot[idx].mask = mask;
    return true;
}

void pico_arm_init(pico_arm_t *a)
{
    for (int i = 0; i < PICO_SLOT_COUNT; i++) {
        a->deadline_ms[i] = 0;
    }
    a->claims = 0;
}

bool pico_arm_slot_armed(const pico_arm_t *a, unsigned slot, int64_t now_ms)
{
    if (slot >= PICO_SLOT_COUNT) {
        return true;
    }
    return a->deadline_ms[slot] == 0 || now_ms >= a->deadline_ms[slot];
}

uint32_t pico_arm_armed_mask(const pico_arm_t *a, int64_t now_ms)
{
    uint32_t m = 0;
    for (unsigned i = 0; i < PICO_SLOT_COUNT; i++) {
        if (pico_arm_slot_armed(a, i, now_ms)) {
            m |= 1u << i;
        }
    }
    return m;
}

bool pico_arm_armed(const pico_arm_t *a, int64_t now_ms)
{
    return pico_arm_armed_mask(a, now_ms) != 0;
}

void pico_arm_claim(pico_arm_t *a, int64_t now_ms, uint32_t handled)
{
    for (unsigned i = 0; i < PICO_SLOT_COUNT; i++) {
        if (handled & (1u << i)) {
            a->deadline_ms[i] = now_ms + PICO_CLAIM_WINDOW_MS;
        }
    }
    a->claims++;
}

int pico_moving_pop(uint32_t *moving, uint32_t only)
{
    for (int i = 0; i < PICO_SLOT_COUNT; i++) {
        if ((*moving & (1u << i)) && (only & (1u << i))) {
            *moving &= ~(1u << i);
            return i;
        }
    }
    return -1;
}

pico_decision_t pico_decide(const pico_slots_t *t, const pico_arm_t *arm, int64_t now_ms, uint32_t serial, uint8_t b,
                            bool press, uint32_t *moving)
{
    pico_decision_t d = {PICO_CMD_NONE, PICO_WHY_NOTHING, -1, 0};

    uint8_t mask = pico_button_to_mask(b);
    if (mask == 0) {
        d.why = PICO_WHY_LOG_ONLY;
        return d;
    }
    int slot = pico_slots_find(t, serial, mask);
    if (slot < 0) {
        d.why = pico_slots_has_serial(t, serial) ? PICO_WHY_NOT_MAPPED : PICO_WHY_UNKNOWN_SERIAL;
        return d;
    }
    d.slot = slot;
    d.endpoint = (uint8_t)(PICO_EP_BASE + slot);

    if (!press) {
        /* Only a running dim needs anything on release: stop it. */
        if ((mask == PICO_BTN_RAISE || mask == PICO_BTN_LOWER) && (*moving & (1u << slot))) {
            *moving &= ~(1u << slot);
            d.cmd = PICO_CMD_STOP;
            d.why = PICO_WHY_SEND;
        }
        return d;
    }

    switch (mask) {
    case PICO_BTN_ON:    d.cmd = PICO_CMD_ON; break;
    case PICO_BTN_OFF:   d.cmd = PICO_CMD_OFF; break;
    case PICO_BTN_RAISE: d.cmd = PICO_CMD_MOVE_UP; break;
    case PICO_BTN_LOWER: d.cmd = PICO_CMD_MOVE_DOWN; break;
    default:             d.why = PICO_WHY_LOG_ONLY; return d; /* favorite */
    }
    if (!pico_arm_slot_armed(arm, (unsigned)slot, now_ms)) {
        d.why = PICO_WHY_DISARMED;
        return d;
    }
    d.why = PICO_WHY_SEND;
    if (d.cmd == PICO_CMD_MOVE_UP || d.cmd == PICO_CMD_MOVE_DOWN) {
        *moving |= (1u << slot);
    }
    return d;
}

const char *pico_cmd_name(pico_cmd_t c)
{
    switch (c) {
    case PICO_CMD_ON:        return "on";
    case PICO_CMD_OFF:       return "off";
    case PICO_CMD_MOVE_UP:   return "move-up";
    case PICO_CMD_MOVE_DOWN: return "move-down";
    case PICO_CMD_STOP:      return "stop";
    default:                 return "none";
    }
}

const char *pico_why_name(pico_why_t w)
{
    switch (w) {
    case PICO_WHY_SEND:           return "send";
    case PICO_WHY_DISARMED:       return "would-have-sent";
    case PICO_WHY_UNKNOWN_SERIAL: return "unknown-serial";
    case PICO_WHY_NOT_MAPPED:     return "not-mapped";
    case PICO_WHY_LOG_ONLY:       return "log-only";
    default:                      return "nothing";
    }
}
