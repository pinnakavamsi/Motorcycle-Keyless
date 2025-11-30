
/*
  Motorcycle Keyless – ESP32 (HTTP only) + Offline Firmware Upgrade
  - Web GUI (Basic Auth + Logout in sidebar)
  - Ignition relay control
  - Voltage reading
  - Uptime + browser time sync
  - Admin + AP Wi-Fi settings (Show/Hide password)
  - Reboot + Factory Reset (bottom of Settings)
  - BLE pairing (3-minute window, 6-digit PIN; bonded-only otherwise)
  - NEW: Offline Firmware Upgrade (.bin) via /fw_upload using Update.h (config preserved)
*/

#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include "esp_wifi.h"
#include <time.h>
#include <sys/time.h>
#include <NimBLEDevice.h>
#include <Update.h>   // <-- for offline firmware flashing

// ====== USER I/O ======
const int  RELAY_PIN = 26;            // change to your relay pin
const bool RELAY_ACTIVE_HIGH = true;  // true: HIGH energizes relay

const int VOLTAGE_ADC_PIN = 34;       // ADC-capable pin (32–39)

// Voltage divider + ADC fudge (tune for your hardware)
const float R1 = 100000.0f;      // upper resistor (ohms)
const float R2 = 10000.0f;       // lower resistor (ohms)
const float ADC_REF_V = 1100.0f; // mV nominal ref
const int   ADC_MAX   = 4095;    // 12-bit
const float ADC_ATTEN_GAIN = 2.0f;

// ====== AP / Network ======
const int   AP_CH   = 6;
IPAddress   apIP(192,168,23,1), apGW(192,168,23,1), apMask(255,255,255,0);

// ====== Globals ======
Preferences prefs;
WebServer server(80);

String moduleName = "Scout Keyless";
String adminUser  = "admin";
String adminPass  = "admin";
String apSsid     = "SK-ESP32";
String apKey      = "admin12345";

bool needsPassChange = false;
bool ignitionOn = false;

// ====== Time persist ======
static const char* NS_TIME = "timecfg";
void setEpochPersist(int64_t sec, const char* srcTag = nullptr) {
  if (sec <= 0) return;
  struct timeval tv; tv.tv_sec = (time_t)sec; tv.tv_usec = 0;
  settimeofday(&tv, nullptr);
  prefs.begin(NS_TIME, false);
  prefs.putLong64("epoch", sec);
  if (srcTag) prefs.putString("src", srcTag);
  prefs.end();
}
void loadSavedTime() {
  prefs.begin(NS_TIME, true);
  int64_t saved = prefs.getLong64("epoch", 0);
  prefs.end();
  if (saved > 0) {
    struct timeval tv; tv.tv_sec = (time_t)saved; tv.tv_usec = 0;
    settimeofday(&tv, nullptr);
  }
}

// ====== Stored Keys (names/types for bonded devices) ======
struct BtKey { String name; String type; String addr; };
#define MAX_BT_KEYS 16
BtKey btKeys[MAX_BT_KEYS];
int   btKeyCount = 0;

// ====== BLE (UART-like service for apps) ======
static NimBLEServer*         gBleServer = nullptr;
static NimBLEAdvertising*    gBleAdv    = nullptr;
static NimBLEService*        gBdbSvc    = nullptr;
static NimBLECharacteristic* gBdbRx     = nullptr;  // phone writes (FFE1)
static NimBLECharacteristic* gBdbTx     = nullptr;  // we notify/read (FFE2)
static bool                  gBleInited = false;

// Forward declare
void bleStartAdvertising();

// HM-10 style UUIDs
static const NimBLEUUID BDB_UART_SVC((uint16_t)0xFFE0);
static const NimBLEUUID BDB_UART_RX ((uint16_t)0xFFE1);
static const NimBLEUUID BDB_UART_TX ((uint16_t)0xFFE2);

// ====== Pairing Window ======
static uint32_t pairingCode = 0;
static bool     pairingActive = false;
static unsigned long pairingUntil = 0;
static const unsigned long PAIR_WINDOW_MS = 180000UL; // 3 minutes

// ====== UI / HTML ======
const char* CSS_BASE =
"body{font-family:system-ui;margin:0;color:#111;background:#f7f7f9}"
".layout{display:flex;min-height:100vh}"
".sidebar{width:240px;background:#fff;border-right:1px solid #e5e7eb;padding:16px;position:sticky;top:0;height:100vh}"
".brand{font-weight:700;font-size:20px;margin-bottom:16px}"
".nav a{display:block;padding:10px 12px;margin:6px 0;border-radius:10px;text-decoration:none;color:#111}"
".nav a.active, .nav a:hover{background:#e8f0ff;color:#0b57d0}"
".content{flex:1;padding:20px}"
".card{background:#fff;border:1px solid #e5e7eb;border-radius:12px;padding:16px;margin-bottom:16px}"
"label{display:block;margin-top:10px}"
"input,select{width:100%;padding:8px;margin-top:4px;border:1px solid #ccc;border-radius:8px}"
"button{padding:10px 14px;border:none;border-radius:10px;background:#007BFF;color:#fff;font-weight:600;cursor:pointer}"
"button:hover{background:#0069d9}"
".btn-danger{background:#d32f2f} .btn-danger:hover{background:#b71c1c}"
".grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(240px,1fr));gap:12px}"
".list{width:100%;border-collapse:collapse}"
".list th,.list td{border-bottom:1px solid #eee;padding:8px;text-align:left;vertical-align:middle}"
".pill{display:inline-block;padding:2px 8px;border-radius:999px;background:#eef2ff;color:#1e3a8a;font-size:12px}"
".ok{background:#eaf8ee;border:1px solid #bfe6cb;padding:10px;border-radius:10px;margin:12px 0}"
".warn{background:#fff6d8;border:1px solid #f3c969;padding:10px;margin:12px 0}"
".err{background:#fde8e8;border:1px solid #f5b4b4;padding:10px;margin:12px 0}"
".muted{color:#6b7280;font-size:12px}"
".row{display:flex;gap:10px;align-items:center;flex-wrap:wrap}"
".spacer{height:8px}"
".code{font:700 36px ui-monospace,monospace;letter-spacing:4px;background:#0b57d0;color:#fff;padding:8px 12px;border-radius:12px}";

String htmlShell(const String& active, const String& pageTitle, const String& inner) {
  String h;
  h += "<!doctype html><html><head><meta name=viewport content='width=device-width, initial-scale=1'>";
  h += "<title>" + moduleName + " - " + pageTitle + "</title><style>";
  h += CSS_BASE;
  h += "</style></head><body><div class=layout>";
  h += "<div class=sidebar><div class=brand>🏍️ " + moduleName + "</div><div class=nav>";
  h += "<a href='/'"          + String(active=="/" ? " class=active" : "") + ">Dashboard</a>";
  h += "<a href='/settings'"  + String(active=="/settings" ? " class=active" : "") + ">Module Settings</a>";
  h += "<a href='/bt'"        + String(active=="/bt" ? " class=active" : "") + ">Bluetooth Keys</a>";
  h += "<a href='/logout' style='margin-top:16px;color:#b00020'>Logout</a>";  // in menu
  h += "</div><div class=muted>AP: 192.168.23.1</div></div>";
  h += "<div class=content>";
  // Silent time sync from browser
  h += "<script>(function(){try{var e=Math.floor(Date.now()/1000);fetch('/set_time',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'epoch='+e+'&src=browser'})}catch(_){}})();</script>";
  h += inner;
  h += "</div></div></body></html>";
  return h;
}

// ====== Prefs ======
void savePrefs() {
  prefs.begin("skcfg", false);
  prefs.putString("moduleName", moduleName);
  prefs.putString("adminUser", adminUser);
  prefs.putString("adminPass", adminPass);
  prefs.putString("apSsid", apSsid);
  prefs.putString("apKey", apKey);
  prefs.putBool("ign", ignitionOn);
  prefs.end();

  prefs.begin("btkeys", false);
  prefs.putInt("count", btKeyCount);
  for (int i=0;i<btKeyCount;i++) {
    String key = "k" + String(i);
    String val = btKeys[i].name + "|" + btKeys[i].type + "|" + btKeys[i].addr;
    prefs.putString(key.c_str(), val);
  }
  prefs.end();
}

void loadPrefs() {
  prefs.begin("skcfg", true);
  moduleName  = prefs.getString("moduleName", "Scout Keyless");
  adminUser   = prefs.getString("adminUser", "admin");
  adminPass   = prefs.getString("adminPass", "admin");
  apSsid      = prefs.getString("apSsid", "SK-ESP32");
  apKey       = prefs.getString("apKey", "admin12345");
  ignitionOn  = prefs.getBool("ign", false);
  prefs.end();
  needsPassChange = (adminUser == "admin" && adminPass == "admin");

  prefs.begin("btkeys", true);
  btKeyCount = prefs.getInt("count", 0);
  if (btKeyCount < 0 || btKeyCount > MAX_BT_KEYS) btKeyCount = 0;
  for (int i=0;i<btKeyCount;i++) {
    String key = "k" + String(i);
    String val = prefs.getString(key.c_str(), "");
    int p1 = val.indexOf('|');
    int p2 = (p1>=0) ? val.indexOf('|', p1+1) : -1;
    if (p1>0 && p2>p1) {
      btKeys[i].name = val.substring(0,p1);
      btKeys[i].type = val.substring(p1+1,p2);
      btKeys[i].addr = val.substring(p2+1);
    } else {
      btKeys[i].name = "Key"+String(i+1);
      btKeys[i].type = "Unknown";
      btKeys[i].addr = "";
    }
  }
  prefs.end();
}

void factoryReset() {
  prefs.begin("skcfg", false); prefs.clear(); prefs.end();
  prefs.begin("btkeys", false); prefs.clear(); prefs.end();
  prefs.begin(NS_TIME, false);  prefs.clear(); prefs.end();
  delay(100);
  ESP.restart();
}

// ====== Utils ======
bool requireAuth() {
  if (!server.authenticate(adminUser.c_str(), adminPass.c_str())) { server.requestAuthentication(); return false; }
  return true;
}
bool isAsciiSafe(const String& s) {
  for (size_t i=0;i<s.length();++i) { char c = s[i]; if (c < 32 || c > 126) return false; }
  return true;
}
float readBatteryVoltage() {
  int raw = analogRead(VOLTAGE_ADC_PIN);
  float mv  = (raw * (ADC_REF_V / ADC_MAX)) * ADC_ATTEN_GAIN;
  float vin = mv * ( (R1 + R2) / R2 ) / 1000.0f;
  return vin;
}
String fmtUptime(unsigned long ms) {
  unsigned long s = ms / 1000UL;
  unsigned int d = s / 86400UL; s %= 86400UL;
  unsigned int h = s / 3600UL;  s %= 3600UL;
  unsigned int m = s / 60UL;    s %= 60UL;
  char b[64];
  if (d) snprintf(b, sizeof(b), "%ud %uh %um %lus", d,h,m,(unsigned long)s);
  else if (h) snprintf(b, sizeof(b), "%uh %um %lus", h,m,(unsigned long)s);
  else if (m) snprintf(b, sizeof(b), "%um %lus", m,(unsigned long)s);
  else snprintf(b, sizeof(b), "%lus", (unsigned long)s);
  return String(b);
}
String fmtTime(time_t t) {
  if (t <= 0) return String("Not set");
  struct tm tmv; localtime_r(&t, &tmv);
  char b[48]; strftime(b, sizeof(b), "%Y-%m-%d %H:%M:%S", &tmv);
  return String(b);
}
String safeHtml(const String& s){ String o=s; o.replace("&","&amp;"); o.replace("<","&lt;"); o.replace(">","&gt;"); o.replace("\"","&quot;"); return o; }
int findKeyIndexByAddr(const String& addr){ for(int i=0;i<btKeyCount;i++) if(btKeys[i].addr==addr) return i; return -1; }

// ----- Ignition (relay) -----
void driveRelayPin(bool on) {
  int level = RELAY_ACTIVE_HIGH ? (on ? HIGH : LOW) : (on ? LOW : HIGH);
  digitalWrite(RELAY_PIN, level);
  Serial.printf("[IGNITION] pin=%d level=%d logical=%s\n", RELAY_PIN, level, on ? "ON" : "OFF");
}
void applyRelay() { driveRelayPin(ignitionOn); }

// ====== BLE: RX handler (LOCK/UNLOCK + time sync text) ======
class BdbRxCallbacks : public NimBLECharacteristicCallbacks {
public:
  // Support both NimBLE signatures; no 'override' to avoid version issues
  void onWrite(NimBLECharacteristic* c) { handleWrite(c); }
  void onWrite(NimBLECharacteristic* c, NimBLEConnInfo& /*info*/) { handleWrite(c); }
private:
  static void handleWrite(NimBLECharacteristic* c) {
    std::string v = c->getValue();
    if (v.empty()) return;
    if (v == "LOCK" || v == "lock")   { ignitionOn = false; applyRelay(); savePrefs(); return; }
    if (v == "UNLOCK" || v == "unlock"){ ignitionOn = true;  applyRelay(); savePrefs(); return; }
    // Optional: epoch sync if numeric payload
    auto isDigit = [](char ch){ return ch>='0' && ch<='9'; };
    bool all = true; for (char ch: v) if (!isDigit(ch)) { all=false; break; }
    if (all && v.size()>=9) {
      int64_t e = 0; for (char ch: v) e = e*10 + (ch-'0');
      if (e > 1000000000LL) { if (e > 2000000000LL) e/=1000LL; setEpochPersist(e, "ble"); }
    }
  }
};
static BdbRxCallbacks gBdbRxCb;

// ====== BLE server callbacks (gate connections) ======
class MyServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer* /*s*/, NimBLEConnInfo& info) override {
    NimBLEAddress addr = info.getAddress();
    bool bonded = NimBLEDevice::isBonded(addr);

    if (bonded) {
      Serial.printf("[BLE] Known bonded %s connected.\n", addr.toString().c_str());
      return; // allow
    }

    if (pairingActive) {
      NimBLEDevice::setSecurityPasskey(pairingCode);
      Serial.printf("[BLE] Pairing mode: starting security with %s\n", addr.toString().c_str());
      NimBLEDevice::startSecurity(info.getConnHandle());
      return;
    }

    // Not bonded and not in pairing window → reject
    Serial.printf("[BLE] Rejecting unbonded %s (pairing closed)\n", addr.toString().c_str());
    if (gBleServer) gBleServer->disconnect(info.getConnHandle());
  }

  void onAuthenticationComplete(NimBLEConnInfo& info) override {
    bool bonded = NimBLEDevice::isBonded(info.getAddress());
    Serial.printf("[SEC] Auth complete. bonded=%u peer=%s\n", (unsigned)bonded, info.getAddress().toString().c_str());
    if (bonded) {
      String addr = String(info.getAddress().toString().c_str());
      bool known=false; for (int i=0;i<btKeyCount;i++) if (btKeys[i].addr==addr) { known=true; break; }
      if (!known && btKeyCount < MAX_BT_KEYS) {
        btKeys[btKeyCount++] = { String("Key")+String(btKeyCount+1), String("Unknown"), addr };
        savePrefs();
      }
      pairingActive = false; pairingCode = 0; pairingUntil = 0;
    } else {
      if (gBleServer) gBleServer->disconnect(info.getConnHandle());
    }
  }
};
static MyServerCallbacks gServerCb;

// ====== BLE advertising ======
void bleStartAdvertising() {
  if (!gBleAdv) return;
  if (gBleAdv->isAdvertising()) gBleAdv->stop();

  NimBLEAdvertisementData advData;
  advData.addServiceUUID(BDB_UART_SVC);
  gBleAdv->setAdvertisementData(advData);

  NimBLEAdvertisementData scanData;
  scanData.setName(moduleName.c_str());
  gBleAdv->setScanResponseData(scanData);

  gBleAdv->start();
}

void bleInitIfNeeded() {
  if (gBleInited) return;

  NimBLEDevice::init(moduleName.c_str());
  NimBLEDevice::setDeviceName(moduleName.c_str());
  NimBLEDevice::setPower(ESP_PWR_LVL_P9);

  // Bonding + MITM + Secure Connections; PIN entry on phone
  NimBLEDevice::setSecurityAuth(true, true, true);
  NimBLEDevice::setSecurityIOCap(BLE_HS_IO_DISPLAY_ONLY);
  NimBLEDevice::setSecurityPasskey(123456); // replaced during Pairing Mode

  gBleServer = NimBLEDevice::createServer();
  gBleServer->setCallbacks(&gServerCb);

  gBdbSvc = gBleServer->createService(BDB_UART_SVC);
  gBdbRx  = gBdbSvc->createCharacteristic(
              BDB_UART_RX,
              NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR |
              NIMBLE_PROPERTY::WRITE_ENC | NIMBLE_PROPERTY::WRITE_AUTHEN
            );
  gBdbTx  = gBdbSvc->createCharacteristic(
              BDB_UART_TX,
              NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY |
              NIMBLE_PROPERTY::READ_ENC | NIMBLE_PROPERTY::READ_AUTHEN
            );
  gBdbRx->setCallbacks(&gBdbRxCb);
  gBdbTx->setValue("READY");
  gBdbSvc->start();

  gBleAdv = NimBLEDevice::getAdvertising();
  gBleInited = true;
  bleStartAdvertising();
}

// ====== Pages ======
void pageDashboard() {
  if (!requireAuth()) return;

  float v = readBatteryVoltage();

  String inner;
  inner += "<h2>" + moduleName + " – Dashboard</h2>";

  inner += String("<div class=card><div class=row><div><b>Module Uptime:</b> ") + fmtUptime(millis()) + "</div>";
  inner += String("<div class='muted'>Current Time: ") + fmtTime(time(nullptr)) + "</div></div></div>";

  if (needsPassChange)
    inner += "<div class=warn><b>Security:</b> Default admin credentials in use. Please change in Settings.</div>";

  inner += "<div class=grid>";

  // Ignition
  inner += "<div class=card><h3>Ignition</h3>";
  inner += String("<p>Status: ") + (ignitionOn ? "<span class=pill>ON</span>" : "<span class=pill>OFF</span>") + "</p>";
  inner += String("<p class=muted>GPIO ") + String(RELAY_PIN) + (RELAY_ACTIVE_HIGH ? " (active-high)</p>" : " (active-low)</p>");
  inner += String("<div class=row>"
                  "<form method='POST' action='/toggle_ign'><button type='submit'>")
        + String(ignitionOn ? "Turn OFF" : "Turn ON") + "</button></form>"
                  "</div>";
  inner += "</div>";

  // Voltage
  inner += String("<div class=card><h3>Voltage</h3><p><b>") + String(v,2) + " V</b></p>"
           "<div class=muted>ADC pin " + String(VOLTAGE_ADC_PIN) + " • Calibrate constants in code</div></div>";

  // Bluetooth summary
  int nb = NimBLEDevice::getNumBonds();
  inner += String("<div class=card><h3>Bluetooth Keys</h3><p><b>") + String(nb) + "</b> bonded device(s)</p>"
           "<a href='/bt'>Manage Keys →</a></div>";

  inner += "</div>";
  server.send(200, "text/html", htmlShell("/", "Dashboard", inner));
}

void pageSettings() {
  if (!requireAuth()) return;
  String msg = server.hasArg("m") ? server.arg("m") : "";
  String inner;
  inner += "<h2>Module Settings</h2>";
  if (msg == "saved") inner += "<div class=ok>Saved.</div>";
  if (msg == "aperr") inner += "<div class=err>SSID/Key validation failed.</div>";
  if (msg == "pwerr") inner += "<div class=err>Password validation failed.</div>";
  if (msg == "nmerr") inner += "<div class=err>Module name invalid.</div>";
  if (msg == "timeok") inner += "<div class=ok>Clock synchronized.</div>";
  if (msg == "timeerr") inner += "<div class=err>Clock sync failed.</div>";
  if (msg == "fwok") inner += "<div class=ok>Firmware updated. Rebooting…</div>";
  if (msg == "fwfail") inner += "<div class=err>Firmware update failed. See Serial for details.</div>";

  inner += "<div class=card><h3>Module Name</h3>"
           "<form method='POST' action='/set_name'>"
           "<label>Name<input name='name' minlength='3' maxlength='40' required value='" + moduleName + "'></label>"
           "<div class=spacer></div><button type='submit'>Save</button></form></div>";

  inner += "<div class=card><h3>Admin Credentials</h3>";
  if (needsPassChange) inner += "<div class=warn><b>Action required:</b> Change admin password now.</div>";
  inner += "<form method='POST' action='/set_admin'>"
           "<label>Username<input name='user' minlength='3' maxlength='32' required value='" + adminUser + "'></label>"
           "<label>New Password<input name='pass' type='password' minlength='6' maxlength='64' required></label>"
           "<div class=spacer></div><button type='submit'>Save</button></form></div>";

  // Wi-Fi with Show/Hide password
  inner += "<div class=card><h3>Wi-Fi Access Point</h3>"
           "<form method='POST' action='/set_ap'>"
           "<label>SSID<input name='ssid' minlength='3' maxlength='31' required value='" + apSsid + "'></label>"
           "<label>Password (8–63 chars)"
             "<div class='row' style='gap:8px'>"
               "<input id='wifikey' name='key' type='password' minlength='8' maxlength='63' required value='" + apKey + "'>"
               "<button type='button' id='pwbtn' onclick='(function(){var i=document.getElementById(\"wifikey\");var b=document.getElementById(\"pwbtn\");if(i.type===\"password\"){i.type=\"text\";b.textContent=\"Hide\";}else{i.type=\"password\";b.textContent=\"Show\";}})()'>Show</button>"
             "</div>"
           "</label>"
           "<div class=spacer></div><button type='submit'>Save</button></form></div>";

  // Offline Firmware Upgrade
  inner += "<div class=card><h3>Firmware Upgrade (offline)</h3>"
           "<p>Select a compiled <b>.bin</b> firmware file and flash it. Settings are kept.</p>"
           "<form method='POST' action='/fw_upload' enctype='multipart/form-data'>"
           "<input type='file' name='fw' accept='.bin' required>"
           "<div class=spacer></div><button type='submit'>Upload & Flash</button>"
           "</form>"
           "<p class='muted'>Tip: In Arduino IDE use <i>Sketch → Export Compiled Binary</i>, then upload the <code>.bin</code>.</p>"
           "</div>";

  time_t now = time(nullptr);
  inner += "<div class=card><h3>Clock</h3>"
           "<p>Current time: <b>" + fmtTime(now) + "</b></p>"
           "<div class=row><form onsubmit='return false;'>"
           "<button onclick='syncBrowserTime()' type='button'>Sync with Browser</button>"
           "</form></div>"
           "<script>function syncBrowserTime(){const e=Math.floor(Date.now()/1000);fetch('/set_time',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'epoch='+e+'&src=browser'}).then(()=>{location='/settings?m=timeok'}).catch(()=>{location='/settings?m=timeerr'});}</script>"
           "</div>";

  // Keep these at bottom
  inner += "<div class=card><h3>Reboot Module</h3>"
           "<p>Restart the ESP32 without changing any settings.</p>"
           "<form method='POST' action='/reboot_module'><button type='submit'>Reboot</button></form></div>";

  inner += "<div class=card><h3>Factory Reset</h3>"
           "<p class=warn>This will erase all settings, Bluetooth bonds/keys, and restore defaults. The module will reboot.</p>"
           "<form method='POST' action='/factory_reset'><button class='btn-danger' type='submit'>Factory Reset</button></form></div>";

  server.send(200, "text/html", htmlShell("/settings", "Settings", inner));
}

void pageBt() {
  if (!requireAuth()) return;
  String msg = server.hasArg("m") ? server.arg("m") : "";

  // Merge bonded devices from stack into our named list if missing
  int bonds = NimBLEDevice::getNumBonds();
  for (int i=0;i<bonds;i++) {
    String addr = String(NimBLEDevice::getBondedAddress(i).toString().c_str());
    if (findKeyIndexByAddr(addr) < 0 && btKeyCount < MAX_BT_KEYS) {
      btKeys[btKeyCount++] = { String("Key")+String(btKeyCount+1), String("Unknown"), addr };
    }
  }
  savePrefs();

  String inner;
  inner += "<h2>Bluetooth Keys</h2>";
  if (msg == "pair") inner += "<div class=ok>Pairing code generated.</div>";
  if (msg == "cancel") inner += "<div class=ok>Pairing cancelled.</div>";
  if (msg == "forgot") inner += "<div class=ok>Bond deleted.</div>";

  // Pairing card
  inner += "<div class=card><h3>Pair a new phone / key</h3>";
  if (pairingActive) {
    unsigned long remain = (millis() < pairingUntil) ? (pairingUntil - millis())/1000UL : 0;
    char buf[16]; snprintf(buf,sizeof(buf),"%06lu",(unsigned long)pairingCode);
    inner += "<p>Enter this one-time code when prompted on your phone:</p>";
    inner += String("<div class=code>") + buf + "</div>";
    inner += String("<p class=muted>Expires in ") + String(remain) + "s. Discoverable as <b>" + safeHtml(moduleName) + "</b>. Connect in your BLE app; the OS will ask for this code.</p>";
    inner += "<div class=row><form method='POST' action='/pair_cancel'><button type='submit'>Cancel Pairing</button></form></div>";
  } else {
    inner += "<p>When you start Pairing Mode, the module will ask for a 6-digit PIN on connect and only bond if it matches.</p>";
    inner += "<div class=row><form method='POST' action='/pair_start'><button type='submit'>Start Pairing (3 min)</button></form></div>";
  }
  inner += "</div>";

  // Saved/Bonded keys
  inner += "<div class=card><h3>Saved Keys</h3>";
  if (btKeyCount == 0) {
    inner += "<p class=muted>No bonded devices yet.</p>";
  } else {
    inner += "<table class=list><tr><th>Name</th><th>Type</th><th>Address</th><th>Actions</th></tr>";
    for (int i=0;i<btKeyCount;i++) {
      inner += "<tr>";
      inner += "<td><form method='POST' action='/btupdate' class=nowrap>"
               "<input type='hidden' name='i' value='"+String(i)+"'>"
               "<input name='name' minlength='2' maxlength='32' required value='"+safeHtml(btKeys[i].name)+"'></td>";
      inner += "<td><select name='type'>"
               "<option"+String(btKeys[i].type=="iPhone"?" selected":"")+">iPhone</option>"
               "<option"+String(btKeys[i].type=="Apple Watch"?" selected":"")+">Apple Watch</option>"
               "<option"+String(btKeys[i].type=="Android Phone"?" selected":"")+">Android Phone</option>"
               "<option"+String(btKeys[i].type=="Garmin Watch"?" selected":"")+">Garmin Watch</option>"
               "<option"+String(btKeys[i].type=="Other"?" selected":"")+">Other</option>"
               "</select></td>";
      inner += "<td class=muted>"+safeHtml(btKeys[i].addr)+"</td>";
      inner += "<td class=nowrap><button type='submit'>Save</button></form> "
               "<form method='POST' action='/btdelete' style='display:inline'><input type='hidden' name='i' value='"+String(i)+"'>"
               "<button class='btn-danger' type='submit'>Delete</button></form> "
               "<form method='POST' action='/forget_bond' style='display:inline'><input type='hidden' name='addr' value='"+safeHtml(btKeys[i].addr)+"'>"
               "<button type='submit'>Forget Bond</button></form>"
               "</td>";
      inner += "</tr>";
    }
    inner += "</table>";
  }
  inner += "</div>";

  server.send(200, "text/html", htmlShell("/bt", "Bluetooth Keys", inner));
}

// ====== Handlers ======
void handleRoot() { if (!requireAuth()) return; pageDashboard(); }

void handleSetName() {
  if (!requireAuth()) return;
  String nm = server.arg("name");
  bool ok = (nm.length()>=3 && nm.length()<=40 && isAsciiSafe(nm));
  if (!ok) { server.sendHeader("Location","/settings?m=nmerr"); server.send(302,"text/plain",""); return; }
  moduleName = nm; savePrefs();
  if (gBleInited) { NimBLEDevice::setDeviceName(moduleName.c_str()); bleStartAdvertising(); }
  server.sendHeader("Location","/settings?m=saved"); server.send(302,"text/plain","");
}

void handleSetAdmin() {
  if (!requireAuth()) return;
  String user = server.arg("user"), pass = server.arg("pass");
  bool ok = (user.length()>=3 && user.length()<=32 && pass.length()>=6 && pass.length()<=64 && isAsciiSafe(user) && isAsciiSafe(pass));
  if (!ok) { server.sendHeader("Location","/settings?m=pwerr"); server.send(302,"text/plain",""); return; }
  adminUser = user; adminPass = pass; needsPassChange = (adminUser=="admin" && adminPass=="admin");
  savePrefs(); server.sendHeader("Location","/settings?m=saved"); server.send(302,"text/plain","");
}

void handleSetAp() {
  if (!requireAuth()) return;
  String ssid = server.arg("ssid"), key = server.arg("key");
  bool ok = (ssid.length()>=3 && ssid.length()<=31 && key.length()>=8 && key.length()<=63 && isAsciiSafe(ssid) && isAsciiSafe(key));
  if (!ok) { server.sendHeader("Location","/settings?m=aperr"); server.send(302,"text/plain",""); return; }
  apSsid = ssid; apKey = key; savePrefs();
  String inner; inner += "<h2>Rebooting…</h2><p>Reconnect to SSID <b>"+apSsid+"</b> and open <b>http://192.168.23.1</b>.</p><form><button disabled>Rebooting...</button></form>";
  server.send(200, "text/html", htmlShell("/settings", "Rebooting", inner));
  delay(300); ESP.restart();
}

void handleSetTime() {
  if (!requireAuth()) return;
  if (!server.hasArg("epoch")) { server.sendHeader("Location","/settings?m=timeerr"); server.send(302,"text/plain",""); return; }
  int64_t sec = atoll(server.arg("epoch").c_str());
  if (sec <= 0) { server.sendHeader("Location","/settings?m=timeerr"); server.send(302,"text/plain",""); return; }
  setEpochPersist(sec, server.hasArg("src") ? server.arg("src").c_str() : "http");
  server.sendHeader("Location","/settings?m=timeok"); server.send(302,"text/plain","");
}

void handleFactoryReset() {
  if (!requireAuth()) return;
  String inner; inner += "<h2>Factory Reset</h2><p>Erasing settings… rebooting.</p><form><button disabled>Resetting...</button></form>";
  server.send(200, "text/html", htmlShell("/settings", "Factory Reset", inner));
  delay(300); factoryReset();
}

void handleRebootModule() {
  if (!requireAuth()) return;
  String inner; inner += "<h2>Rebooting Module</h2><p>Please reconnect after a few seconds.</p><form><button disabled>Rebooting...</button></form>";
  server.send(200, "text/html", htmlShell("/settings", "Rebooting", inner));
  delay(300); ESP.restart();
}

void handleToggleIgn() {
  if (!requireAuth()) return;
  ignitionOn = !ignitionOn; applyRelay(); savePrefs();
  server.sendHeader("Location","/"); server.send(302,"text/plain","");
}

void handleBtUpdate() {
  if (!requireAuth()) return;
  if (!server.hasArg("i")) { server.send(400,"text/plain","Missing index"); return; }
  int idx = server.arg("i").toInt();
  if (idx<0 || idx>=btKeyCount) { server.send(400,"text/plain","Bad index"); return; }
  String name = server.arg("name");
  String type = server.arg("type");
  if (name.length()<2 || !isAsciiSafe(name) || !isAsciiSafe(type)) { server.send(400,"text/plain","Invalid input"); return; }
  btKeys[idx].name = name; btKeys[idx].type = type; savePrefs();
  server.sendHeader("Location","/bt"); server.send(302,"text/plain","");
}

void handleBtDelete() {
  if (!requireAuth()) return;
  int idx = server.hasArg("i") ? server.arg("i").toInt() : -1;
  if (idx<0 || idx>=btKeyCount) { server.send(400,"text/plain","Bad index"); return; }
  for (int i=idx;i<btKeyCount-1;i++) btKeys[i]=btKeys[i+1];
  btKeyCount--; savePrefs();
  server.sendHeader("Location","/bt"); server.send(302,"text/plain","");
}

void handleForgetBond() {
  if (!requireAuth()) return;
  if (!server.hasArg("addr")) { server.send(400,"text/plain","Missing addr"); return; }

  String addrStr = server.arg("addr");
  bool found = false;
  NimBLEAddress match;

  int nb = NimBLEDevice::getNumBonds();
  for (int i = 0; i < nb; ++i) {
    NimBLEAddress a = NimBLEDevice::getBondedAddress(i);
    String bonded = String(a.toString().c_str());
    if (addrStr.equalsIgnoreCase(bonded)) { match = a; found = true; break; }
  }
  if (!found) { server.send(404, "text/plain", "Bond not found"); return; }

  NimBLEDevice::deleteBond(match);

  int idx = -1;
  for (int i = 0; i < btKeyCount; ++i) {
    if (addrStr.equalsIgnoreCase(btKeys[i].addr)) { idx = i; break; }
  }
  if (idx >= 0) {
    for (int i = idx; i < btKeyCount - 1; ++i) btKeys[i] = btKeys[i+1];
    btKeyCount--;
    savePrefs();
  }

  server.sendHeader("Location","/bt?m=forgot");
  server.send(302, "text/plain", "");
}

void handleLogout() {
  // Force Basic Auth re-prompt; most browsers "log out" on 401 here.
  server.sendHeader("WWW-Authenticate", "Basic realm=\"ESP32\", charset=\"UTF-8\"");
  server.send(401, "text/plain", "Logged out");
}

// Pairing start/stop (3 minutes)
uint32_t genCode() { return (uint32_t)(random(100000, 1000000)); }
void handlePairStart() {
  if (!requireAuth()) return;
  pairingCode = genCode();
  NimBLEDevice::setSecurityPasskey(pairingCode);
  pairingActive = true;
  pairingUntil = millis() + PAIR_WINDOW_MS;
  bleStartAdvertising();
  server.sendHeader("Location","/bt?m=pair"); server.send(302,"text/plain","");
}
void handlePairCancel() {
  if (!requireAuth()) return;
  pairingActive = false; pairingCode = 0; pairingUntil = 0;
  server.sendHeader("Location","/bt?m=cancel"); server.send(302,"text/plain","");
}

// ====== NEW: Firmware upload (multipart form) ======
void handleFwUploadFinish() {
  if (!requireAuth()) return;
  bool ok = !Update.hasError();
  if (ok) {
    server.sendHeader("Location","/settings?m=fwok"); server.send(302,"text/plain","");
    delay(400);
    ESP.restart();
  } else {
    server.sendHeader("Location","/settings?m=fwfail"); server.send(302,"text/plain","");
  }
}

void handleFwUploadStream() {
  if (!requireAuth()) return;
  HTTPUpload& upload = server.upload();
  if (upload.status == UPLOAD_FILE_START) {
    Serial.printf("[FW] Update start: %s, size unknown\n", upload.filename.c_str());
    if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
      Serial.printf("[FW] Update.begin failed: %s\n", Update.errorString());
    }
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
      Serial.printf("[FW] Write failed: %s\n", Update.errorString());
    }
  } else if (upload.status == UPLOAD_FILE_END) {
    if (Update.end(true)) {
      Serial.printf("[FW] Update success: %u bytes\n", upload.totalSize);
    } else {
      Serial.printf("[FW] Update.end failed: %s\n", Update.errorString());
    }
  } else if (upload.status == UPLOAD_FILE_ABORTED) {
    Update.abort();
    Serial.println("[FW] Update aborted");
  }
}

// ====== AP bring-up ======
void setupAP() { WiFi.softAPConfig(apIP, apGW, apMask); WiFi.softAP(apSsid.c_str(), apKey.c_str(), AP_CH, false, 8); }

// ====== Setup / Loop ======
void setup() {
  Serial.begin(115200); delay(200);
  randomSeed(esp_random());

  pinMode(RELAY_PIN, OUTPUT);
  ignitionOn = false; applyRelay();

  analogReadResolution(12);

  loadPrefs(); applyRelay();
  loadSavedTime();

  WiFi.persistent(false); WiFi.mode(WIFI_AP); WiFi.setSleep(false);
  setupAP();

  // HTTP routes
  server.on("/",            HTTP_GET,  [](){ handleRoot(); });
  server.on("/settings",    HTTP_GET,  [](){ pageSettings(); });
  server.on("/bt",          HTTP_GET,  [](){ pageBt(); });

  server.on("/set_name",    HTTP_POST, [](){ handleSetName(); });
  server.on("/set_admin",   HTTP_POST, [](){ handleSetAdmin(); });
  server.on("/set_ap",      HTTP_POST, [](){ handleSetAp(); });
  server.on("/set_time",    HTTP_POST, [](){ handleSetTime(); });

  // NEW: firmware upload route (multipart)
  server.on("/fw_upload",   HTTP_POST, [](){ handleFwUploadFinish(); }, [](){ handleFwUploadStream(); });

  server.on("/factory_reset", HTTP_POST, [](){ handleFactoryReset(); });
  server.on("/reboot_module", HTTP_POST, [](){ handleRebootModule(); });

  server.on("/toggle_ign",  HTTP_POST, [](){ handleToggleIgn(); });

  server.on("/btupdate",    HTTP_POST, [](){ handleBtUpdate(); });
  server.on("/btdelete",    HTTP_POST, [](){ handleBtDelete(); });
  server.on("/forget_bond", HTTP_POST, [](){ handleForgetBond(); });

  server.on("/pair_start",  HTTP_POST, [](){ handlePairStart(); });
  server.on("/pair_cancel", HTTP_POST, [](){ handlePairCancel(); });

  server.on("/logout",      HTTP_GET,  [](){ handleLogout(); });

  server.begin();
  Serial.println("AP up @ 192.168.23.1 (HTTP). Open: http://192.168.23.1");

  // BLE
  bleInitIfNeeded();
}

void loop() {
  server.handleClient();

  // Pairing timeout
  if (pairingActive && millis() > pairingUntil) {
    pairingActive = false; pairingCode = 0; pairingUntil = 0;
    Serial.println("[BLE] Pairing window expired");
  }

  // Advertising keep-alive
  static unsigned long lastBleKick = 0;
  if (millis() - lastBleKick > 5000) {
    lastBleKick = millis();
    if (gBleInited && gBleAdv && gBleServer &&
        gBleServer->getConnectedCount() == 0 &&
        !gBleAdv->isAdvertising()) {
      Serial.println("[BLE] Advertising stopped unexpectedly – restarting");
      bleStartAdvertising();
    }
  }
}
