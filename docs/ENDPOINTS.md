# HTTP Endpoints & BLE Profile

## HTTP Endpoints
- `GET /` — Dashboard
- `GET /settings` — Module settings
- `GET /bt` — Bluetooth Keys
- `GET /logout` — Force re-auth (Basic Auth)
- `POST /toggle_ign` — Toggle ignition relay
- `POST /set_name` — Set module name
- `POST /set_admin` — Set admin credentials
- `POST /set_ap` — Set SSID/password
- `POST /set_time` — Set epoch (browser sync)
- `POST /pair_start` — Open 3‑min pairing window
- `POST /pair_cancel` — Cancel pairing
- `POST /btupdate` — Update key name/type
- `POST /btdelete` — Delete key from list
- `POST /forget_bond` — Delete BLE bond in stack
- `POST /fw_upload` — Upload & flash `.bin`
- `POST /reboot_module` — Reboot device
- `POST /factory_reset` — Wipe prefs + reboot

## BLE GATT (UART-like)
- **Service**: `0xFFE0`
- **RX (phone→module)**: `0xFFE1` — WRITE/WRITE_NR **(encrypted)**  
  - Commands: `LOCK`, `UNLOCK`, or epoch seconds for time sync
- **TX (module→phone)**: `0xFFE2` — READ/NOTIFY **(encrypted)**

**Security**: Bonding + MITM + LE Secure Connections; IO cap DISPLAY_ONLY with a one-time 6‑digit passkey during pairing.
