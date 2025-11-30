# Features

- **Keyless ignition control** (GPIO26 relay; active-high configurable).
- **Secure BLE pairing** with a **one-time 6‑digit passkey** (MITM + LE Secure Connections).
- **Bonded-only reconnection** outside a 3‑minute pairing window.
- **Web GUI (HTTP)** with Basic Auth: Dashboard, Module Settings, Bluetooth Keys, Logout.
- **Offline firmware upgrade** via `.bin` upload (**config preserved**).
- **Battery voltage sensing** (ADC34, 100k/10k divider; adjustable constants).
- **Uptime & timekeeping**, browser time sync, persisted epoch in NVS.
- **Editable Module Name**, AP SSID/password, and admin credentials.
- **Reboot** and **Factory Reset** (settings wipe) from GUI.
