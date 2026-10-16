/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Dave Hammer
 */
/*
 * Host tests for pico_logic.c: slot table, arm state, press routing.
 *   make -C firmware/pico_bridge/test
 */
#include <stdio.h>
#include <string.h>

#include "../main/pico_logic.h"

static int s_fails;

#define CHECK(cond, ...)                                  \
    do {                                                  \
        if (cond) {                                       \
            printf("PASS ");                              \
        } else {                                          \
            printf("FAIL ");                              \
            s_fails++;                                    \
        }                                                 \
        printf(__VA_ARGS__);                              \
        printf("\n");                                     \
    } while (0)

#define DINING 0x0595a886u
#define LIVING 0x059582beu
#define SPARE 0x02dd0abfu

/* Arm states used by the routing tests: ARMED has never been claimed, DISARMED was claimed for every slot just before T. */
#define T 2000
static pico_arm_t ARMED, DISARMED;

static void setup_arm_states(void)
{
    pico_arm_init(&ARMED);
    pico_arm_init(&DISARMED);
    pico_arm_claim(&DISARMED, 1000, PICO_ALL_SLOTS);
}

/* Pico body[10] values */
#define B_ON 0x02
#define B_FAV 0x03
#define B_OFF 0x04
#define B_RAISE 0x05
#define B_LOWER 0x06

static void test_buttons(void)
{
    CHECK(pico_button_to_mask(B_ON) == PICO_BTN_ON && pico_button_to_mask(B_OFF) == PICO_BTN_OFF &&
              pico_button_to_mask(B_RAISE) == PICO_BTN_RAISE && pico_button_to_mask(B_LOWER) == PICO_BTN_LOWER &&
              pico_button_to_mask(B_FAV) == PICO_BTN_FAVORITE,
          "button bytes map to mask bits");
    CHECK(pico_button_to_mask(0x00) == 0 && pico_button_to_mask(0x07) == 0, "unknown button bytes map to 0");
}

static void test_slots(void)
{
    pico_slots_t t;
    memset(&t, 0, sizeof t);
    CHECK(pico_slots_find(&t, DINING, PICO_BTN_ON) == -1, "empty table finds nothing");

    CHECK(pico_slots_set(&t, 0, DINING, PICO_BTN_ALL), "set slot 0 for all buttons");
    CHECK(pico_slots_find(&t, DINING, PICO_BTN_RAISE) == 0, "default: every button of the Pico routes to slot 0");
    CHECK(pico_slots_find(&t, LIVING, PICO_BTN_ON) == -1, "another Pico does not match");

    CHECK(!pico_slots_set(&t, 1, DINING, PICO_BTN_ON), "second slot overlapping the same button is refused");
    CHECK(pico_slots_find(&t, DINING, PICO_BTN_ON) == 0, "refused set leaves the table unchanged");

    /* Split a Pico across two endpoints: shrink slot 0, give favorite to slot 1. */
    CHECK(pico_slots_set(&t, 0, DINING, PICO_BTN_ON | PICO_BTN_OFF | PICO_BTN_RAISE | PICO_BTN_LOWER), "shrink slot 0");
    CHECK(pico_slots_set(&t, 1, DINING, PICO_BTN_FAVORITE), "favorite takes slot 1");
    CHECK(pico_slots_find(&t, DINING, PICO_BTN_FAVORITE) == 1 && pico_slots_find(&t, DINING, PICO_BTN_ON) == 0,
          "split Pico: buttons route to different endpoints");

    CHECK(!pico_slots_set(&t, PICO_SLOT_COUNT, LIVING, PICO_BTN_ON), "index past the pool is refused");
    CHECK(!pico_slots_set(&t, 2, LIVING, 0), "empty mask for a real serial is refused");
    CHECK(!pico_slots_set(&t, 2, LIVING, 0x20), "mask bit outside the five buttons is refused");

    CHECK(pico_slots_set(&t, 1, 0, 0) && pico_slots_find(&t, DINING, PICO_BTN_FAVORITE) == -1, "serial 0 clears a slot");
    CHECK(pico_slots_set(&t, 0, DINING, PICO_BTN_ALL), "rewriting the same slot is not an overlap with itself");
    CHECK(pico_slots_has_serial(&t, DINING) && !pico_slots_has_serial(&t, LIVING) && !pico_slots_has_serial(&t, 0),
          "has_serial");
}

static void test_arm(void)
{
    pico_arm_t a;
    pico_arm_init(&a);
    CHECK(pico_arm_armed(&a, 0) && pico_arm_armed(&a, 1000000), "boot is armed, with no time limit");
    CHECK(pico_arm_armed_mask(&a, 5) == PICO_ALL_SLOTS, "boot: every slot is armed");
    pico_arm_claim(&a, 1000, PICO_ALL_SLOTS);
    CHECK(!pico_arm_armed(&a, 1000) && !pico_arm_armed(&a, 45999), "a claim for every slot disarms for 45 s");
    CHECK(pico_arm_armed(&a, 46000), "armed again exactly at the deadline");
    pico_arm_claim(&a, 20000, PICO_ALL_SLOTS);
    CHECK(!pico_arm_armed(&a, 64999) && pico_arm_armed(&a, 65000), "the next claim moves the deadline");
    CHECK(a.claims == 2, "claims counted");
    pico_arm_claim(&a, 0, PICO_ALL_SLOTS);
    CHECK(!pico_arm_armed(&a, 100), "a claim at t=0 still disarms (deadline is not the 'never claimed' value)");
}

static void test_arm_per_slot(void)
{
    pico_arm_t a;
    pico_arm_init(&a);
    const uint32_t all_but_6 = PICO_ALL_SLOTS & ~(1u << 6);

    pico_arm_claim(&a, 1000, all_but_6);
    CHECK(pico_arm_armed_mask(&a, 2000) == (1u << 6), "claiming every slot but 6 leaves only slot 6 armed");
    CHECK(pico_arm_slot_armed(&a, 6, 2000) && !pico_arm_slot_armed(&a, 3, 2000), "slot 6 armed, slot 3 disarmed");
    CHECK(pico_arm_armed(&a, 2000), "armed overall while any slot is armed");

    /* Slot 6 keeps being left out; the others keep being claimed. */
    pico_arm_claim(&a, 16000, all_but_6);
    pico_arm_claim(&a, 31000, all_but_6);
    CHECK(pico_arm_armed_mask(&a, 40000) == (1u << 6), "the left-out slot stays armed as the others are renewed");

    /* A slot claimed once, then left out, arms 45 s after its last claim, not before. */
    pico_arm_t b;
    pico_arm_init(&b);
    pico_arm_claim(&b, 1000, PICO_ALL_SLOTS);
    pico_arm_claim(&b, 16000, all_but_6);
    CHECK(!pico_arm_slot_armed(&b, 6, 45999), "slot 6 is still disarmed just before 45 s after its last claim");
    CHECK(pico_arm_slot_armed(&b, 6, 46000), "slot 6 arms 45 s after its last claim");
    CHECK(!pico_arm_slot_armed(&b, 3, 46000) && !pico_arm_slot_armed(&b, 3, 60999), "slot 3, renewed at 16 s, is still disarmed");
    CHECK(pico_arm_slot_armed(&b, 3, 61000), "slot 3 arms 45 s after its own last claim");

    /* A later full claim disarms slot 6 again. */
    pico_arm_claim(&b, 50000, PICO_ALL_SLOTS);
    CHECK(!pico_arm_slot_armed(&b, 6, 50001), "a claim that includes the slot disarms it again");

    /* Out-of-range slot queries read as armed, and a mask bit past the table is ignored. */
    CHECK(pico_arm_slot_armed(&b, PICO_SLOT_COUNT, 50001), "an out-of-range slot reads as armed");
    pico_arm_t c;
    pico_arm_init(&c);
    pico_arm_claim(&c, 1000, 0xFFFFFFFFu);
    CHECK(pico_arm_armed_mask(&c, 2000) == 0, "bits beyond the table are ignored");
}

static void test_decide_per_slot(void)
{
    pico_slots_t t;
    memset(&t, 0, sizeof t);
    pico_slots_set(&t, 3, DINING, PICO_BTN_ALL);
    pico_slots_set(&t, 6, LIVING, PICO_BTN_ALL);
    pico_arm_t a;
    pico_arm_init(&a);
    pico_arm_claim(&a, 1000, PICO_ALL_SLOTS & ~(1u << 6)); /* Home Assistant handles everything but slot 6 */
    uint32_t moving = 0;
    pico_decision_t d;

    d = pico_decide(&t, &a, 2000, DINING, B_ON, true, &moving);
    CHECK(d.why == PICO_WHY_DISARMED && d.slot == 3, "a handled Pico stays quiet");
    d = pico_decide(&t, &a, 2000, LIVING, B_ON, true, &moving);
    CHECK(d.why == PICO_WHY_SEND && d.cmd == PICO_CMD_ON && d.slot == 6 && d.endpoint == PICO_EP_BASE + 6,
          "the Pico whose slot was left out is served by the fallback");
    d = pico_decide(&t, &a, 2000, LIVING, B_RAISE, true, &moving);
    CHECK(d.why == PICO_WHY_SEND && (moving & (1u << 6)), "a dim on the armed slot starts and is tracked");

    /* The claim that finally includes slot 6 stops that dim, and only that dim. */
    moving |= 1u << 3;
    CHECK(pico_moving_pop(&moving, 1u << 6) == 6 && moving == (1u << 3), "popping for slot 6 leaves slot 3's dim alone");
    CHECK(pico_moving_pop(&moving, 1u << 6) == -1 && moving == (1u << 3), "nothing else to pop for that mask");
}

static void test_decide(void)
{
    pico_slots_t t;
    memset(&t, 0, sizeof t);
    pico_slots_set(&t, 3, DINING, PICO_BTN_ALL);
    pico_slots_set(&t, 5, LIVING, PICO_BTN_ON | PICO_BTN_OFF);
    pico_slots_set(&t, 6, LIVING, PICO_BTN_RAISE | PICO_BTN_LOWER);
    uint32_t moving = 0;
    pico_decision_t d;

    d = pico_decide(&t, &ARMED, T, DINING, B_ON, true, &moving);
    CHECK(d.why == PICO_WHY_SEND && d.cmd == PICO_CMD_ON && d.slot == 3 && d.endpoint == PICO_EP_BASE + 3,
          "armed on press: On from slot 3's endpoint");
    d = pico_decide(&t, &ARMED, T, DINING, B_OFF, true, &moving);
    CHECK(d.why == PICO_WHY_SEND && d.cmd == PICO_CMD_OFF, "armed off press: Off");

    d = pico_decide(&t, &DISARMED, T, DINING, B_ON, true, &moving);
    CHECK(d.why == PICO_WHY_DISARMED && d.cmd == PICO_CMD_ON, "disarmed: logs the command it would have sent");

    d = pico_decide(&t, &ARMED, T, 0x11223344u, B_ON, true, &moving);
    CHECK(d.why == PICO_WHY_UNKNOWN_SERIAL && d.cmd == PICO_CMD_NONE && d.slot == -1, "unknown serial");

    d = pico_decide(&t, &ARMED, T, LIVING, B_FAV, true, &moving);
    CHECK(d.why == PICO_WHY_NOT_MAPPED, "known Pico, button in no slot: not mapped");
    d = pico_decide(&t, &ARMED, T, DINING, B_FAV, true, &moving);
    CHECK(d.why == PICO_WHY_LOG_ONLY && d.cmd == PICO_CMD_NONE, "favorite is log-only");
    d = pico_decide(&t, &ARMED, T, DINING, 0x09, true, &moving);
    CHECK(d.why == PICO_WHY_LOG_ONLY, "unknown button byte is log-only");

    /* Releases of on/off do nothing. */
    d = pico_decide(&t, &ARMED, T, DINING, B_ON, false, &moving);
    CHECK(d.why == PICO_WHY_NOTHING && d.cmd == PICO_CMD_NONE, "release of on sends nothing");

    /* A split Pico routes buttons to their own endpoints. */
    d = pico_decide(&t, &ARMED, T, LIVING, B_ON, true, &moving);
    CHECK(d.slot == 5 && d.endpoint == PICO_EP_BASE + 5, "split: on goes to slot 5");
    d = pico_decide(&t, &ARMED, T, LIVING, B_RAISE, true, &moving);
    CHECK(d.slot == 6 && d.cmd == PICO_CMD_MOVE_UP && (moving & (1u << 6)), "split: raise goes to slot 6, dim marked running");

    /* Dim: Move on press, Stop on release, Stop only once. */
    d = pico_decide(&t, &ARMED, T, LIVING, B_RAISE, false, &moving);
    CHECK(d.why == PICO_WHY_SEND && d.cmd == PICO_CMD_STOP && d.slot == 6 && moving == 0, "release of raise: Stop, bit cleared");
    d = pico_decide(&t, &ARMED, T, LIVING, B_RAISE, false, &moving);
    CHECK(d.why == PICO_WHY_NOTHING, "a second release does not Stop again");

    /* Disarmed press of a dim button must not mark it running. */
    d = pico_decide(&t, &DISARMED, T, DINING, B_LOWER, true, &moving);
    CHECK(d.why == PICO_WHY_DISARMED && d.cmd == PICO_CMD_MOVE_DOWN && moving == 0, "disarmed lower: no Move, no running bit");
    d = pico_decide(&t, &DISARMED, T, DINING, B_LOWER, false, &moving);
    CHECK(d.why == PICO_WHY_NOTHING, "so its release sends nothing either");

    /* A dim started while armed is still stopped on release after a claim disarms the bridge. */
    d = pico_decide(&t, &ARMED, T, DINING, B_LOWER, true, &moving);
    CHECK(d.why == PICO_WHY_SEND && (moving & (1u << 3)), "armed lower starts a dim");
    d = pico_decide(&t, &DISARMED, T, DINING, B_LOWER, false, &moving);
    CHECK(d.why == PICO_WHY_SEND && d.cmd == PICO_CMD_STOP, "release while disarmed still stops a running dim");

    /* Two dims at once (two Picos) are tracked independently. */
    moving = 0;
    pico_decide(&t, &ARMED, T, DINING, B_RAISE, true, &moving);
    pico_decide(&t, &ARMED, T, LIVING, B_LOWER, true, &moving);
    CHECK(moving == ((1u << 3) | (1u << 6)), "dining and living dims both tracked");
    d = pico_decide(&t, &ARMED, T, DINING, B_RAISE, false, &moving);
    CHECK(d.cmd == PICO_CMD_STOP && moving == (1u << 6), "stopping dining does not clear living's bit");
    CHECK(pico_moving_pop(&moving, PICO_ALL_SLOTS) == 6 && moving == 0 && pico_moving_pop(&moving, PICO_ALL_SLOTS) == -1, "moving_pop drains the bits");
}

int main(void)
{
    test_buttons();
    test_slots();
    setup_arm_states();
    test_arm();
    test_arm_per_slot();
    test_decide();
    test_decide_per_slot();
    if (s_fails) {
        printf("LOGIC TESTS FAILED (%d)\n", s_fails);
        return 1;
    }
    printf("LOGIC TESTS PASSED\n");
    return 0;
}
