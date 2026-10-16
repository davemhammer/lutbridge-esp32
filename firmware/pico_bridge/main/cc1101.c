/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Dave Hammer
 */
#include "cc1101.h"

#include <string.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "cc1101";

static spi_device_handle_t s_dev;
static SemaphoreHandle_t s_lock; /* serializes SPI between the RF tasks */

/* Wait for the chip to pull MISO low (crystal stable / ready) with CSN low. */
static esp_err_t wait_miso_low(uint32_t timeout_us)
{
    int64_t end = esp_timer_get_time() + timeout_us;
    while (gpio_get_level(CC1101_PIN_MISO)) {
        if (esp_timer_get_time() > end) {
            return ESP_ERR_TIMEOUT;
        }
    }
    return ESP_OK;
}

/* One CSN-framed transaction. Returns ESP_ERR_TIMEOUT if the chip never answers. */
static esp_err_t xfer(const uint8_t *tx, uint8_t *rx, size_t len)
{
    if (s_lock) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }
    gpio_set_level(CC1101_PIN_CSN, 0);
    esp_err_t err = wait_miso_low(5000);
    if (err == ESP_OK) {
        spi_transaction_t t = {
            .length = len * 8,
            .tx_buffer = tx,
            .rx_buffer = rx,
        };
        err = spi_device_polling_transmit(s_dev, &t);
    }
    gpio_set_level(CC1101_PIN_CSN, 1);
    if (s_lock) {
        xSemaphoreGive(s_lock);
    }
    return err;
}

uint8_t cc1101_strobe(cc1101_strobe_t s)
{
    uint8_t tx = (uint8_t)s, rx = 0xFF;
    xfer(&tx, &rx, 1);
    return rx;
}

uint8_t cc1101_read_reg(uint8_t addr)
{
    uint8_t tx[2] = {(uint8_t)(0x80 | addr), 0}, rx[2] = {0xFF, 0xFF};
    xfer(tx, rx, 2);
    return rx[1];
}

uint8_t cc1101_read_status(uint8_t addr)
{
    uint8_t tx[2] = {(uint8_t)(0xC0 | addr), 0}, rx[2] = {0xFF, 0xFF}; /* burst bit selects status regs */
    xfer(tx, rx, 2);
    return rx[1];
}

void cc1101_write_reg(uint8_t addr, uint8_t v)
{
    uint8_t tx[2] = {addr, v}, rx[2];
    xfer(tx, rx, 2);
}

esp_err_t cc1101_init(void)
{
    gpio_config_t cs = {
        .pin_bit_mask = 1ULL << CC1101_PIN_CSN,
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&cs), TAG, "csn gpio");
    gpio_set_level(CC1101_PIN_CSN, 1);
    s_lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_lock, ESP_ERR_NO_MEM, TAG, "mutex");

    spi_bus_config_t bus = {
        .mosi_io_num = CC1101_PIN_MOSI,
        .miso_io_num = CC1101_PIN_MISO,
        .sclk_io_num = CC1101_PIN_SCK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 16,
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_DISABLED), TAG, "spi bus");

    spi_device_interface_config_t dev = {
        .clock_speed_hz = 1000000, /* well under the 6.5 MHz limit for burst access */
        .mode = 0,
        .spics_io_num = -1, /* CSN is driven by hand so we can wait for MISO low */
        .queue_size = 1,
    };
    ESP_RETURN_ON_ERROR(spi_bus_add_device(SPI2_HOST, &dev, &s_dev), TAG, "spi dev");

    /* Manual power-on reset sequence from the datasheet, then SRES. */
    gpio_set_level(CC1101_PIN_CSN, 1);
    esp_rom_delay_us(10);
    gpio_set_level(CC1101_PIN_CSN, 0);
    esp_rom_delay_us(10);
    gpio_set_level(CC1101_PIN_CSN, 1);
    esp_rom_delay_us(60);
    cc1101_strobe(CC1101_SRES);
    esp_rom_delay_us(2000);
    return ESP_OK;
}

int cc1101_rssi_dbm(uint8_t raw)
{
    int v = raw >= 128 ? (int)raw - 256 : (int)raw;
    return v / 2 - 74;
}

typedef struct {
    uint8_t reg;
    uint8_t val;
} reg_val_t;

/*
 * Async serial receive, 2-FSK, 62.5 kBaud, 433.592 MHz, 26 MHz crystal.
 * The modulation parameters come from measuring real Pico transmissions; the AGC, carrier-sense and front-end
 * values were tuned on the bench until bursts decoded reliably (see "Notes from bring-up" in README.md).
 */
static const reg_val_t k_async_rx[] = {
    {CC1101_REG_IOCFG2,   0x0E}, /* GDO2: carrier sense, marks the start and end of a burst */
    {CC1101_REG_IOCFG0,   0x0D}, /* GDO0: serial data output (async) */
    {CC1101_REG_PKTCTRL1, 0x00},
    {CC1101_REG_PKTCTRL0, 0x32}, /* async serial mode, infinite length, no CRC */
    {CC1101_REG_FSCTRL1,  0x08},
    {CC1101_REG_FSCTRL0,  0x00},
    {CC1101_REG_FREQ2,    0x10}, /* 0x10AD37 -> ~433.592 MHz */
    {CC1101_REG_FREQ1,    0xAD},
    {CC1101_REG_FREQ0,    0x37},
    {CC1101_REG_MDMCFG4,  0x0B}, /* 812 kHz channel BW, DRATE_E=11 */
    {CC1101_REG_MDMCFG3,  0x3B}, /* DRATE_M=59 -> ~62.48 kBaud */
    {CC1101_REG_MDMCFG2,  0x00}, /* 2-FSK, no Manchester, no sync */
    {CC1101_REG_MDMCFG1,  0x22},
    {CC1101_REG_MDMCFG0,  0xF8},
    {CC1101_REG_DEVIATN,  0x45}, /* ~41.2 kHz */
    {CC1101_REG_MCSM1,    0x3F}, /* stay in RX after RX and TX */
    {CC1101_REG_MCSM0,    0x18}, /* auto-calibrate on idle -> RX */
    {CC1101_REG_FOCCFG,   0x1D},
    {CC1101_REG_BSCFG,    0x1C},
    {CC1101_REG_AGCCTRL2, 0xC7},
    {CC1101_REG_AGCCTRL1, 0x07}, /* carrier sense: relative off, absolute +7 dB (the Pico is ~50 dB over the noise floor) */
    {CC1101_REG_AGCCTRL0, 0xB0},
    {CC1101_REG_FREND1,   0xB6},
    {CC1101_REG_FREND0,   0x10},
    {CC1101_REG_FSCAL3,   0xEA},
    {CC1101_REG_FSCAL2,   0x2A},
    {CC1101_REG_FSCAL1,   0x00},
    {CC1101_REG_FSCAL0,   0x1F},
    {CC1101_REG_TEST2,    0x81},
    {CC1101_REG_TEST1,    0x35},
    {CC1101_REG_TEST0,    0x09},
};

esp_err_t cc1101_start_async_rx(void)
{
    cc1101_strobe(CC1101_SIDLE);
    for (size_t i = 0; i < sizeof k_async_rx / sizeof k_async_rx[0]; i++) {
        cc1101_write_reg(k_async_rx[i].reg, k_async_rx[i].val);
    }
    /* No PATABLE write and no STX anywhere: this radio only receives. */
    cc1101_strobe(CC1101_SFRX);
    cc1101_strobe(CC1101_SRX);
    esp_rom_delay_us(1000);

    uint8_t marc = cc1101_read_status(CC1101_REG_MARCSTATE) & 0x1F;
    ESP_LOGI(TAG, "MARCSTATE=0x%02x (0x0d = RX)", marc);
    return marc == 0x0D ? ESP_OK : ESP_FAIL;
}
