// === Combined Prometheus Pushgateway Test Harness ===
// A single ESP32 that pushes DUMMY data for ALL test devices to the local
// Pushgateway, so the server/dashboard interface can be validated without any
// real hardware. Combines the ECS devices (full sensor set) and the AC devices
// (t1/t2/relay1 only) into one sweep.
//
//   ECS metrics: t1 t2 rh1 rh2 pm1 pm25 pm10 avg_particle_size
//                nc0_5 nc1_0 nc2_5 nc10 relay1 relay2
//   AC  metrics: t1 t2 relay1
//
// Each sweep pushes every device with a 1s gap between consecutive devices.
// This is a test tool only — it has no sensor / NVS / NTP / Supabase logic.

#include <WiFiManager.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <base64.h>

// === Local Server (Prometheus Pushgateway) ===
// Full push URL = pushBaseURL + device id, e.g.
//   http://13.200.74.140:9091/metrics/job/sensors/sensor_id/ecs_1
//https://dhap-api.dbf.ooo/#
const char *pushBaseURL = "https://dhap-api.dbf.ooo/metrics/job/sensors/sensor_id/";

// === WiFi ===
// On boot the device tries this static network first; if it is not reachable
// within STATIC_WIFI_TIMEOUT_MS, it opens the WiFiManager config portal (AP
// named PORTAL_AP_NAME) so credentials can be entered from a web page.
#define STATIC_SSID            "dada"
#define STATIC_PASS            "dadaniruma"
#define STATIC_WIFI_TIMEOUT_MS 15000
#define PORTAL_AP_NAME         "TESTHARNESS_SETUP"

// Basic auth credentials
const char* pgUser = "admin";
const char* pgPass = "admin1";

// Start a new sweep every SEND_INTERVAL_MS. Within a sweep, push exactly one
// device every DEVICE_GAP_MS (1/sec), so 6 devices take ~6s, then idle until
// the next 15s mark.
#define SEND_INTERVAL_MS 15000
#define DEVICE_GAP_MS    1000

// === Device table ===
enum DeviceType { DEV_ECS, DEV_AC };
struct TestDevice {
  const char *id;
  DeviceType  type;
};

// Add/remove test devices here — the harness loops over this list each sweep.
TestDevice devices[] = {
  {"ecs_1", DEV_ECS},
  {"ecs_2", DEV_ECS},
  {"ecs_3", DEV_ECS},
  {"ac_1",  DEV_AC},
  {"ac_2",  DEV_AC},
  {"ac_3",  DEV_AC},
};
const size_t numDevices = sizeof(devices) / sizeof(devices[0]);

unsigned long lastSweep = 0;       // start time of the current/last sweep
unsigned long lastDevicePush = 0;  // time of the last single-device push
unsigned long lastWiFiCheck = 0;
size_t sweepIndex = 0;             // next device to push in the active sweep
bool sweeping = false;             // true while a sweep is in progress

// === Prometheus Pushgateway helpers ===
// Append one "name value" line in Prometheus text exposition format.
static void addMetric(String &body, const char *name, const String &value) {
  body += name;
  body += ' ';
  body += value;
  body += '\n';
}

// POST a Prometheus text body to the Pushgateway for the given device id.
bool pushToGateway(const char *id, const String &body) {
  if (WiFi.status() != WL_CONNECTED) return false;

  HTTPClient http;
  String url = String(pushBaseURL) + id;
  http.begin(url);
  http.addHeader("Content-Type", "text/plain");

   // Build Basic Auth header: base64("user:pass")
  String credentials = String(pgUser) + ":" + String(pgPass);
  String encodedCreds = base64::encode(credentials);
  http.addHeader("Authorization", "Basic " + encodedCreds);

  int code = http.POST(body);
  Serial.println("📤 POST " + url);
  Serial.print(body);
  bool ok = (code == 200 || code == 202);
  if (ok) {
    Serial.println("✅ Pushgateway: POST success");
  } else {
    Serial.printf("❌ Pushgateway POST failed. Code: %d, Body: %s\n", code, http.getString().c_str());
  }
  http.end();
  return ok;
}

// ECS dummy: full sensor set. `bias` shifts the temperatures per device so each
// one is distinguishable on the dashboard.
void pushDummyECS(const char *id, float bias) {
  String body;
  addMetric(body, "t1", String(22.56 + bias + random(-30, 30) / 100.0, 2));
  addMetric(body, "t2", String(21.78 + bias + random(-30, 30) / 100.0, 2));
  addMetric(body, "rh1", String(25.12 + random(-50, 50) / 100.0, 2));
  addMetric(body, "rh2", String(23.34 + random(-50, 50) / 100.0, 2));
  addMetric(body, "pm1", String(23.1 + random(-20, 20) / 10.0, 1));
  addMetric(body, "pm25", String(22.3 + random(-20, 20) / 10.0, 1));
  addMetric(body, "pm10", String(54.4 + random(-30, 30) / 10.0, 1));
  addMetric(body, "avg_particle_size", String(0.85 + random(-10, 10) / 100.0, 2));
  addMetric(body, "nc0_5", String(1234 + random(-50, 50)));
  addMetric(body, "nc1_0", String(567 + random(-30, 30)));
  addMetric(body, "nc2_5", String(89 + random(-10, 10)));
  addMetric(body, "nc10", String(40 + random(-5, 5)));
  addMetric(body, "relay1", random(0, 2) ? "1" : "0");
  addMetric(body, "relay2", random(0, 2) ? "1" : "0");
  pushToGateway(id, body);
}

// AC dummy: only t1, t2, relay1.
void pushDummyAC(const char *id, float bias) {
  String body;
  addMetric(body, "t1", String(24.00 + bias + random(-50, 50) / 100.0, 2));
  addMetric(body, "t2", String(18.50 + bias + random(-50, 50) / 100.0, 2));
  addMetric(body, "relay1", random(0, 2) ? "1" : "0");
  pushToGateway(id, body);
}

void pushDummy(const TestDevice &d, float bias) {
  if (d.type == DEV_ECS) pushDummyECS(d.id, bias);
  else                   pushDummyAC(d.id, bias);
}

// Try the static/default network first; if it is not available within
// STATIC_WIFI_TIMEOUT_MS, open the WiFiManager config portal (web page).
void connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(STATIC_SSID, STATIC_PASS);
  Serial.printf("📶 Trying static SSID \"%s\"", STATIC_SSID);
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < STATIC_WIFI_TIMEOUT_MS) {
    delay(500);
    Serial.print(".");
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("\n✅ Connected to \"%s\", IP: %s\n", STATIC_SSID, WiFi.localIP().toString().c_str());
    return;
  }

  Serial.printf("\n⚠️ \"%s\" not available — opening config portal (AP: %s)\n", STATIC_SSID, PORTAL_AP_NAME);
  WiFiManager wm;
  wm.setConfigPortalTimeout(120);
  wm.setWiFiAutoReconnect(true);
  if (!wm.autoConnect(PORTAL_AP_NAME)) {
    Serial.println("❌ Portal timeout / failed. Restarting...");
    ESP.restart();
  }
  Serial.printf("✅ Connected via portal, IP: %s\n", WiFi.localIP().toString().c_str());
}

void checkWiFi() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("⚠️ WiFi disconnected! Reconnecting...");
    connectWiFi();
  }
}

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\n=== Prometheus Pushgateway combined test harness ===");

  connectWiFi();
  Serial.printf("✅ Pushing %u devices every %lus (1s gap each)\n",
                (unsigned)numDevices, (unsigned long)(SEND_INTERVAL_MS / 1000));

  lastSweep = millis() - SEND_INTERVAL_MS; // push immediately on first loop
  lastWiFiCheck = millis();
}

void loop() {
  unsigned long now = millis();

  if (now - lastWiFiCheck > 10000) {
    lastWiFiCheck = now;
    checkWiFi();
  }

  // Start a new sweep every SEND_INTERVAL_MS.
  if (!sweeping && now - lastSweep >= SEND_INTERVAL_MS) {
    lastSweep = now;
    sweeping = true;
    sweepIndex = 0;
    lastDevicePush = now - DEVICE_GAP_MS; // push the first device immediately
    Serial.printf("\n🔁 Sweep start: %u devices, 1/sec...\n", (unsigned)numDevices);
  }

  // During a sweep, push exactly one device per DEVICE_GAP_MS (1/sec).
  if (sweeping && now - lastDevicePush >= DEVICE_GAP_MS) {
    lastDevicePush = now;
    pushDummy(devices[sweepIndex], sweepIndex * 0.5f); // small per-device bias
    sweepIndex++;
    if (sweepIndex >= numDevices) sweeping = false;    // sweep complete
  }

  delay(10);
}
