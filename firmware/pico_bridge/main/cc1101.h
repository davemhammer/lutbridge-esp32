/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Dave Hammer
 */
/*
 * Receive-only CC1101 driver.
 *
 * There is intentionally no STX strobe, no way to select TX, and no PATABLE
 * write in this API. cc1101_strobe() takes an enum that only lists receive-side
 * and housekeeping strobes, so a bug elsewhere cannot key the 433 MHz radio.
 */
#pragma once

#include <stdint.h>

#include "esp_err.h"

/* Wiring from pinout.md (ESP32-C6 Super Mini GPIO numbers). */
#define CC1101_PIN_SCK  2
#define CC1101_PIN_MISO 3
#define CC1101_PIN_MOSI 4
#define CC1101_PIN_CSN  5
#define CC1101_PIN_GDO0 6
#define CC1101_PIN_GDO2 7

/* Command strobes we allow. STX (0x35), SFSTXON (0x31) and SWOR (0x38) are
 * deliberately absent. */
typedef enum {
    CC1101_SRES = 0x30,  /* reset chip */
    CC1101_SCAL = 0x33,  /* calibrate synthesizer, from idle */
    CC1101_SRX  = 0x34,  /* enter RX */
    CC1101_SIDLE = 0x36, /* go idle */
    CC1101_SFRX = 0x3A,  /* flush RX FIFO */
} cc1101_strobe_t;

/* Configuration/status register addresses used by this firmware. */
#define CC1101_REG_IOCFG2   0x00
#define CC1101_REG_IOCFG0   0x02
#define CC1101_REG_PKTLEN   0x06
#define CC1101_REG_PKTCTRL1 0x07
#define CC1101_REG_PKTCTRL0 0x08
#define CC1101_REG_FSCTRL1  0x0B
#define CC1101_REG_FSCTRL0  0x0C
#define CC1101_REG_FREQ2    0x0D
#define CC1101_REG_FREQ1    0x0E
#define CC1101_REG_FREQ0    0x0F
#define CC1101_REG_MDMCFG4  0x10
#define CC1101_REG_MDMCFG3  0x11
#define CC1101_REG_MDMCFG2  0x12
#define CC1101_REG_MDMCFG1  0x13
#define CC1101_REG_MDMCFG0  0x14
#define CC1101_REG_DEVIATN  0x15
#define CC1101_REG_MCSM2    0x16
#define CC1101_REG_MCSM1    0x17
#define CC1101_REG_MCSM0    0x18
#define CC1101_REG_FOCCFG   0x19
#define CC1101_REG_BSCFG    0x1A
#define CC1101_REG_AGCCTRL2 0x1B
#define CC1101_REG_AGCCTRL1 0x1C
#define CC1101_REG_AGCCTRL0 0x1D
#define CC1101_REG_FREND1   0x21
#define CC1101_REG_FREND0   0x22
#define CC1101_REG_FSCAL3   0x23
#define CC1101_REG_FSCAL2   0x24
#define CC1101_REG_FSCAL1   0x25
#define CC1101_REG_FSCAL0   0x26
#define CC1101_REG_TEST2    0x2C
#define CC1101_REG_TEST1    0x2D
#define CC1101_REG_TEST0    0x2E
#define CC1101_REG_PARTNUM  0x30 /* status */
#define CC1101_REG_VERSION  0x31 /* status */
#define CC1101_REG_RSSI     0x34 /* status */
#define CC1101_REG_MARCSTATE 0x35 /* status */

esp_err_t cc1101_init(void);                  /* bus + CSN setup, hardware reset */
uint8_t cc1101_strobe(cc1101_strobe_t s);     /* returns chip status byte */
uint8_t cc1101_read_reg(uint8_t addr);        /* config register */
uint8_t cc1101_read_status(uint8_t addr);     /* status register (0x30..0x3D) */
void cc1101_write_reg(uint8_t addr, uint8_t v);

/* Apply the async-serial receive configuration and enter RX. */
esp_err_t cc1101_start_async_rx(void);

/* Convert the RSSI status register value to dBm (CC1101 datasheet, 26 MHz offset -74). */
int cc1101_rssi_dbm(uint8_t raw);
