# lutbridge-esp32

A small ESP32-C6 device that listens for Lutron Clear Connect Pico remotes with a CC1101 radio and, when told to,
turns the lights they control on, off or dimmed directly over Zigbee. It joins your Zigbee2MQTT network as a router
and sends standard Zigbee commands through ordinary Zigbee bindings, so no cloud, hub or Home Assistant is on the
path when a button is pressed.

The radio is receive-only. There is no transmit code anywhere in the tree.

It was built as a **fallback**: Home Assistant normally handles every press, and the device takes over only when
Home Assistant stops looking after a Pico. Provisioning that mapping and the heartbeat are done by the companion
Home Assistant integration, **[lutbridge-ha](https://github.com/davemhammer/lutbridge-ha)**. The device also works
on its own (see "Standalone use").

## How it works

```
 Pico ──433 MHz──> CC1101 ──> ESP32-C6 (Zigbee router) ──Zigbee group──> lights
                                  ^
                                  | "claim" heartbeat: "these Picos are handled, stay quiet"
                       lutbridge-ha (or anything that can send the command)
```

- It decodes Pico frames and holds a table of `(Pico serial, buttons) -> endpoint`. Each of its 24 endpoints is
  bound by Zigbee2MQTT to a Zigbee group.
- **Armed**, it sends On, Off, or a Level Control Move/Stop from the matching endpoint. **Disarmed**, it only logs
  what it would have sent.
- It boots armed. A *claim* command disarms the Picos it names for 45 s; when claims stop, those Picos arm
  themselves, each on its own clock.

Details, command formats and radio notes: [`docs/protocol.md`](docs/protocol.md). Wiring: [`pinout.md`](pinout.md).

## What you need

- An ESP32-C6 board (developed on a "Super Mini" style board) and a CC1101 module for 433 MHz, wired as in
  `pinout.md`, with a matching antenna.
- A Zigbee network run by Zigbee2MQTT.
- ESP-IDF 6.0.x to build and flash.
- Lutron Pico remotes (Caseta, 433 MHz Clear Connect). Their on-air serial is the one printed on the remote.

## Build and flash

```
firmware/idf.sh -C firmware/pico_bridge build
firmware/idf.sh -C firmware/pico_bridge -p <serial port> flash
python3 firmware/serial_log.py [seconds] [port] [--no-reset]    # reset the board and print its log
```

`firmware/idf.sh` runs ESP-IDF without sourcing the EIM activation script, which opens a GUI and hangs a
non-interactive shell; adjust it if your ESP-IDF is installed differently. `serial_log.py` needs `pyserial` (the
ESP-IDF Python environment has it). Do not hold the serial port with a capture while flashing.

On a healthy boot the log shows the CC1101 identifying itself (`PARTNUM=0x00 VERSION=0x14`) and the number of
slots in use. Decoded presses appear as lines like `serial=0595a886 button=on action=press ...`.

## Pair it with Zigbee2MQTT

1. Put Zigbee2MQTT in pairing mode and power the board. It joins as a router, model `l-re.pico-bridge`.
2. Install the converter so Zigbee2MQTT knows its commands: publish the output of
   `python3 -I z2m/make_payload.py` to `zigbee2mqtt/bridge/request/converter/save` (or let
   [lutbridge-ha](https://github.com/davemhammer/lutbridge-ha) do it for you).
3. Optional but recommended: enable Zigbee2MQTT's availability feature so the device's online state is reported.

## Standalone use

Without Home Assistant the device is a Pico-to-Zigbee bridge you configure by hand. Because it boots armed and is
never claimed, it acts on every mapped press.

- **Map a Pico:** publish to `zigbee2mqtt/<device>/set`
  `{"slot": {"index": 0, "serial": "0595a886", "mask": 15}}`. The mask is the sum of on 1, off 2, raise 4, lower 8,
  favorite 16 (favorite is logged only). A slot index `i` is endpoint `10 + i`. Serial `00000000` clears the slot.
- **Bind it:** in Zigbee2MQTT, bind that endpoint's On/Off and Level Control clusters to a group (or a single
  device) in the usual way.
- **Quiet it** while something else handles presses: `{"claim": 1}` disarms every slot for 45 s;
  `{"claim": {"counter": 2, "slots": [0, 1]}}` disarms only those slots.

## Tests

```
make -C firmware/pico_bridge/test                                            # decoder and routing logic on the host, with ASan/UBSan
make -C firmware/pico_bridge/test vectors ORACLE=<dir with lutron_pico.py>   # regenerate decoder test vectors
```

`pico_decode.c` is a port of a reference Python decoder (`lutron_pico.py`). `test/gen_vectors.py` imports that
reference to produce the vectors, so there is no private vector set: captured bursts, published frame bodies, and
the exact event lines the reference prints for timed scenarios. The C tests decode each fixture in both bit
polarities, check that beacons stay unclassified, flip every bit of a burst (the CRC must never accept a different
frame), and compare event lines to the reference's. `pico_logic.c` (slot table, arming, press routing) is portable
C99 with its own host tests.

## Limits

- **Close range:** a Pico within about half a metre of the antenna loses the first burst of each transmission
  because the receiver overloads. The following copies still decode, so a press is caught about 12 ms late.
- **Capacity:** 24 slot entries, fixed at build time because Zigbee2MQTT caches the endpoint list.
- **Trust model:** the device accepts commands only from the coordinator's address (0x0000). There is no further
  authentication, and Clear Connect itself is unauthenticated, so anyone can forge a Pico.
- **No read-back:** Zigbee2MQTT never delivers this device's attribute reads, so the slot table cannot be read back
  through it; whoever provisions it must remember what it wrote.

## Layout

- `firmware/pico_bridge/main/`: `cc1101.c` (receive-only driver; the strobe enum has no `STX` and `PATABLE` is never
  written), `rx_pipeline.c` (carrier sense, RMT capture, decode), `pico_decode.c` (portable decoder and grouper),
  `pico_logic.c` (slot table, arming and press routing, host-tested), `bridge.c` (the Zigbee side).
- `firmware/pico_bridge/test/`: host tests and the vector generator.
- `z2m/`: the Zigbee2MQTT external converter and a helper that prints the install payload.
- `firmware/idf.sh`, `firmware/serial_log.py`: build and log helpers.

## Versions

Firmware and [lutbridge-ha](https://github.com/davemhammer/lutbridge-ha) are versioned independently and speak a
small, backward-compatible protocol (`docs/protocol.md`).

| Feature | Needs firmware | Needs lutbridge-ha |
|---|---|---|
| Slots, 1-byte claim (all or nothing) | 0.1 | 0.1 |
| Per-Pico claims (one broken automation affects one Pico) | 0.2 | 0.2 |

Plain claims work with every firmware. Older firmware rejects a claim that names slots (a ZCL `UNSUP_COMMAND`), so
with old firmware a controller that wants per-Pico behaviour gets none and the board stays armed for the Picos it
left out, which is the safe direction. Update the firmware first, then the integration.

## Future work

- **One radio or two.** The CC1101 is cheap, so a second one is an option. One radio cannot receive while it
  transmits, so any transmit feature (below) would blind the Pico decoder for the length of a send. A second,
  receive-only radio that is always listening would remove that conflict, at the cost of another module and four to
  six more GPIOs. Start with one radio and measure how often a send overlaps a Pico press before deciding.
- **Present Lutron dimmers as Zigbee dimmers.** Needs a gated transmit path (off by default, a compile-time switch,
  rate limited), the bridge subnet (bytes 3 to 5 of bridge frames, learnable from the air by the existing receiver),
  and a Level Control server endpoint per dimmer. Bridge-format set-level frames (dimmer serial, 16-bit level, fade)
  are already decoded, and a transmitter on another platform has been shown to work. Open: dimmer state feedback,
  and the one-time re-pair when the endpoint list changes. Transmitting on this band is regulated; keep it opt-in.
- **A small display** (an I2C or SPI LCD or OLED on spare GPIOs) showing armed state per slot, time since the last
  claim, slots in use, and a scrolling log of recent Pico presses (serial, button, and what was or would have been
  sent). It needs no new radio and no controller changes: the firmware already logs the same events on its serial
  console.
- Reading the slot table back through Zigbee2MQTT, if it can be made to deliver the device's attribute reports.
- A design for the favorite button.

## Notes from bring-up

- **Zigbee SDK:** written against `esp-zigbee-lib` 2.0.x (the `ezb_*` API). `ezb_config_memory` succeeds when called
  right after `esp_zigbee_init`.
- **Custom cluster id:** the manufacturer-specific range (0xFC00 and up) needs manufacturer-specific frames matched
  to a cluster manufacturer code; with 0xFF00 the stack answered every frame `UNSUPPORTED_CLUSTER`. The cluster is
  0xEF00 with plain ZCL frames.
- **Basic model string:** `l-re.pico-bridge` is 16 characters, so its ZCL length byte is `0x10`; the firmware
  computes it.
- **Zigbee2MQTT** interviews the 25-endpoint device fine. Send commands with `disableDefaultResponse`; without it
  each command waits about 10 s for a default response.
- **Carrier sense (GDO2) chatters** on noise with a relative threshold. The firmware uses an absolute threshold
  (`AGCCTRL1 = 0x07`) plus a 150 us qualifier. Carrier sense also drops mid-burst, so it cannot mark the end of a
  burst: the capture window is a fixed 7 ms from the first qualified rise (a burst is about 5.7 ms), then the radio
  is idled so GDO0 goes quiet and the RMT idle timeout (0.5 ms) closes the capture, and RX is restored before the
  next copy 12 ms later.
- **Transmission pattern:** a Pico sends three bursts per 75 ms slot, 12 ms apart, with sequence N, N+1, N+2. It
  transmits press bursts for only about 0.9 s of a hold and then stays silent until release, so the end of the
  bursts is not the end of the press. Dimming therefore sends one Move on the first press frame and one Stop on the
  first release frame; a first attempt that stopped when the bursts ended dimmed only a fifth of the range.
- **Close-range overload:** freezing the offset and clock loops until carrier sense (`FOCCFG` bit `FOC_BS_CS_GATE`)
  made no difference. If it ever matters, reduce `MAX_LNA_GAIN` in `AGCCTRL2`, at the cost of range.
- **Radio settings** in `cc1101.c`: 62.5 kBaud, 2-FSK, about 41 kHz deviation, 433.592 MHz. They decode reliably.

## License

Copyright 2026 Dave Hammer. Licensed under the [Apache License, Version 2.0](LICENSE); see [NOTICE](NOTICE) for
acknowledgements. Every source file carries an `SPDX-License-Identifier` header. It builds on ESP-IDF and
`espressif/esp-zigbee-lib`, both Apache-2.0, which are downloaded at build time and not included here.

Lutron, Caseta, Pico and Clear Connect are trademarks of Lutron Electronics Co., Inc.; Zigbee is a trademark of the
Connectivity Standards Alliance. This project is independent and is not affiliated with, endorsed by, or supported by
Lutron, the Connectivity Standards Alliance, Espressif, or Zigbee2MQTT. The names are used only to say what it works
with. The device is receive-only and does not transmit on any Lutron frequency. Use at your own risk: it switches
lights and is meant as a fallback, not as a safety system.
