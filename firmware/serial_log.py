#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Dave Hammer
"""Reset the board over USB serial/JTAG and print its log for N seconds.

    firmware/idf.sh is not needed; run with the IDF venv python (has pyserial):
    ~/.espressif/tools/python/v6.0.2/venv/bin/python firmware/serial_log.py [seconds] [port] [--no-reset]
"""
import sys
import time

import serial


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    seconds = float(args[0]) if args else 15.0
    port = args[1] if len(args) > 1 else "/dev/ttyACM0"
    reset = "--no-reset" not in sys.argv

    s = serial.Serial(port, 115200, timeout=0.2)
    if reset:
        # USB serial/JTAG: RTS drives EN, DTR drives GPIO9 (BOOT). Keep DTR high
        # (BOOT not pulled low) so the chip runs the app instead of download mode.
        s.dtr = False
        s.rts = True
        time.sleep(0.1)
        s.rts = False
    end = time.time() + seconds
    while time.time() < end:
        data = s.read(4096)
        if data:
            sys.stdout.write(data.decode("utf-8", "replace"))
            sys.stdout.flush()


if __name__ == "__main__":
    main()
