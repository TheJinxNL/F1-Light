# ESP32-C6 SuperMini Migration Notes

## platformio.ini changes
- `platform` → `https://github.com/pioarduino/platform-espressif32/releases/download/stable/platform-espressif32.zip` (registry espressif32 6.x/7.x lacks C6 Arduino support)
- `board` → custom `boards/esp32c6supermini.json` (no official PlatformIO board supports arduino framework for C6)
- `board_build.f_cpu` → `160000000L` (C6 max is 160 MHz, not 240)
- Removed `platform_packages` Xtensa toolchain block (RISC-V, auto-selected)
- Added build flags: `-DARDUINO_USB_MODE=1 -DARDUINO_USB_CDC_ON_BOOT=1` (routes Serial to USB Serial/JTAG)
- Updated `-I` paths: `WiFiClientSecure/src` → `NetworkClientSecure/src`, added `Network/src` (Arduino ESP32 3.x restructure)

## boards/esp32c6supermini.json (created)
- Custom board JSON required because all official C6 board defs only list `espidf` framework
- `mcu: esp32c6`, `variant: esp32c6`, `ldscript: esp32c6_out.ld`, `frameworks: [arduino, espidf]`
- Flash: 4MB, RAM: 320KB, f_cpu: 160 MHz

## display.cpp changes
- `SPIClass g_spi(HSPI)` → `SPIClass g_spi(FSPI)` (HSPI not defined on C6; FSPI = SPI2)
- `ledcSetup(ch, freq, bits)` + `ledcAttachPin(pin, ch)` → `ledcAttach(pin, freq, bits)` (Arduino ESP32 3.x new API)
- All `ledcWrite(channel, val)` → `ledcWrite(pin, val)`
- Removed `TFT_BL_PWM_CHANNEL` constant (no longer needed)

## config.h GPIO changes
- `TFT_BL`: 13 → 2 (GPIO 13 = USB D+ on ESP32-C6, must not be used)
- `LED_PIN`: 18 → 8 (onboard WS2812B RGB LED on GPIO 8; original external strip was GPIO 18)

## ESP32-C6 SuperMini pin warnings
- GPIO 12 = USB D−, GPIO 13 = USB D+ → never drive these (breaks serial monitor)
- GPIO 8 = onboard WS2812B RGB LED
- GPIO 14/15 = JTAG TMS/TDO (usable as GPIO, but avoid if hardware debugging)
- GPIO 0 = strapping/boot button (safe for WiFi reset as on original ESP32)
- All other project pins (23, 27, 4, 0) are fine on C6
