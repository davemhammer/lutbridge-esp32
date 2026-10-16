# Pinout

Wiring between a CC1101 module (2x4 header) and an ESP32-C6 "Super Mini" style board.

The CC1101 is a 3.3 V part. Never connect it to 5 V.

## Connections

Pin numbers are the module's 2x4 header pins, assuming the common header order (GND, VCC, GDO0, CSN, SCK, MOSI,
MISO, GDO2). Check pins 1 and 2 against your board's silkscreen before powering it, because modules differ. Wire
colors depend on your cable, so write yours in the last column.

| Module pin | CC1101 signal | ESP32-C6 pin | Your wire color |
|------------|---------------|--------------|-----------------|
| 1          | GND           | GND          |                 |
| 2          | VCC           | 3V3          |                 |
| 3          | GDO0          | GPIO6        |                 |
| 4          | CSN           | GPIO5        |                 |
| 5          | SCK           | GPIO2        |                 |
| 6          | MOSI          | GPIO4        |                 |
| 7          | MISO          | GPIO3        |                 |
| 8          | GDO2          | GPIO7        |                 |

The firmware takes these pins from `firmware/pico_bridge/main/cc1101.h`. To use other pins, change them there;
the C6 routes SPI through its GPIO matrix, so any free GPIO works for SCK, MISO, MOSI and CSN.

## Signals

| Signal | Direction (from the ESP) | Purpose |
|--------|--------------------------|---------|
| SCK    | out | SPI clock |
| MOSI   | out | SPI data to the CC1101 |
| MISO   | in  | SPI data from the CC1101 |
| CSN    | out | SPI chip select, active low |
| GDO0   | in  | Demodulated bit stream (asynchronous serial receive), captured by the RMT peripheral |
| GDO2   | in  | Carrier sense, used to start a capture |

SCK is the SPI clock; CSN is chip select (some boards label it CS or CSn).

## Board notes (ESP32-C6 Super Mini style)

- 20 pins: one side has 5V, GND, 3V3, then GPIO20, 19 and others; the other side has TX, RX and GPIO0 to 7.
- All GPIOs are 3.3 V only.
- GPIO8, GPIO9 and GPIO15 are strapping pins, so they are avoided. GPIO9 is also the BOOT button.
- Attach an antenna matched to the module's band (this project uses 433.92 MHz class modules) before use.

## Checking the wiring

On boot the firmware reads the chip's `PARTNUM` and `VERSION` registers and logs them. A CC1101 reports
`PARTNUM 0x00` and `VERSION 0x14` (0x04 also occurs and is accepted). All-zero or all-one reads mean a
wiring or power problem, usually MISO, CSN or the supply.
