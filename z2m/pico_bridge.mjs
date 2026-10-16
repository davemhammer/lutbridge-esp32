// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Dave Hammer
// Zigbee2MQTT external converter for the Lutron Pico fallback bridge (ESP32-C6, model l-re.pico-bridge).
// Source of truth lives in lutbridge-esp32; the Home Assistant integration (lutbridge-ha) bundles an identical copy.
//
// Carries the two commands and the read-only mirrors on endpoint 1, custom cluster 0xEF00
// (see docs/protocol.md):
//   set {"claim": <0-255>}                                  Claim: disarm the fallback for 45 s, for every slot (any firmware)
//   set {"claim": {"counter": 7, "slots": [0, 1, 2]}}       Claim only those slots; the others keep arming on their own
//                                                           (firmware 0.2 and later)
//   set {"slot": {"index": 3, "serial": "0595a886", "mask": 31}}   write one slot (serial all zeros clears it)
//   get {"slots": ""}                                       read every slot back
//   state: armed (bool), claim (last accepted counter), slot_0 .. slot_23 as "<serial hex>:<mask hex>"
//
// The device is a command source: its per-Pico endpoints (10..33) are bound to Zigbee groups with the
// normal Zigbee2MQTT bind API and need nothing from this file.

import {Zcl} from 'zigbee-herdsman';
import * as exposes from 'zigbee-herdsman-converters/lib/exposes';
import * as m from 'zigbee-herdsman-converters/lib/modernExtend';

const e = exposes.presets;
const ea = exposes.access;

const SLOTS = 24;
// The device does not send ZCL default responses for these commands, so do not wait for one.
const OPTS = {disableDefaultResponse: true};

const slotAttributes = {};
for (let i = 0; i < SLOTS; i++) {
    slotAttributes[`slot${i}`] = {ID: 0x0100 + i, type: Zcl.DataType.OCTET_STR};
}

const picoBridgeDefinition = {
    ID: 0xef00,
    attributes: {
        claim: {ID: 0x0000, type: Zcl.DataType.UINT8},
        armed: {ID: 0x0001, type: Zcl.DataType.BOOLEAN},
        ...slotAttributes,
    },
    commands: {
        // The original claim: one byte, every slot. Firmware of any version accepts it.
        claim: {ID: 0x00, parameters: [{name: 'counter', type: Zcl.DataType.UINT8}]},
        // Claim only some slots: the counter, then the slots as a 32-bit mask, most significant byte first (bit i = slot i).
        // A separate command (0x02) that only firmware 0.2 and later knows; older firmware answers UNSUP_COMMAND.
        claimSlots: {
            ID: 0x02,
            parameters: [
                {name: 'counter', type: Zcl.DataType.UINT8},
                {name: 'h3', type: Zcl.DataType.UINT8},
                {name: 'h2', type: Zcl.DataType.UINT8},
                {name: 'h1', type: Zcl.DataType.UINT8},
                {name: 'h0', type: Zcl.DataType.UINT8},
            ],
        },
        // Serial is sent as four bytes, most significant first, so there is no byte-order guesswork.
        setSlot: {
            ID: 0x01,
            parameters: [
                {name: 'index', type: Zcl.DataType.UINT8},
                {name: 's3', type: Zcl.DataType.UINT8},
                {name: 's2', type: Zcl.DataType.UINT8},
                {name: 's1', type: Zcl.DataType.UINT8},
                {name: 's0', type: Zcl.DataType.UINT8},
                {name: 'mask', type: Zcl.DataType.UINT8},
            ],
        },
    },
    commandsResponse: {},
};

const picoBridgeCluster = m.deviceAddCustomCluster('picoBridge', picoBridgeDefinition);

// Zigbee2MQTT registers a device's custom clusters when it starts the device and does not do so again when this file
// is reloaded, so a command added by a newer converter is unknown ('UNSUP_COMMAND') until Zigbee2MQTT restarts.
// Registering the definition again before each use makes an updated converter take effect at once.
const ensureCluster = (device) => device.addCustomCluster('picoBridge', picoBridgeDefinition);

function hex(bytes) {
    return Array.from(bytes, (b) => b.toString(16).padStart(2, '0')).join('');
}

const fzPicoBridge = {
    cluster: 'picoBridge',
    type: ['attributeReport', 'readResponse'],
    convert: (model, msg, publish, options, meta) => {
        const out = {};
        const d = msg.data;
        if (d.armed !== undefined) out.armed = Boolean(d.armed);
        if (d.claim !== undefined) out.claim = d.claim;
        for (let i = 0; i < SLOTS; i++) {
            const v = d[`slot${i}`];
            if (v === undefined) continue;
            const b = Array.from(v);
            if (b.length >= 5) out[`slot_${i}`] = `${hex(b.slice(0, 4))}:${b[4].toString(16).padStart(2, '0')}`;
        }
        return out;
    },
};

const tzPicoBridge = [
    {
        key: ['claim'],
        convertSet: async (entity, key, value, meta) => {
            // A bare number, or an object without a slot list, claims every slot with the one-byte command that
            // every firmware version understands. {counter, slots: [..]} claims just those slots.
            const counter = (typeof value === 'object' ? Number(value.counter) : Number(value)) & 0xff;
            ensureCluster(meta.device);
            const ep = meta.device.getEndpoint(1);
            if (typeof value !== 'object' || !Array.isArray(value.slots)) {
                await ep.command('picoBridge', 'claim', {counter}, OPTS);
                return {state: {}};
            }
            let mask = 0;
            for (const slot of value.slots) {
                const i = Number(slot);
                if (!Number.isInteger(i) || i < 0 || i >= SLOTS) throw new Error(`bad slot '${slot}'`);
                mask |= 1 << i;
            }
            const [h3, h2, h1, h0] = [mask >>> 24, (mask >>> 16) & 0xff, (mask >>> 8) & 0xff, mask & 0xff];
            await ep.command('picoBridge', 'claimSlots', {counter, h3, h2, h1, h0}, OPTS);
            return {state: {}}; // the device mirrors the accepted counter; read it back with a get if needed
        },
    },
    {
        key: ['slot'],
        convertSet: async (entity, key, value, meta) => {
            const index = Number(value.index);
            const serial = String(value.serial).replace(/^0x/, '').padStart(8, '0');
            if (!/^[0-9a-fA-F]{8}$/.test(serial)) throw new Error(`bad serial '${value.serial}'`);
            const mask = Number(value.mask) & 0xff;
            const b = Buffer.from(serial, 'hex');
            ensureCluster(meta.device);
            await meta.device
                .getEndpoint(1)
                .command('picoBridge', 'setSlot', {index, s3: b[0], s2: b[1], s1: b[2], s0: b[3], mask}, OPTS);
            return {state: {}};
        },
    },
    {
        key: ['slots', 'armed'],
        convertGet: async (entity, key, meta) => {
            ensureCluster(meta.device);
            const ep = meta.device.getEndpoint(1);
            if (key === 'armed') {
                await ep.read('picoBridge', ['armed', 'claim'], OPTS);
                return;
            }
            // A Read Attributes response must fit one APS frame, so read the slots a few at a time.
            for (let i = 0; i < SLOTS; i += 4) {
                const names = [];
                for (let j = i; j < Math.min(i + 4, SLOTS); j++) names.push(`slot${j}`);
                await ep.read('picoBridge', names, OPTS);
            }
            await ep.read('picoBridge', ['armed', 'claim'], OPTS);
        },
    },
];

const definition = {
    zigbeeModel: ['l-re.pico-bridge'],
    model: 'l-re.pico-bridge',
    vendor: 'l-re',
    description: 'Lutron Pico to Zigbee fallback bridge (ESP32-C6 + CC1101, receive only)',
    extend: [picoBridgeCluster],
    fromZigbee: [fzPicoBridge],
    toZigbee: tzPicoBridge,
    exposes: [
        e.binary('armed', ea.STATE_GET, true, false).withDescription('true when some Pico is not being claimed (no heartbeat for 45 s)'),
        e.numeric('claim', ea.STATE).withDescription('last accepted claim counter'),
        ...Array.from({length: SLOTS}, (_, i) => e.text(`slot_${i}`, ea.STATE).withDescription(`slot ${i}: serial:mask`)),
    ],
};

export default definition;
