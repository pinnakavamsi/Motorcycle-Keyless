# Setup & Build

## Hardware (reference)
- ESP32 DevKit (ESP32-WROOM/ESP32-WROVER).
- 12V→5V buck converter (automotive-tolerant).
- Relay (automotive), N-MOSFET low-side driver, flyback diode (e.g., 1N5819).
- Voltage divider: R1=100k, R2=10k → ADC34.
- All grounds common; fuse on battery feed; optional TVS/LC filter.

## Arduino IDE
1. **Install ESP32 core** (Boards Manager → `esp32` by Espressif; 2.x+ recommended).
2. **Libraries** (Library Manager):
   - `NimBLE-Arduino`
3. **Board**: ESP32 Dev Module (or your specific ESP32 board).
4. **Flash Erase** (optional first time): `Tools → Erase Flash → All Flash Contents`.
5. Open `firmware/sketch_MotorcycleKeyless.ino` and **Upload**.

## Default Access
- AP SSID: `SK-ESP32`
- AP password: `admin12345`
- Admin GUI: `http://192.168.23.1/`
- Login: `admin` / `admin` (change on first use)

## Offline Firmware Upgrade
- Arduino IDE: `Sketch → Export Compiled Binary` to produce `.bin`.
- Open GUI → **Module Settings** → **Firmware Upgrade (offline)** → select `.bin` and flash.
- Device reboots, **settings are preserved**.
