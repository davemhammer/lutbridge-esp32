# Protocol and design

What the device does and how to talk to it. The Home Assistant integration
([lutbridge-ha](https://github.com/davemhammer/lutbridge-ha)) is one client of this; anything that can publish to
Zigbee2MQTT can be another.

## Goals and constraints

- A Pico press must still work when Home Assistant, the Lutron bridge or the Zigbee coordinator is down.
- The radio is receive-only. Nothing in the tree can transmit on the Lutron band.
- The device must never fight another controller: it acts only on Picos nobody else is claiming.
- It is a router on the existing Zigbee network and uses binding address mode, so the coordinator itself may be
  down while the mesh still works.

## Radio and decode

A CC1101 in asynchronous serial receive mode feeds the demodulated bit stream (GDO0) into the RMT peripheral;
carrier sense (GDO2) marks the start of a burst. Carrier sense chatters and drops mid-burst, so a capture is a fixed
window after the first qualified rise, after which the radio is idled and then restored for the next copy. The
decoder (`pico_decode.c`) is portable C, host-tested against vectors generated from a reference Python decoder, and
a grouper merges the repeated copies of a transmission into one press, hold or release event.

A Pico sends three bursts per 75 ms slot, 12 ms apart. It transmits press bursts for only about 0.9 s of a hold,
so a release is the only reliable end of a press.

## Slot table: one endpoint per Pico by default, configurable

A fixed pool of client endpoints, each with On/Off and Level Control client clusters, each bound by Zigbee2MQTT to a
group. Each endpoint has one slot entry `(serial u32, button_mask u8)`. A press goes to the first entry whose serial
matches and whose mask contains the button.

- Default: one entry per Pico with every button in the mask, so one endpoint and one group per Pico.
- Split: a Pico whose buttons go to different groups takes two entries with disjoint masks. The device refuses a
  write that would make two slots claim the same button of one serial.
- Pool: 24 endpoints (ids 10 to 33); endpoint 1 is the command and status endpoint. The pool is fixed at build time
  because Zigbee2MQTT caches the endpoint list from the interview.
- Mask bits: on 0x01, off 0x02, raise 0x04, lower 0x08, favorite 0x10.
- Persisted in NVS (`pico_map`) with a version byte. Arm state is never persisted; boot is armed.

## Commands on endpoint 1, custom cluster 0xEF00 (plain ZCL)

| id | name | payload | effect |
|---|---|---|---|
| 0x00 | Claim | counter u8 | disarm every slot for 45 s |
| 0x01 | SetSlot | index u8, serial u32 big-endian, mask u8 | write one slot; serial 0 clears it |
| 0x02 | ClaimSlots | counter u8, handled-slots u32 big-endian (bit i = slot i) | disarm only the named slots for 45 s (firmware 0.2 and later) |

Commands are accepted only from short address 0x0000 (the coordinator); that is the whole authentication. The
cluster id is below the manufacturer-specific range (0xFC00 and up): a cluster in that range needs
manufacturer-specific frames matched to a cluster manufacturer code, and with 0xFF00 the stack answered every
frame with `UNSUPPORTED_CLUSTER`. Read-only attributes mirror state: `armed` (true while any slot in use is
armed), `claim` (last accepted counter), and the slots. One SetSlot per changed slot keeps every command well
under one APS frame, so there is no fragmentation and a partial sync is just fewer slots written.

Through Zigbee2MQTT, with the bundled converter (`z2m/pico_bridge.mjs`), these are `set` payloads on
`<base>/<device>/set`:

```
{"claim": 7}                                              every slot
{"claim": {"counter": 7, "slots": [0, 1, 2]}}             only those slots
{"slot": {"index": 3, "serial": "0595a886", "mask": 31}}  write a slot (serial "00000000" clears it)
```

## What a press does (armed)

| button | command from the slot's endpoint |
|---|---|
| on / off | On / Off |
| raise / lower | Move up / down with On/Off, rate 64, on the press frame; Stop with On/Off on the release frame |
| favorite | logged only |

Disarmed, the same decisions are logged as "would-have-sent" and nothing is transmitted on Zigbee. A Stop for a dim
that is already running is sent even when disarmed, because leaving a bulb dimming is worse than one extra Stop.

## Arming

Arm state is **per slot**. Boot is armed for every slot. A claim sets a 45 s deadline for each slot it names; slots
it leaves out keep whatever deadline they already had, so a slot that stops being claimed arms 45 s after its last
claim while the others stay disarmed, and a later claim that includes it disarms it again. A plain Claim
names every slot, which keeps older controllers working. A claim that covers a slot with a dim running makes the
device send the Stop for exactly that slot.

A controller therefore sends one claim every 15 s (three missed beats arm the slot) and one immediately when it sees
the device re-announce after a reboot. Which slots to name is the controller's decision: lutbridge-ha leaves out
the Picos whose Home Assistant automations are missing or switched off.

## Compatibility

Changes to this protocol are additive and use new command ids rather than reshaping old ones. A controller that
only sends Claim (0x00) works with every firmware version. ClaimSlots (0x02) is new in 0.2; 0.1 firmware answers it
with a ZCL default response of UNSUP_COMMAND and does not disarm anything, so a controller that sees that error
should fall back to plain Claim or tell its owner to update the firmware.
