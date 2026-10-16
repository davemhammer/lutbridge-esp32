/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Dave Hammer
 */
#include "bridge.h"

#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_zigbee.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include "pico_logic.h"
#include "rx_pipeline.h"

static const char *TAG = "bridge";

#define DEVICE_ID_CUSTOM 0xFF01 /* custom device id, not On/Off Switch */
#define HB_EP 1
#define CLUSTER_PICO 0xEF00 /* below the manufacturer-specific range 0xFC00+, so plain ZCL frames reach it */
#define ATTR_CLAIM 0x0000
#define ATTR_ARMED 0x0001
#define ATTR_SLOT_BASE 0x0100
#define CMD_CLAIM 0x00
#define CMD_SETSLOT 0x01
#define CMD_CLAIM_SLOTS 0x02 /* Claim with a mask of the slots it covers; a separate command so old firmware says UNSUP */
#define DIM_RATE 64 /* units per second: 254 / 64 is about 4 s from bottom to top */
#define NVS_NS "pico_map"
#define NVS_VERSION 1
#define SLOT_BYTES 5 /* serial u32 big-endian + mask */

volatile bridge_stats_t g_bridge_stats;

static pico_slots_t s_slots;
static pico_arm_t s_arm;
static uint32_t s_moving;
static SemaphoreHandle_t s_lock; /* guards s_slots, s_arm, s_moving */
static QueueHandle_t s_stop_q;   /* slots that need a Stop after a claim */
static uint32_t s_last_armed_mask = PICO_ALL_SLOTS; /* armed slots we last reported, among those in use */

/* Basic cluster strings are ZCL character strings: length byte, then bytes. */
static char s_manufacturer[16];
static char s_model[32];

static int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

/* Slots with a Pico in them. Caller holds s_lock. */
static uint32_t used_mask_locked(void)
{
    uint32_t m = 0;
    for (int i = 0; i < PICO_SLOT_COUNT; i++) {
        if (s_slots.slot[i].serial) {
            m |= 1u << i;
        }
    }
    return m;
}

/* Slots that are armed and in use; with nothing provisioned, whether anything is armed at all. Caller holds s_lock. */
static uint32_t armed_in_use_locked(int64_t now)
{
    uint32_t armed = pico_arm_armed_mask(&s_arm, now), used = used_mask_locked();
    return used ? (armed & used) : armed;
}

static void zcl_str_init(char *dst, size_t cap, const char *s)
{
    size_t n = strlen(s);
    if (n + 1 > cap) {
        n = cap - 1;
    }
    dst[0] = (char)n;
    memcpy(dst + 1, s, n);
}

/* ---- persistence ----------------------------------------------------------- */

static void slots_encode(const pico_slots_t *t, uint8_t *out)
{
    for (int i = 0; i < PICO_SLOT_COUNT; i++) {
        uint32_t s = t->slot[i].serial;
        out[i * SLOT_BYTES + 0] = (uint8_t)(s >> 24);
        out[i * SLOT_BYTES + 1] = (uint8_t)(s >> 16);
        out[i * SLOT_BYTES + 2] = (uint8_t)(s >> 8);
        out[i * SLOT_BYTES + 3] = (uint8_t)s;
        out[i * SLOT_BYTES + 4] = t->slot[i].mask;
    }
}

static void slots_save(const pico_slots_t *t)
{
    uint8_t buf[PICO_SLOT_COUNT * SLOT_BYTES];
    slots_encode(t, buf);
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed, slot table not saved");
        return;
    }
    nvs_set_blob(h, "slots", buf, sizeof buf);
    nvs_set_u8(h, "ver", NVS_VERSION);
    nvs_commit(h);
    nvs_close(h);
}

static void slots_load(pico_slots_t *t)
{
    memset(t, 0, sizeof *t);
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        ESP_LOGI(TAG, "no stored slot table (first boot)");
        return;
    }
    uint8_t ver = 0;
    uint8_t buf[PICO_SLOT_COUNT * SLOT_BYTES];
    size_t len = sizeof buf;
    if (nvs_get_u8(h, "ver", &ver) == ESP_OK && ver == NVS_VERSION && nvs_get_blob(h, "slots", buf, &len) == ESP_OK &&
        len == sizeof buf) {
        for (int i = 0; i < PICO_SLOT_COUNT; i++) {
            uint32_t s = ((uint32_t)buf[i * SLOT_BYTES] << 24) | ((uint32_t)buf[i * SLOT_BYTES + 1] << 16) |
                         ((uint32_t)buf[i * SLOT_BYTES + 2] << 8) | buf[i * SLOT_BYTES + 3];
            pico_slots_set(t, (unsigned)i, s, buf[i * SLOT_BYTES + 4]);
        }
    } else {
        /* A stored table in another layout must not be trusted: start empty and let the sync rewrite it. */
        ESP_LOGW(TAG, "stored slot table has version %u (want %u) or bad size, ignoring it", ver, NVS_VERSION);
    }
    nvs_close(h);
}

void bridge_init_state(void)
{
    s_lock = xSemaphoreCreateMutex();
    s_stop_q = xQueueCreate(8, sizeof(int));
    slots_load(&s_slots);
    pico_arm_init(&s_arm);
    s_moving = 0;
    s_last_armed_mask = armed_in_use_locked(now_ms()); /* boot is armed, so this is every slot in use */

    int used = 0;
    for (int i = 0; i < PICO_SLOT_COUNT; i++) {
        if (s_slots.slot[i].serial) {
            used++;
        }
    }
    ESP_LOGI(TAG, "boot armed; %d of %d slots in use", used, PICO_SLOT_COUNT);
}

/* ---- Zigbee commands ------------------------------------------------------- */

static void send_onoff(uint8_t src_ep, bool on)
{
    ezb_zcl_on_off_cmd_t cmd = {.cmd_ctrl = {.dst_addr = EZB_ADDRESS_NONE(), .src_ep = src_ep, .dis_default_rsp = true}};
    if (!esp_zigbee_lock_acquire(portMAX_DELAY)) {
        g_bridge_stats.send_failed++;
        return;
    }
    ezb_err_t err = on ? ezb_zcl_on_off_on_cmd_req(&cmd) : ezb_zcl_on_off_off_cmd_req(&cmd);
    esp_zigbee_lock_release();
    (err == EZB_ERR_NONE ? g_bridge_stats.sent++ : g_bridge_stats.send_failed++);
    ESP_LOGI(TAG, "send %s via binding from ep %u: err=%d", on ? "ON" : "OFF", src_ep, (int)err);
}

static void send_move(uint8_t src_ep, bool up)
{
    ezb_zcl_level_move_cmd_t cmd = {
        .cmd_ctrl = {.dst_addr = EZB_ADDRESS_NONE(), .src_ep = src_ep, .dis_default_rsp = true},
        .payload = {.move_mode = up ? EZB_ZCL_LEVEL_FADE_MODE_UP : EZB_ZCL_LEVEL_FADE_MODE_DOWN,
                    .rate = DIM_RATE,
                    .options_mask = 0,
                    .options_override = 0},
    };
    if (!esp_zigbee_lock_acquire(portMAX_DELAY)) {
        g_bridge_stats.send_failed++;
        return;
    }
    ezb_err_t err = ezb_zcl_level_move_with_on_off_cmd_req(&cmd);
    esp_zigbee_lock_release();
    (err == EZB_ERR_NONE ? g_bridge_stats.sent++ : g_bridge_stats.send_failed++);
    ESP_LOGI(TAG, "send MOVE %s rate=%d with on/off via binding from ep %u: err=%d", up ? "UP" : "DOWN", DIM_RATE,
             src_ep, (int)err);
}

static void send_stop(uint8_t src_ep)
{
    ezb_zcl_level_stop_cmd_t cmd = {.cmd_ctrl = {.dst_addr = EZB_ADDRESS_NONE(), .src_ep = src_ep, .dis_default_rsp = true}};
    if (!esp_zigbee_lock_acquire(portMAX_DELAY)) {
        g_bridge_stats.send_failed++;
        return;
    }
    ezb_err_t err = ezb_zcl_level_stop_with_on_off_cmd_req(&cmd);
    esp_zigbee_lock_release();
    (err == EZB_ERR_NONE ? g_bridge_stats.sent++ : g_bridge_stats.send_failed++);
    ESP_LOGI(TAG, "send STOP with on/off via binding from ep %u: err=%d", src_ep, (int)err);
}

/* ---- routing a decoded Pico press ------------------------------------------ */

void pico_on_event(uint32_t serial, uint8_t button_byte, bool press)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    pico_decision_t d = pico_decide(&s_slots, &s_arm, now_ms(), serial, button_byte, press, &s_moving);
    xSemaphoreGive(s_lock);

    switch (d.why) {
    case PICO_WHY_SEND:
        break;
    case PICO_WHY_DISARMED:
        g_bridge_stats.suppressed++;
        ESP_LOGI(TAG, "serial=%08x button=0x%02x %s: would-have-sent %s from ep %u (disarmed)", (unsigned)serial,
                 button_byte, press ? "press" : "release", pico_cmd_name(d.cmd), d.endpoint);
        return;
    case PICO_WHY_UNKNOWN_SERIAL:
        g_bridge_stats.unknown++;
        ESP_LOGI(TAG, "serial=%08x not in any slot", (unsigned)serial);
        return;
    case PICO_WHY_NOT_MAPPED:
        g_bridge_stats.not_mapped++;
        ESP_LOGI(TAG, "serial=%08x button=0x%02x is in no slot for this Pico", (unsigned)serial, button_byte);
        return;
    case PICO_WHY_LOG_ONLY:
        ESP_LOGI(TAG, "serial=%08x button=0x%02x is log-only", (unsigned)serial, button_byte);
        return;
    default:
        return;
    }

    switch (d.cmd) {
    case PICO_CMD_ON:        send_onoff(d.endpoint, true); break;
    case PICO_CMD_OFF:       send_onoff(d.endpoint, false); break;
    case PICO_CMD_MOVE_UP:   send_move(d.endpoint, true); break;
    case PICO_CMD_MOVE_DOWN: send_move(d.endpoint, false); break;
    case PICO_CMD_STOP:      send_stop(d.endpoint); break;
    default:                 break;
    }
}

/* ---- heartbeat endpoint: Claim and SetSlot --------------------------------- */

static void mirror_slot_attr(unsigned idx)
{
    uint8_t v[1 + SLOT_BYTES] = {SLOT_BYTES};
    uint8_t enc[PICO_SLOT_COUNT * SLOT_BYTES];
    slots_encode(&s_slots, enc);
    memcpy(v + 1, enc + idx * SLOT_BYTES, SLOT_BYTES);
    ezb_zcl_set_attr_value(HB_EP, CLUSTER_PICO, EZB_ZCL_CLUSTER_SERVER, (uint16_t)(ATTR_SLOT_BASE + idx), EZB_ZCL_STD_MANUF_CODE, v,
                           false);
}

/* Runs inside the Zigbee task: no esp_zigbee_lock_acquire here. Accept only a command from the
 * coordinator (short address 0x0000) before touching any state. */
static ezb_zcl_status_t on_pico_cmd(const ezb_zcl_cmd_hdr_t *h, const uint8_t *p, uint16_t n)
{
    ESP_LOGD(TAG, "command id=0x%02x len=%u", h ? (unsigned)h->cmd_id : 0xffu, (unsigned)n);
    if (!h || !ezb_address_is_short(&h->src_addr, 0x0000)) {
        g_bridge_stats.ignored++;
        return EZB_ZCL_STATUS_NOT_AUTHORIZED;
    }

    /* Claim (0x00): counter u8; every slot is handled.
     * ClaimSlots (0x02): counter u8, then the slots being handled as a u32 bitmask, most significant byte first
     * (bit i = slot i). */
    if ((h->cmd_id == CMD_CLAIM && n == 1) || (h->cmd_id == CMD_CLAIM_SLOTS && n == 5)) {
        uint32_t handled = PICO_ALL_SLOTS;
        if (h->cmd_id == CMD_CLAIM_SLOTS) {
            handled = (((uint32_t)p[1] << 24) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 8) | p[4]) & PICO_ALL_SLOTS;
        }
        int stops[PICO_SLOT_COUNT];
        int ns = 0, s;
        xSemaphoreTake(s_lock, portMAX_DELAY);
        pico_arm_claim(&s_arm, now_ms(), handled);
        while ((s = pico_moving_pop(&s_moving, handled)) >= 0 && ns < PICO_SLOT_COUNT) {
            stops[ns++] = s;
        }
        bool armed = armed_in_use_locked(now_ms()) != 0;
        xSemaphoreGive(s_lock);
        g_bridge_stats.claims++;
        for (int i = 0; i < ns; i++) {
            xQueueSend(s_stop_q, &stops[i], 0); /* sent from the worker task, which may take the Zigbee lock */
        }
        uint8_t counter = p[0];
        ezb_zcl_set_attr_value(HB_EP, CLUSTER_PICO, EZB_ZCL_CLUSTER_SERVER, ATTR_CLAIM, EZB_ZCL_STD_MANUF_CODE, &counter, false);
        ezb_zcl_set_attr_value(HB_EP, CLUSTER_PICO, EZB_ZCL_CLUSTER_SERVER, ATTR_ARMED, EZB_ZCL_STD_MANUF_CODE, &armed, false);
        return EZB_ZCL_STATUS_SUCCESS;
    }

    if (h->cmd_id == CMD_SETSLOT && n == 6) {
        unsigned idx = p[0];
        uint32_t serial = ((uint32_t)p[1] << 24) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 8) | p[4];
        uint8_t mask = p[5];
        xSemaphoreTake(s_lock, portMAX_DELAY);
        bool ok = pico_slots_set(&s_slots, idx, serial, mask);
        pico_slots_t copy = s_slots;
        xSemaphoreGive(s_lock);
        if (!ok) {
            g_bridge_stats.setslot_bad++;
            ESP_LOGW(TAG, "SetSlot rejected: idx=%u serial=%08x mask=0x%02x", idx, (unsigned)serial, mask);
            return EZB_ZCL_STATUS_INVALID_VALUE;
        }
        slots_save(&copy);
        mirror_slot_attr(idx);
        g_bridge_stats.setslot_ok++;
        ESP_LOGI(TAG, "SetSlot idx=%u serial=%08x mask=0x%02x (ep %u)", idx, (unsigned)serial, mask,
                 (unsigned)(PICO_EP_BASE + idx));
        return EZB_ZCL_STATUS_SUCCESS;
    }

    ESP_LOGW(TAG, "rejected command id=0x%02x with %u payload byte(s)", (unsigned)h->cmd_id, (unsigned)n);
    return EZB_ZCL_STATUS_INVALID_VALUE;
}

static uint8_t on_pico_disc(bool is_recv, uint8_t **list)
{
    static uint8_t cmds[] = {CMD_CLAIM, CMD_SETSLOT, CMD_CLAIM_SLOTS};
    if (is_recv) {
        *list = cmds;
        return sizeof cmds;
    }
    return 0;
}

static void cluster_noop(uint8_t ep_id)
{
    (void)ep_id;
}

/* ---- data model ------------------------------------------------------------- */

static ezb_af_ep_desc_t new_endpoint(uint8_t id)
{
    ezb_af_ep_config_t cfg = {
        .ep_id = id,
        .app_profile_id = EZB_AF_HA_PROFILE_ID,
        .app_device_id = DEVICE_ID_CUSTOM,
        .app_device_version = 0,
    };
    ezb_af_ep_desc_t ep = ezb_af_create_endpoint_desc(&cfg);
    ESP_ERROR_CHECK(ep ? ESP_OK : ESP_FAIL);
    return ep;
}

static void add_cluster(ezb_af_ep_desc_t ep, ezb_zcl_cluster_desc_t c)
{
    ESP_ERROR_CHECK(c ? ESP_OK : ESP_FAIL);
    ESP_ERROR_CHECK(ezb_af_endpoint_add_cluster_desc(ep, c) == EZB_ERR_NONE ? ESP_OK : ESP_FAIL);
}

void bridge_register_data_model(void)
{
    zcl_str_init(s_manufacturer, sizeof s_manufacturer, "l-re");
    zcl_str_init(s_model, sizeof s_model, "l-re.pico-bridge");

    ESP_ERROR_CHECK(ezb_af_dev_set_max_endpoint_num(PICO_SLOT_COUNT + 2) == EZB_ERR_NONE ? ESP_OK : ESP_FAIL);
    ezb_af_device_desc_t dev = ezb_af_create_device_desc();
    ESP_ERROR_CHECK(dev ? ESP_OK : ESP_FAIL);

    /* Endpoint 1: identity, plus the heartbeat/status cluster */
    ezb_af_ep_desc_t hb = new_endpoint(HB_EP);

    ezb_zcl_basic_cluster_server_config_t basic_cfg = {
        .zcl_version = EZB_ZCL_BASIC_ZCL_VERSION_DEFAULT_VALUE,
        .power_source = EZB_ZCL_BASIC_POWER_SOURCE_SINGLE_PHASE_MAINS,
    };
    ezb_zcl_cluster_desc_t basic = ezb_zcl_basic_create_cluster_desc(&basic_cfg, EZB_ZCL_CLUSTER_SERVER);
    ESP_ERROR_CHECK(basic ? ESP_OK : ESP_FAIL);
    ezb_zcl_basic_cluster_desc_add_attr(basic, EZB_ZCL_ATTR_BASIC_MANUFACTURER_NAME_ID, s_manufacturer);
    ezb_zcl_basic_cluster_desc_add_attr(basic, EZB_ZCL_ATTR_BASIC_MODEL_IDENTIFIER_ID, s_model);
    add_cluster(hb, basic);

    ezb_zcl_custom_cluster_config_t ccfg = {.cluster_id = CLUSTER_PICO, .init_func = cluster_noop, .deinit_func = cluster_noop};
    ezb_zcl_cluster_desc_t pico = ezb_zcl_custom_create_cluster_desc(&ccfg, EZB_ZCL_CLUSTER_SERVER);
    ESP_ERROR_CHECK(pico ? ESP_OK : ESP_FAIL);
    uint8_t claim0 = 0;
    bool armed0 = true;
    ezb_zcl_custom_cluster_desc_add_attr(pico, ATTR_CLAIM, EZB_ZCL_ATTR_TYPE_UINT8, EZB_ZCL_ATTR_ACCESS_READ, &claim0);
    ezb_zcl_custom_cluster_desc_add_attr(pico, ATTR_ARMED, EZB_ZCL_ATTR_TYPE_BOOL, EZB_ZCL_ATTR_ACCESS_READ, &armed0);
    uint8_t enc[PICO_SLOT_COUNT * SLOT_BYTES];
    slots_encode(&s_slots, enc);
    for (int i = 0; i < PICO_SLOT_COUNT; i++) {
        uint8_t v[1 + SLOT_BYTES] = {SLOT_BYTES};
        memcpy(v + 1, enc + i * SLOT_BYTES, SLOT_BYTES);
        ezb_zcl_custom_cluster_desc_add_attr(pico, (uint16_t)(ATTR_SLOT_BASE + i), EZB_ZCL_ATTR_TYPE_OCTSTR,
                                             EZB_ZCL_ATTR_ACCESS_READ, v);
    }
    add_cluster(hb, pico);
    ESP_ERROR_CHECK(ezb_af_device_add_endpoint_desc(dev, hb) == EZB_ERR_NONE ? ESP_OK : ESP_FAIL);

    /* The endpoint pool: each slot is one endpoint with On/Off and Level Control clients, bound by
     * Zigbee2MQTT to one group. */
    for (int i = 0; i < PICO_SLOT_COUNT; i++) {
        ezb_af_ep_desc_t ep = new_endpoint((uint8_t)(PICO_EP_BASE + i));
        add_cluster(ep, ezb_zcl_on_off_create_cluster_desc(NULL, EZB_ZCL_CLUSTER_CLIENT));
        add_cluster(ep, ezb_zcl_level_create_cluster_desc(NULL, EZB_ZCL_CLUSTER_CLIENT));
        ESP_ERROR_CHECK(ezb_af_device_add_endpoint_desc(dev, ep) == EZB_ERR_NONE ? ESP_OK : ESP_FAIL);
    }
    ESP_ERROR_CHECK(ezb_af_device_desc_register(dev) == EZB_ERR_NONE ? ESP_OK : ESP_FAIL);

    ezb_zcl_custom_cluster_handlers_t handlers = {
        .cluster_id = CLUSTER_PICO,
        .cluster_role = EZB_ZCL_CLUSTER_SERVER,
        .check_value_cb = NULL,
        .write_attr_cb = NULL,
        .cmd_disc_cb = on_pico_disc,
        .process_cmd_cb = on_pico_cmd,
    };
    ESP_LOGI(TAG, "custom cluster handlers register -> %d", (int)ezb_zcl_custom_cluster_handlers_register(&handlers));
}

/* ---- tasks ------------------------------------------------------------------ */

/* Sends the Stop for dims that were running when a claim arrived. Not done in the Zigbee callback itself. */
static void worker_task(void *arg)
{
    (void)arg;
    for (;;) {
        int slot;
        if (xQueueReceive(s_stop_q, &slot, portMAX_DELAY) == pdTRUE) {
            ESP_LOGI(TAG, "claim: stopping the dim on slot %d", slot);
            send_stop((uint8_t)(PICO_EP_BASE + slot));
        }
    }
}

/* Logs arm transitions and keeps the readable `armed` attribute honest. */
static void monitor_task(void *arg)
{
    (void)arg;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(500));
        xSemaphoreTake(s_lock, portMAX_DELAY);
        uint32_t armed_mask = armed_in_use_locked(now_ms()), used = used_mask_locked();
        xSemaphoreGive(s_lock);
        if (armed_mask != s_last_armed_mask) {
            bool was = s_last_armed_mask != 0, armed = armed_mask != 0;
            s_last_armed_mask = armed_mask;
            if (armed_mask == 0) {
                ESP_LOGW(TAG, "DISARMED: claim received, Home Assistant is handling every Pico");
            } else if (armed_mask == used || !used) {
                ESP_LOGW(TAG, "ARMED: no claim for 45 s, Home Assistant is gone");
            } else {
                ESP_LOGW(TAG, "ARMED for some Picos: slots 0x%06x are not being claimed (in use 0x%06x)",
                         (unsigned)armed_mask, (unsigned)used);
            }
            if (was == armed) {
                continue; /* the readable attribute is a single flag; only publish when it flips */
            }
            if (esp_zigbee_lock_acquire(pdMS_TO_TICKS(200))) {
                ezb_zcl_set_attr_value(HB_EP, CLUSTER_PICO, EZB_ZCL_CLUSTER_SERVER, ATTR_ARMED, EZB_ZCL_STD_MANUF_CODE, &armed, false);
                esp_zigbee_lock_release();
            }
        }
    }
}

static void log_simple_desc(uint8_t ep)
{
    const ezb_af_simple_desc_t *d = ezb_af_get_simple_desc(ep);
    if (!d) {
        ESP_LOGW(TAG, "ep %u: no simple descriptor", ep);
        return;
    }
    char line[160];
    int p = snprintf(line, sizeof line, "ep %u profile=0x%04x device=0x%04x in=%u out=%u clusters:", d->ep_id,
                     d->app_profile_id, d->app_device_id, d->app_input_cluster_count, d->app_output_cluster_count);
    for (int i = 0; i < d->app_input_cluster_count + d->app_output_cluster_count && p < (int)sizeof line - 8; i++) {
        p += snprintf(line + p, sizeof line - (size_t)p, " 0x%04x%s", d->app_cluster_list[i],
                      i < d->app_input_cluster_count ? "(in)" : "(out)");
    }
    ESP_LOGI(TAG, "%s", line);
}

static void log_cluster_state(void)
{
    if (!esp_zigbee_lock_acquire(pdMS_TO_TICKS(500))) {
        return;
    }
    ezb_zcl_cluster_desc_t c = ezb_zcl_get_cluster_desc(HB_EP, CLUSTER_PICO, EZB_ZCL_CLUSTER_SERVER);
    if (c) {
        ESP_LOGI(TAG, "cluster 0xef00 server: found, id=0x%04x role=%u manuf=0x%04x", ezb_zcl_cluster_desc_get_id(c),
                 ezb_zcl_cluster_desc_get_role(c), ezb_zcl_cluster_desc_get_manuf_code(c));
    } else {
        ESP_LOGW(TAG, "cluster 0xef00 server: NOT FOUND by the stack");
    }
    ezb_zcl_attr_desc_t a = ezb_zcl_get_attr_desc(HB_EP, CLUSTER_PICO, EZB_ZCL_CLUSTER_SERVER, ATTR_ARMED, EZB_ZCL_STD_MANUF_CODE);
    ESP_LOGI(TAG, "attr armed (std manuf code): %s", a ? "found" : "NOT FOUND");
    a = ezb_zcl_get_attr_desc(HB_EP, CLUSTER_PICO, EZB_ZCL_CLUSTER_SERVER, ATTR_SLOT_BASE, EZB_ZCL_STD_MANUF_CODE);
    ESP_LOGI(TAG, "attr slot0 (std manuf code): %s", a ? "found" : "NOT FOUND");
    esp_zigbee_lock_release();
}

void bridge_start_tasks(void)
{
    log_cluster_state();
    log_simple_desc(HB_EP);
    log_simple_desc(PICO_EP_BASE);
    xTaskCreate(worker_task, "bridge_stop", 3072, NULL, 4, NULL);
    xTaskCreate(monitor_task, "bridge_arm", 3072, NULL, 3, NULL);
}

void bridge_status(char *buf, size_t cap)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    uint32_t armed_mask = armed_in_use_locked(now_ms());
    bool armed = armed_mask != 0;
    int used = 0;
    for (int i = 0; i < PICO_SLOT_COUNT; i++) {
        used += s_slots.slot[i].serial != 0;
    }
    xSemaphoreGive(s_lock);
    snprintf(buf, cap, "armed=%d armed_slots=0x%06x slots=%d claims=%u ignored=%u setslot=%u/%u sent=%u failed=%u suppressed=%u unknown=%u",
             armed, (unsigned)armed_mask, used, (unsigned)g_bridge_stats.claims, (unsigned)g_bridge_stats.ignored,
             (unsigned)g_bridge_stats.setslot_ok, (unsigned)g_bridge_stats.setslot_bad, (unsigned)g_bridge_stats.sent,
             (unsigned)g_bridge_stats.send_failed, (unsigned)g_bridge_stats.suppressed, (unsigned)g_bridge_stats.unknown);
}
