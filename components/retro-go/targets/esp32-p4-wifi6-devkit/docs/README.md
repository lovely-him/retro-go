# ESP32-P4-WIFI6-DEVKIT
- Status: Work in progress (boots and uses the SD card, display / audio / input not implemented yet)
- Ref: https://www.espressif.com/en/products/devkits?id=ESP32-P4

# Hardware info
- SoC: ESP32-P4, dual-core RISC-V at 400MHz (chip revision v3.2)
- Flash: 16MB WinBond, 80MHz QIO
- PSRAM: 32MB in-package, HEX mode, 16-bit, 200MHz, with `.text` and `.rodata` XIP from PSRAM
- Storage: microSD on SDMMC slot 0, 4-bit bus at 20MHz (VDD supplied by on-chip LDO channel 4)
- Display: MIPI-DSI, no driver written yet (falls back to the dummy driver)
- Audio: ES8311 codec over I2S, no driver written yet (falls back to the dummy sink)
- Networking: the ESP32-P4 itself has no WiFi radio, build with `--no-networking`
- Boot memory baseline: 401/497 KiB internal RAM and 32280/32317 KiB PSRAM free

# Known issues:
- The display driver is not written yet, the screen stays black.
- Audio uses the dummy sink, there is no sound.
- Gamepad and touch mappings are not wired up yet, the device cannot be controlled.
- `W ldo: The voltage value 0 is out of the recommended range [500, 2700]` is printed at
  SD card init. It comes from IDF internals (`sd_pwr_ctrl_ldo_config_t` has no voltage
  field, so the value is always 0) and is harmless: the card enumerates, switches to a
  4-bit bus and reads files normally.

# Images
<!-- TODO: add a photo of the board as device.jpg -->
