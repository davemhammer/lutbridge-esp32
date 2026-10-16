/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Dave Hammer
 */
/*
 * ESP32-C6 Lutron Pico fallback bridge: Zigbee router (commissioning here), CC1101 receive task,
 * and the bridge application in bridge.c. See docs/protocol.md and README.md.
 * No Wi-Fi, no Bluetooth, and no 433 MHz transmit code anywhere.
 */
#include "bridge.h"
#include "cc1101.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "rx_pipeline.h"

#include "esp_zigbee.h"

static const char *TAG = "pico_bridge";

#define BIND_TABLE_SRC   64  /* requested APS source binding rows (24 endpoints x On/Off + Level = 48) */
#define STEERING_RETRY_MS 5000

/* ---- commissioning ------------------------------------------------------ */

static esp_timer_handle_t s_steer_timer;

static void steering_start_cb(void *ctx)
{
    (void)ctx; /* runs in the Zigbee task, no lock needed */
    ESP_LOGI(TAG, "network steering: scanning for an open network");
    ezb_bdb_start_top_level_commissioning(EZB_BDB_MODE_NETWORK_STEERING);
}

static void steering_timer_cb(void *arg)
{
    (void)arg;
    esp_zigbee_task_queue_post(steering_start_cb, NULL);
}

static void steering_retry(void)
{
    esp_timer_stop(s_steer_timer);
    esp_timer_start_once(s_steer_timer, (uint64_t)STEERING_RETRY_MS * 1000);
}

static void log_network(void)
{
    ezb_extaddr_t ieee;
    ezb_get_extended_address(&ieee); /* little-endian; Zigbee2MQTT prints it reversed */
    ESP_LOGI(TAG, "ieee=0x%02x%02x%02x%02x%02x%02x%02x%02x",
             ieee.u8[7], ieee.u8[6], ieee.u8[5], ieee.u8[4], ieee.u8[3], ieee.u8[2], ieee.u8[1], ieee.u8[0]);

    ezb_extpanid_t ext;
    ezb_get_extended_panid(&ext);
    ESP_LOGI(TAG, "joined: pan=0x%04x ch=%u short=0x%04x ext_pan=%02x%02x%02x%02x%02x%02x%02x%02x",
             ezb_get_panid(), ezb_get_current_channel(), ezb_get_short_address(),
             ext.u8[7], ext.u8[6], ext.u8[5], ext.u8[4], ext.u8[3], ext.u8[2], ext.u8[1], ext.u8[0]);
}

static bool app_signal_handler(const ezb_app_signal_t *signal)
{
    ezb_app_signal_type_t type = ezb_app_signal_get_type(signal);
    const void *params = ezb_app_signal_get_params(signal);
    const ezb_bdb_signal_simple_params_t *bdb = params;

    switch (type) {
    case EZB_ZDO_SIGNAL_SKIP_STARTUP:
        ESP_LOGI(TAG, "zigbee stack up, initializing BDB");
        ezb_bdb_start_top_level_commissioning(EZB_BDB_MODE_INITIALIZATION);
        break;

    case EZB_BDB_SIGNAL_DEVICE_FIRST_START:
        if (bdb && bdb->status == EZB_BDB_STATUS_SUCCESS) {
            ESP_LOGI(TAG, "factory new: start steering (enable permit-join in Zigbee2MQTT)");
            ezb_bdb_start_top_level_commissioning(EZB_BDB_MODE_NETWORK_STEERING);
        } else {
            ESP_LOGW(TAG, "first start failed, status=%d", bdb ? bdb->status : -1);
        }
        break;

    case EZB_BDB_SIGNAL_DEVICE_REBOOT:
        if (bdb && bdb->status == EZB_BDB_STATUS_SUCCESS) {
            ESP_LOGI(TAG, "rebooted onto stored network");
            log_network();
        } else {
            ESP_LOGW(TAG, "reboot rejoin failed (status=%d), steering", bdb ? bdb->status : -1);
            ezb_bdb_start_top_level_commissioning(EZB_BDB_MODE_NETWORK_STEERING);
        }
        break;

    case EZB_BDB_SIGNAL_STEERING:
        if (bdb && bdb->status == EZB_BDB_STATUS_SUCCESS) {
            log_network();
        } else {
            ESP_LOGW(TAG, "steering failed (status=%d), retry in %d ms", bdb ? bdb->status : -1, STEERING_RETRY_MS);
            steering_retry();
        }
        break;

    case EZB_ZDO_SIGNAL_LEAVE:
        ESP_LOGW(TAG, "left the network");
        break;

    case EZB_NWK_SIGNAL_PERMIT_JOIN_STATUS: {
        const ezb_nwk_signal_permit_join_status_params_t *pj = params;
        ESP_LOGI(TAG, "permit-join %us", pj ? pj->duration : 0);
        break;
    }

    default:
        ESP_LOGI(TAG, "signal %s (0x%04x)", ezb_app_signal_to_string(type), (unsigned)type);
        break;
    }
    return true;
}

/* ---- CC1101: chip check, async RX, receive pipeline, periodic status -------- */

static void rf_task(void *arg)
{
    (void)arg;
    if (cc1101_init() != ESP_OK) {
        ESP_LOGE(TAG, "cc1101 spi init failed");
        vTaskDelete(NULL);
    }

    uint8_t part = cc1101_read_status(CC1101_REG_PARTNUM);
    uint8_t ver = cc1101_read_status(CC1101_REG_VERSION);
    ESP_LOGI(TAG, "cc1101 PARTNUM=0x%02x VERSION=0x%02x (expect 0x00 / 0x14 or 0x04)", part, ver);

    /* Register write/readback proves MOSI, MISO, SCK and CSN all work. */
    cc1101_write_reg(CC1101_REG_PKTLEN, 0x5A);
    uint8_t rb1 = cc1101_read_reg(CC1101_REG_PKTLEN);
    cc1101_write_reg(CC1101_REG_PKTLEN, 0xA5);
    uint8_t rb2 = cc1101_read_reg(CC1101_REG_PKTLEN);
    bool spi_ok = rb1 == 0x5A && rb2 == 0xA5;
    ESP_LOGI(TAG, "register readback: wrote 5A got %02x, wrote A5 got %02x -> %s", rb1, rb2, spi_ok ? "OK" : "FAIL");
    if (!spi_ok || (part == 0xFF && ver == 0xFF) || (part == 0x00 && ver == 0x00)) {
        ESP_LOGE(TAG, "cc1101 not responding; check wiring (CSN=%d SCK=%d MOSI=%d MISO=%d) and 3V3",
                 CC1101_PIN_CSN, CC1101_PIN_SCK, CC1101_PIN_MOSI, CC1101_PIN_MISO);
        vTaskDelete(NULL);
    }

    if (cc1101_start_async_rx() != ESP_OK) {
        ESP_LOGE(TAG, "cc1101 did not enter RX");
    }

    rx_pipeline_start();

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        int dbm = cc1101_rssi_dbm(cc1101_read_status(CC1101_REG_RSSI));
        uint8_t marc = cc1101_read_status(CC1101_REG_MARCSTATE) & 0x1F;
        char bs[200];
        bridge_status(bs, sizeof bs);
        ESP_LOGI(TAG, "rf: rssi=%d dBm marc=0x%02x glitches=%u bursts=%u crc_ok=%u crc_fail=%u no_frame=%u events=%u",
                 dbm, marc, (unsigned)g_rx_stats.glitches, (unsigned)g_rx_stats.bursts,
                 (unsigned)g_rx_stats.frames_ok, (unsigned)g_rx_stats.frames_bad, (unsigned)g_rx_stats.no_frame,
                 (unsigned)g_rx_stats.events);
        ESP_LOGI(TAG, "bridge: %s", bs);
    }
}

/* ---- main --------------------------------------------------------------- */

static void zigbee_task(void *arg)
{
    (void)arg;
    esp_zigbee_launch_mainloop(); /* returns only if the stack is stopped */
    vTaskDelete(NULL);
}

void app_main(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());
    bridge_init_state();

    esp_zigbee_config_t config = {0};
    config.device_config.device_type = EZB_NWK_DEVICE_TYPE_ROUTER;
    config.device_config.install_code_policy = false;
    config.device_config.zczr_config.max_children = 10;
    config.platform_config.storage_partition_name = "nvs";
    config.platform_config.radio_config.radio_mode = ESP_ZIGBEE_RADIO_MODE_NATIVE;
    ESP_ERROR_CHECK(esp_zigbee_init(&config));

    ezb_mem_config_t mem = {0}; /* 0 = stack default for every other table */
    mem.aps_bind_table_src_size = BIND_TABLE_SRC;
    ezb_err_t mem_err = ezb_config_memory(&mem);
    ESP_LOGI(TAG, "requested aps_bind_table_src_size=%d, ezb_config_memory -> %d (request, not a measurement)",
             BIND_TABLE_SRC, (int)mem_err);

    esp_timer_create_args_t targs = {.callback = steering_timer_cb, .name = "steer"};
    ESP_ERROR_CHECK(esp_timer_create(&targs, &s_steer_timer));

    bridge_register_data_model();
    ESP_ERROR_CHECK(ezb_app_signal_add_handler(app_signal_handler) == EZB_ERR_NONE ? ESP_OK : ESP_FAIL);

    ESP_ERROR_CHECK(esp_zigbee_start(false));
    xTaskCreate(zigbee_task, "zigbee", 8192, NULL, 5, NULL);
    bridge_start_tasks();
    xTaskCreate(rf_task, "rf", 4096, NULL, 2, NULL);
}
