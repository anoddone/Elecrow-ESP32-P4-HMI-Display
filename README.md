# Elecrow ESP32-P4 HMI Display

PlatformIO project for the [Elecrow CrowPanel Advanced 7" ESP32-P4 HMI Display](https://github.com/Elecrow-RD/CrowPanel-Advanced-7inch-ESP32-P4-HMI-AI-Display-1024x600-IPS-Touch-Screen)
(1024x600 IPS via MIPI-DSI, GT911 capacitive touch, ESP32-P4 dual-core RISC-V @ 400 MHz).

## Hardware

- **SoC**: ESP32-P4, HP core dual-core RISC-V @ 400 MHz, LP core @ 40 MHz
- **Memory**: 768 KB L2MEM (HP), 32 KB SRAM (LP), 8 KB TCM, 32 MB external PSRAM, 16 MB flash
- **Display**: 7" 1024x600 IPS panel over MIPI-DSI, EK79007 controller
- **Touch**: GT911 capacitive, I2C (SCL=IO46, SDA=IO45), single/5-point
- No onboard WiFi/BT radio on the P4 itself — pairs with a companion module (see vendor repo for the C6 firmware-upgrade path) and has an optional LoRa (SX1262) / nRF24 socket.

Pin definitions live in [include/board_config.h](include/board_config.h).

## Software stack

Arduino framework via the [pioarduino](https://github.com/pioarduino/platform-espressif32) fork of `platform-espressif32`
(needed because ESP32-P4 support is too new for the official PlatformIO registry build). Display/touch bring-up uses
Espressif's own `ESP32_Display_Panel` + `ESP32_IO_Expander` + `esp-lib-utils` stack with LVGL 8 (LVGL 9 isn't supported by
the Arduino wrapper yet), pinned to the same versions the vendor's official Arduino examples ship.

Config headers (`esp_panel_drivers_conf.h`, `esp_panel_board_custom_conf.h`, `esp_utils_conf.h`, `lv_conf.h`) and the
`lvgl_v8_port` LVGL integration layer were carried over from the vendor's `Lesson07-Turn_on_the_screen` example and
adapted to this project's `board_config.h`.

## Build / flash

```
~/.platformio/penv/bin/pio run -e elecrow_esp32p4
~/.platformio/penv/bin/pio run -e elecrow_esp32p4 -t upload
~/.platformio/penv/bin/pio device monitor -b 115200
```

`pio` isn't on PATH in this environment — use the full path above.

## Exploration roadmap

This starter sketch ([src/main.cpp](src/main.cpp)) brings up the panel, shows a label, and logs chip/flash/heap/PSRAM
stats over Serial as a baseline. Ideas to build on from here:

- **Graphics**: push LVGL further (animations, custom widgets, image/font assets from PSRAM), or drop to the raw
  `esp_panel`/`esp_lcd` DSI framebuffer for direct pixel work; profile frame rate at 1024x600.
- **Expanded memory**: allocate large buffers with `heap_caps_malloc(..., MALLOC_CAP_SPIRAM)`, benchmark PSRAM vs
  internal-RAM bandwidth, try double/triple framebuffering in PSRAM (`LVGL_PORT_AVOID_TEARING_MODE` in
  `lvgl_v8_port.h` already scaffolds multi-buffer support).
- **MMU**: ESP32-P4 exposes the ESP-IDF MMU-map APIs (`esp_mmu_map.h`) for mapping flash/PSRAM into the CPU address
  space on demand — useful for streaming large assets (fonts, images) without holding them all in RAM at once.
