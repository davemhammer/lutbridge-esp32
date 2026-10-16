/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Dave Hammer
 */
#include "rx_pipeline.h"

#include <string.h>

#include "cc1101.h"
#include "driver/gpio.h"
#include "driver/rmt_rx.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "pico_decode.h"

static const char *TAG = "pico";

#define SYM_MAX 640        /* one burst is ~200 symbols; leave room for noise */
#define BIT_US 16          /* 62.5 kBaud */
#define RMT_IDLE_NS 500000 /* longest in-burst run is ~160 us; 0.5 ms of quiet ends the capture */
#define BURST_WINDOW_US 7000 /* a burst is ~5.7 ms (80 preamble bits + 27 bytes at 16 us/bit) */
#define CAL_US 1200          /* IDLE -> RX auto-calibration */
#define MAX_RUN_BITS 12    /* clamp the trailing idle run so it cannot flood the bit string */
#define BITS_MAX 2048
#define CS_QUALIFY_US 150  /* carrier sense must still be high this long after the rise */

volatile rx_stats_t g_rx_stats;

typedef struct {
    uint8_t level;
    int64_t t_us;
} cs_evt_t;

typedef struct {
    int buf;
    size_t n;
} done_evt_t;

static rmt_symbol_word_t s_sym[2][SYM_MAX];
static rmt_channel_handle_t s_rx;
static QueueHandle_t s_cs_q;
static QueueHandle_t s_done_q;
static SemaphoreHandle_t s_window_sem;
static esp_timer_handle_t s_window_timer;

static void window_timer_cb(void *arg)
{
    (void)arg;
    xSemaphoreGive(s_window_sem);
}

static void IRAM_ATTR cs_isr(void *arg)
{
    (void)arg;
    cs_evt_t e = {.level = (uint8_t)gpio_get_level(CC1101_PIN_GDO2), .t_us = esp_timer_get_time()};
    BaseType_t hp = pdFALSE;
    xQueueSendFromISR(s_cs_q, &e, &hp);
    if (hp) {
        portYIELD_FROM_ISR();
    }
}

static bool IRAM_ATTR on_recv_done(rmt_channel_handle_t ch, const rmt_rx_done_event_data_t *ed, void *ctx)
{
    (void)ch;
    (void)ctx;
    done_evt_t d = {.buf = (ed->received_symbols == s_sym[0]) ? 0 : 1, .n = ed->num_symbols};
    BaseType_t hp = pdFALSE;
    xQueueSendFromISR(s_done_q, &d, &hp);
    return hp == pdTRUE;
}

/* Edges to bits the way demod_bits does: each run is round(duration / 16 us) bits. */
static size_t symbols_to_bits(const rmt_symbol_word_t *s, size_t n, uint8_t *bits, size_t cap)
{
    size_t nb = 0;
    for (size_t i = 0; i < n; i++) {
        for (int half = 0; half < 2; half++) {
            unsigned level = half ? s[i].level1 : s[i].level0;
            unsigned dur = half ? s[i].duration1 : s[i].duration0;
            if (dur == 0) {
                return nb;
            }
            unsigned k = (dur + BIT_US / 2) / BIT_US;
            if (k > MAX_RUN_BITS) {
                k = MAX_RUN_BITS;
            }
            while (k-- && nb < cap) {
                bits[nb++] = (uint8_t)level;
            }
        }
    }
    return nb;
}

static void log_event(const pico_event_t *ev)
{
    char line[400];
    pico_format_event(line, sizeof line, ev);
    ESP_LOGI(TAG, "%s", line);
}

/* Per-burst diagnostics are debug level: a Pico held right next to the antenna loses the
 * first burst of every slot (receiver overload, see README), which would otherwise flood the log. */
static void process_capture(const done_evt_t *d, int64_t t_rise_us, pico_grouper_t *g)
{
    static uint8_t bits[BITS_MAX];
    size_t nb = symbols_to_bits(s_sym[d->buf], d->n, bits, sizeof bits);

    pico_frame_t fr;
    memset(&fr, 0, sizeof fr);
    bool ok = pico_decode_bits(bits, nb, true, &fr);
    if (ok) {
        g_rx_stats.frames_ok++;
        fr.t_us = t_rise_us;
        pico_event_t closed;
        pico_add_result_t r = pico_grouper_add(g, &fr, &closed);
        if (r.closed) {
            log_event(&closed);
        }
        if (r.new_group) {
            g_rx_stats.events++;
            char serial[9], button[16];
            pico_serial_str(fr.body, fr.len, serial);
            pico_button_str(fr.body, fr.len, button, sizeof button);
            ESP_LOGI(TAG, "start serial=%s button=%s type=%02x seq=%02x", serial, button, fr.body[0], fr.body[1]);

            pico_action_t act = pico_action_of(fr.body[0]);
            if (!pico_is_set_level(fr.body, fr.len) && (act == PICO_ACT_PRESS || act == PICO_ACT_RELEASE)) {
                pico_on_event(pico_serial_u32(fr.body, fr.len), fr.body[10], act == PICO_ACT_PRESS);
                ESP_LOGI(TAG, "event handled %lld us after the burst started",
                         (long long)(esp_timer_get_time() - t_rise_us));
            }
        }
        ESP_LOGD(TAG, "burst t=%.3f syms=%u bits=%u -> ok seq=%02x", (double)t_rise_us / 1e6, (unsigned)d->n,
                 (unsigned)nb, fr.body[1]);
    } else if (fr.len > 0) {
        g_rx_stats.frames_bad++;
        ESP_LOGD(TAG, "burst t=%.3f syms=%u bits=%u -> CRC fail type=%02x len=%u", (double)t_rise_us / 1e6,
                 (unsigned)d->n, (unsigned)nb, fr.body[0], fr.len);
    } else {
        g_rx_stats.no_frame++;
        ESP_LOGD(TAG, "burst t=%.3f syms=%u bits=%u -> no frame", (double)t_rise_us / 1e6, (unsigned)d->n,
                 (unsigned)nb);
    }
}

static void rx_task(void *arg)
{
    (void)arg;
    pico_grouper_t g;
    pico_grouper_init(&g);
    int cur = 0;

    for (;;) {
        cs_evt_t e;
        if (xQueueReceive(s_cs_q, &e, pdMS_TO_TICKS(100)) != pdTRUE) {
            pico_event_t ev;
            if (pico_grouper_expire(&g, esp_timer_get_time(), &ev)) {
                log_event(&ev);
            }
            continue;
        }
        if (e.level == 0) {
            continue; /* stray fall */
        }
        /* A real burst keeps carrier sense high for milliseconds. Noise glitches are
         * microseconds wide: do not arm the capture or idle the radio for those. */
        esp_rom_delay_us(CS_QUALIFY_US);
        if (gpio_get_level(CC1101_PIN_GDO2) == 0) {
            g_rx_stats.glitches++;
            continue;
        }

        g_rx_stats.bursts++;
        int64_t t_rise = e.t_us;
        rmt_receive_config_t rc = {.signal_range_min_ns = 1000, .signal_range_max_ns = RMT_IDLE_NS};
        if (rmt_receive(s_rx, s_sym[cur], sizeof s_sym[cur], &rc) != ESP_OK) {
            g_rx_stats.arm_fail++;
            continue;
        }

        /* Carrier sense chatters at the start of a burst and can drop mid-burst, so it is
         * not used to find the end. A Pico burst is a fixed ~5.7 ms: capture a fixed window
         * from the first qualified rise, then idle the radio so GDO0 goes quiet and the RMT
         * idle timeout closes the capture. */
        esp_timer_start_once(s_window_timer, BURST_WINDOW_US);
        if (xSemaphoreTake(s_window_sem, pdMS_TO_TICKS(30)) != pdTRUE) {
            esp_timer_stop(s_window_timer);
        }
        cc1101_strobe(CC1101_SIDLE);

        done_evt_t d;
        if (xQueueReceive(s_done_q, &d, pdMS_TO_TICKS(30)) != pdTRUE) {
            g_rx_stats.rmt_timeout++;
            rmt_disable(s_rx);
            rmt_enable(s_rx);
            cc1101_strobe(CC1101_SRX);
            esp_rom_delay_us(CAL_US);
            xQueueReset(s_cs_q);
            continue;
        }
        cc1101_strobe(CC1101_SRX);

        process_capture(&d, t_rise, &g);
        cur ^= 1;

        esp_rom_delay_us(CAL_US); /* let calibration finish, then forget its carrier-sense glitches */
        xQueueReset(s_cs_q);
    }
}

void rx_pipeline_start(void)
{
    s_cs_q = xQueueCreate(16, sizeof(cs_evt_t));
    s_done_q = xQueueCreate(4, sizeof(done_evt_t));
    s_window_sem = xSemaphoreCreateBinary();
    esp_timer_create_args_t targs = {.callback = window_timer_cb, .name = "pico_win"};
    ESP_ERROR_CHECK(esp_timer_create(&targs, &s_window_timer));

    rmt_rx_channel_config_t cfg = {
        .gpio_num = CC1101_PIN_GDO0,
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 1000000, /* 1 us per tick */
        .mem_block_symbols = 48,
    };
    ESP_ERROR_CHECK(rmt_new_rx_channel(&cfg, &s_rx));
    rmt_rx_event_callbacks_t cbs = {.on_recv_done = on_recv_done};
    ESP_ERROR_CHECK(rmt_rx_register_event_callbacks(s_rx, &cbs, NULL));
    ESP_ERROR_CHECK(rmt_enable(s_rx));

    gpio_config_t io = {
        .pin_bit_mask = 1ULL << CC1101_PIN_GDO2,
        .mode = GPIO_MODE_INPUT,
        .intr_type = GPIO_INTR_ANYEDGE,
    };
    ESP_ERROR_CHECK(gpio_config(&io));
    esp_err_t e = gpio_install_isr_service(0);
    if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) {
        ESP_ERROR_CHECK(e);
    }
    ESP_ERROR_CHECK(gpio_isr_handler_add(CC1101_PIN_GDO2, cs_isr, NULL));

    /* Below the Zigbee task (priority 5): the radio must never starve routing. */
    xTaskCreate(rx_task, "pico_rx", 6144, NULL, 4, NULL);
}
