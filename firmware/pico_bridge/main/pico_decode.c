/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Dave Hammer
 */
#include "pico_decode.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define POLY 0xCA0F
#define LEVEL_FULL 0xFEFF
#define READ_MAX 96 /* longest run of 8N1 bytes we keep per offset */

/* ---- CRC ------------------------------------------------------------------ */

static uint16_t s_table[256];
static bool s_table_ready;

static void crc_table_init(void)
{
    for (int j = 0; j < 256; j++) {
        uint16_t crc = (uint16_t)(j << 8);
        for (int i = 0; i < 8; i++) {
            crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ POLY) : (uint16_t)(crc << 1);
        }
        s_table[j] = crc;
    }
    s_table_ready = true;
}

uint16_t pico_crc16(const uint8_t *body, size_t n)
{
    if (!s_table_ready) {
        crc_table_init();
    }
    uint16_t crc = 0;
    for (size_t i = 0; i < n; i++) {
        uint8_t upper = (uint8_t)(crc >> 8);
        crc = (uint16_t)((((crc << 8) & 0xFF00) + body[i]) ^ s_table[upper]);
    }
    return crc;
}

/* ---- field helpers -------------------------------------------------------- */

size_t pico_packet_len(uint8_t t)
{
    if ((t & 0xC0) == 0) {
        return 5;
    }
    if ((t & 0xE0) == 0xA0) {
        return 0x35;
    }
    return 0x18;
}

pico_action_t pico_action_of(uint8_t t)
{
    if (t == 0x88 || t == 0x8A) {
        return PICO_ACT_PRESS;
    }
    if (t == 0x89 || t == 0x8B) {
        return PICO_ACT_RELEASE;
    }
    if ((t & 0xE0) == 0xA0) {
        return PICO_ACT_PAIRING;
    }
    return PICO_ACT_OTHER;
}

bool pico_is_set_level(const uint8_t *b, size_t n)
{
    if (n < 20) {
        return false;
    }
    if ((b[0] & 0xF0) != 0x80 || b[0] == 0x88 || b[0] == 0x89 || b[0] == 0x8A || b[0] == 0x8B) {
        return false;
    }
    static const uint8_t k_a[3] = {0x21, 0x0E, 0x00};
    static const uint8_t k_b[2] = {0x40, 0x02};
    return memcmp(b + 6, k_a, 3) == 0 && b[13] == 0xFE && memcmp(b + 14, k_b, 2) == 0;
}

uint16_t pico_level_word(const uint8_t *b)
{
    return (uint16_t)((b[16] << 8) | b[17]);
}

unsigned pico_fade_ms(const uint8_t *b)
{
    return (unsigned)b[19] * 250U;
}

static void level_text(uint16_t word, char *out, size_t cap)
{
    double pct = word * 100.0 / LEVEL_FULL;
    double nearest = round(pct);
    if (fabs(pct - nearest) < 0.05) {
        snprintf(out, cap, "%.0f%%", nearest);
    } else {
        snprintf(out, cap, "%.1f%%", pct);
    }
}

static const uint8_t *serial_ptr(const uint8_t *b, size_t n)
{
    if (pico_is_set_level(b, n)) {
        return b + 9;
    }
    if (n < 6) {
        return NULL;
    }
    return b + 2;
}

void pico_serial_str(const uint8_t *b, size_t n, char out[9])
{
    const uint8_t *p = serial_ptr(b, n);
    if (!p) {
        strcpy(out, "-");
        return;
    }
    snprintf(out, 9, "%02x%02x%02x%02x", p[0], p[1], p[2], p[3]);
}

uint32_t pico_serial_u32(const uint8_t *b, size_t n)
{
    const uint8_t *p = serial_ptr(b, n);
    if (!p) {
        return 0;
    }
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

void pico_button_str(const uint8_t *b, size_t n, char *out, size_t cap)
{
    if (pico_is_set_level(b, n)) {
        level_text(pico_level_word(b), out, cap);
        return;
    }
    if (n < 11 || (b[0] != 0x88 && b[0] != 0x89 && b[0] != 0x8A && b[0] != 0x8B)) {
        snprintf(out, cap, "-");
        return;
    }
    switch (b[10]) {
    case 0x02: snprintf(out, cap, "on"); break;
    case 0x03: snprintf(out, cap, "favorite"); break;
    case 0x04: snprintf(out, cap, "off"); break;
    case 0x05: snprintf(out, cap, "raise"); break;
    case 0x06: snprintf(out, cap, "lower"); break;
    default:   snprintf(out, cap, "0x%02x", b[10]); break;
    }
}

/* ---- bit level decode ----------------------------------------------------- */

/* read_8n1: start 0, eight data bits LSB first, stop 1. Stops at the first bad frame. */
static size_t read_8n1(const uint8_t *bits, size_t n, size_t off, bool invert, uint8_t *out, size_t cap)
{
    size_t count = 0;
    size_t i = off;
    while (i + 10 <= n && count < cap) {
        uint8_t start = bits[i] ^ (uint8_t)invert;
        uint8_t stop = bits[i + 9] ^ (uint8_t)invert;
        if (start != 0 || stop != 1) {
            break;
        }
        uint8_t v = 0;
        for (int j = 0; j < 8; j++) {
            v |= (uint8_t)(((bits[i + 1 + j] ^ (uint8_t)invert) & 1) << j);
        }
        out[count++] = v;
        i += 10;
    }
    return count;
}

bool pico_decode_bits(const uint8_t *bits, size_t nbits, bool keep_fail, pico_frame_t *out)
{
    bool have_best = false;
    pico_frame_t best;
    memset(&best, 0, sizeof best);

    for (int pol = 0; pol < 2; pol++) {
        long limit = (long)nbits - 80;
        if (limit > 240) {
            limit = 240;
        }
        for (long off = 0; off < limit; off++) {
            uint8_t data[READ_MAX];
            size_t nd = read_8n1(bits, nbits, (size_t)off, pol == 1, data, sizeof data);
            for (size_t k = 0; k + 3 < nd; k++) {
                if (data[k] != 0xFA || data[k + 1] != 0xDE) {
                    continue;
                }
                const uint8_t *pkt = data + k + 2;
                size_t plen = nd - (k + 2);
                if (plen == 0) {
                    continue;
                }
                size_t n = pico_packet_len(pkt[0]);
                if (plen < n || n < 4) {
                    continue;
                }
                bool ok = pico_crc16(pkt, n - 2) == (uint16_t)((pkt[n - 2] << 8) | pkt[n - 1]);
                if (ok) {
                    memset(out, 0, sizeof *out);
                    memcpy(out->body, pkt, n);
                    out->len = (uint8_t)n;
                    out->crc_ok = true;
                    return true;
                }
                if (keep_fail && (!have_best || n > best.len)) {
                    memset(&best, 0, sizeof best);
                    memcpy(best.body, pkt, n);
                    best.len = (uint8_t)n;
                    best.crc_ok = false;
                    have_best = true;
                }
                break;
            }
        }
    }
    if (have_best) {
        *out = best;
    }
    return false;
}

/* ---- grouping ------------------------------------------------------------- */

static bool same_press(const pico_frame_t *prev, const pico_frame_t *fr)
{
    if (fr->t_us - prev->t_us > PICO_GROUP_GAP_US || fr->t_us < prev->t_us) {
        return false;
    }
    if (prev->len < 11 || fr->len < 11) {
        return false;
    }
    char sa[9], sb[9];
    pico_serial_str(prev->body, prev->len, sa);
    pico_serial_str(fr->body, fr->len, sb);
    if (strcmp(sa, sb) != 0) {
        return false;
    }
    if (prev->body[0] != fr->body[0]) {
        return false;
    }
    bool la = pico_is_set_level(prev->body, prev->len);
    bool lb = pico_is_set_level(fr->body, fr->len);
    if (la || lb) {
        return la && lb && pico_level_word(prev->body) == pico_level_word(fr->body);
    }
    return prev->body[10] == fr->body[10];
}

static void event_start(pico_event_t *e, const pico_frame_t *fr)
{
    memset(e, 0, sizeof *e);
    e->first = *fr;
    e->last = *fr;
    e->count = 1;
    e->seqs[0] = fr->len > 1 ? fr->body[1] : 0;
    e->last_seq = e->seqs[0];
    e->all_crc_ok = fr->crc_ok;
}

static void event_append(pico_event_t *e, const pico_frame_t *fr)
{
    uint8_t seq = fr->len > 1 ? fr->body[1] : 0;
    if (e->count < PICO_SEQ_KEEP) {
        e->seqs[e->count] = seq;
    }
    e->count++;
    e->last = *fr;
    e->last_seq = seq;
    e->all_crc_ok = e->all_crc_ok && fr->crc_ok;
}

void pico_grouper_init(pico_grouper_t *g)
{
    memset(g, 0, sizeof *g);
}

pico_add_result_t pico_grouper_add(pico_grouper_t *g, const pico_frame_t *fr, pico_event_t *closed_out)
{
    pico_add_result_t r = {false, false};
    if (g->open && !same_press(&g->ev.last, fr)) {
        if (closed_out) {
            *closed_out = g->ev;
        }
        r.closed = true;
        event_start(&g->ev, fr);
        r.new_group = true;
        return r;
    }
    if (!g->open) {
        g->open = true;
        event_start(&g->ev, fr);
        r.new_group = true;
        return r;
    }
    event_append(&g->ev, fr);
    return r;
}

bool pico_grouper_flush(pico_grouper_t *g, pico_event_t *closed_out)
{
    if (!g->open) {
        return false;
    }
    if (closed_out) {
        *closed_out = g->ev;
    }
    g->open = false;
    return true;
}

bool pico_grouper_expire(pico_grouper_t *g, int64_t now_us, pico_event_t *closed_out)
{
    if (g->open && now_us - g->ev.last.t_us > PICO_GROUP_GAP_US) {
        return pico_grouper_flush(g, closed_out);
    }
    return false;
}

const char *pico_event_action(const pico_event_t *e)
{
    const pico_frame_t *f = &e->first;
    if (pico_is_set_level(f->body, f->len)) {
        return "level";
    }
    pico_action_t a = pico_action_of(f->body[0]);
    if (a == PICO_ACT_PRESS && e->last.t_us - f->t_us >= PICO_HOLD_US) {
        return "hold";
    }
    switch (a) {
    case PICO_ACT_PRESS:   return "press";
    case PICO_ACT_RELEASE: return "release";
    case PICO_ACT_PAIRING: return "pairing";
    default:               return "other";
    }
}

void pico_format_event(char *buf, size_t cap, const pico_event_t *e)
{
    const pico_frame_t *f = &e->first;
    char serial[9], button[16], seqs[PICO_SEQ_KEEP * 3 + 8];
    pico_serial_str(f->body, f->len, serial);
    pico_button_str(f->body, f->len, button, sizeof button);

    size_t shown = e->count < PICO_SEQ_KEEP ? e->count : PICO_SEQ_KEEP;
    int p = 0;
    for (size_t i = 0; i < shown; i++) {
        p += snprintf(seqs + p, sizeof seqs - (size_t)p, "%s%02x", i ? "," : "", e->seqs[i]);
    }
    if (e->count > PICO_SEQ_KEEP) {
        snprintf(seqs + p, sizeof seqs - (size_t)p, "..%02x", e->last_seq);
    }

    char level[40] = "";
    if (pico_is_set_level(f->body, f->len)) {
        snprintf(level, sizeof level, " level=%04x fade=%ums", pico_level_word(f->body), pico_fade_ms(f->body));
    }

    char hex[PICO_BODY_MAX * 3 + 1] = "";
    size_t o = 0;
    for (size_t i = 0; i < f->len; i++) {
        o += (size_t)snprintf(hex + o, sizeof hex - o, "%s%02x", i ? " " : "", f->body[i]);
    }

    snprintf(buf, cap, "t=%.3fs serial=%s button=%s action=%s seq=%02x seqs=%s repeats=%u crc=%s%s frame=%s",
             (double)f->t_us / 1e6, serial, button, pico_event_action(e), e->seqs[0], seqs, e->count,
             e->all_crc_ok ? "ok" : "FAIL", level, hex);
}
