# Motorcycle Keyless Project (ESP32)

A secure, maintainable, and practical **keyless ignition** system for motorcycles using **ESP32**.

- **Author:** Vamsi Pinnaka
- **License:** MIT
- **Version:** 0.1.0
- **Date:** 2025-11-30

## Overview
This project provides an ESP32-based module that controls a motorcycle ignition relay, with a local Wi‑Fi admin GUI and secure
Bluetooth pairing using a one‑time passkey. Configuration is persisted in ESP32 NVS. Firmware upgrades are done offline via `.bin`
upload and **do not wipe settings**.

## Quick Start
1. Flash the sketch in `firmware/sketch_MotorcycleKeyless.ino` to an ESP32.
2. Connect to AP **SK-ESP32** (default password `admin12345`).
3. Open **http://192.168.23.1** and log in with **admin/admin**.
4. Change credentials in **Module Settings** and set your module name.
5. Use **Bluetooth Keys** to start pairing (3‑minute window shows a 6‑digit PIN).

## Documentation
- `docs/FEATURES.md` – full feature list
- `docs/SETUP.md` – build, wiring, defaults, and upgrade workflow
- `docs/BACKGROUND.md` – motivation & problem statement
- `docs/ENDPOINTS.md` – HTTP endpoints and BLE GATT profile
- `docs/SECURITY.md` – security model and threat mitigations
- `docs/SCHEMATIC.pdf` – wiring reference (optional future add)

## Roadmap
- HTTPS for the GUI, device certificates
- Signed updates, A/B partitions with rollback
- Low-power modes and presence detection options (UWB/NFC/Apple Watch-only)

---

> If you use this in a real motorcycle, **test thoroughly** on the bench, add appropriate fusing and protection,
> and consider a mechanical backup. Safety first.
