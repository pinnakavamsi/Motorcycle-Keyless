# Security Model

- **Pairing Window**: 3 minutes with a **random 6‑digit passkey** displayed in the GUI. Outside the window, unbonded connections are rejected.
- **Encrypted BLE**: AES‑CCM via BLE **Secure Connections** with **MITM** protection (passkey entry).
- **Web GUI**: HTTP Basic Auth (transport is local AP). Plan to upgrade to HTTPS with device‑specific certs.
- **Fail-safe Defaults**: Relay OFF at boot/brown‑out; any update or reset sequences end with ignition disabled.
- **Persistence**: Preferences in NVS (namespaces: `skcfg`, `btkeys`, `timecfg`). Factory reset wipes them all.
