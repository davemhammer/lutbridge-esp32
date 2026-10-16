#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Dave Hammer
# Run idf.py for ESP-IDF v6.0.2 installed by EIM, without sourcing the EIM
# activation script (it runs `eim select`, which opens the EIM GUI).
#
#   firmware/idf.sh -C firmware/pico_bridge build
#   firmware/idf.sh -C firmware/pico_bridge -p /dev/ttyACM0 flash monitor
set -euo pipefail

T="$HOME/.espressif/tools"
export IDF_TOOLS_PATH="$T"
export IDF_PATH="$HOME/.espressif/v6.0.2/esp-idf"
export ESP_IDF_VERSION="6.0"
export IDF_PYTHON_ENV_PATH="$T/python/v6.0.2/venv"
export ESP_ROM_ELF_DIR="$T/esp-rom-elfs/20241011/"
export OPENOCD_SCRIPTS="$T/openocd-esp32/v0.12.0-esp32-20260424/openocd-esp32/share/openocd/scripts"
# Use the public component registry, not the offline archive EIM points at.
unset IDF_COMPONENT_LOCAL_STORAGE_URL

export PATH="$IDF_PYTHON_ENV_PATH/bin:\
$T/ninja/1.12.1:\
$T/riscv32-esp-elf/esp-15.2.0_20251204/riscv32-esp-elf/bin:\
$T/openocd-esp32/v0.12.0-esp32-20260424/openocd-esp32/bin:\
$T/riscv32-esp-elf-gdb/17.1_20260402/riscv32-esp-elf-gdb/bin:\
$PATH"

exec python "$IDF_PATH/tools/idf.py" "$@"
