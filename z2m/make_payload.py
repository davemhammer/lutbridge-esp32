#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Dave Hammer
"""Print what is needed to install pico_bridge.mjs through Zigbee2MQTT's converter API.

    python3 -I z2m/make_payload.py            # the MQTT payload (JSON text)
    python3 -I z2m/make_payload.py --service  # the same wrapped as Home Assistant mqtt.publish service data (Developer tools, Actions)
Publish to zigbee2mqtt/bridge/request/converter/save.
"""
import json
import pathlib
import sys

here = pathlib.Path(__file__).resolve().parent
payload = json.dumps({"name": "pico_bridge.mjs", "code": (here / "pico_bridge.mjs").read_text()})
if "--service" in sys.argv:
    print(json.dumps({"topic": "zigbee2mqtt/bridge/request/converter/save", "payload": payload}))
else:
    print(payload)
