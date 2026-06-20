#ifndef ARDUINOJSON_DEPRECATED
#define ARDUINOJSON_DEPRECATED(msg)
#endif
#include <WiFiManager.h> 
#include <WiFi.h>
#include <SPI.h>
#include <time.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <Preferences.h>
#include <WiFiClientSecure.h>
#include <base64.h>
#include "secrets.h"   // gitignored — copy include/secrets.example.h to include/secrets.h

// === NVS (Non-Volatile Storage) Setup ===
// Using Preferences API for better flash wear leveling than EEPROM
Preferences preferences;

// === Time sync configuration ===
// Change these macros to adjust sync/retry behavior
#ifndef TIME_SYNC_INTERVAL_MS
#define TIME_SYNC_INTERVAL_MS 3600000UL // 1 hour
#endif
#ifndef TIME_RETRY_INTERVAL_MS
#define TIME_RETRY_INTERVAL_MS 600000UL // 10 minutes
#endif
#ifndef TIME_INITIAL_TIMEOUT_MS
// Give the initial NTP sync a longer window (15s) to accommodate slow networks
#define TIME_INITIAL_TIMEOUT_MS 15000UL // initial NTP wait (ms)
#endif
#ifndef TIME_SYNC_ATTEMPT_TIMEOUT_MS
// Per-attempt timeout for subsequent sync tries (10s)
#define TIME_SYNC_ATTEMPT_TIMEOUT_MS 10000UL // single sync attempt timeout (ms) to avoid long blocking
#endif

// How often to dump the schedule table to Serial (default 5 minutes)
#ifndef SCHEDULE_DUMP_INTERVAL_MS
#define SCHEDULE_DUMP_INTERVAL_MS 60000UL
#endif

// Device identifier used to select schedules in Supabase
const char *deviceId = "ac_1";  // under test

// Maximum schedules to load
#define MAX_SCHEDULES 8

// India Standard Time offset from UTC in seconds (+5:30)
static const long IST_OFFSET_SECONDS = 5 * 3600 + 30 * 60;
// === Supabase API Info ===
// === Supabase API Info ===
const char *getURL = "https://nkkwdcsoijwcbgqrublg.supabase.co/rest/v1/commands";
const char *getURLschedule = "https://nkkwdcsoijwcbgqrublg.supabase.co/rest/v1/schedule";
const char *postURL = "https://nkkwdcsoijwcbgqrublg.supabase.co/rest/v1/sensor_data";
const char *apikey = "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJpc3MiOiJzdXBhYmFzZSIsInJlZiI6Im5ra3dkY3NvaWp3Y2JncXJ1YmxnIiwicm9sZSI6ImFub24iLCJpYXQiOjE3NjM0OTg2MDgsImV4cCI6MjA3OTA3NDYwOH0.z3P1a_zOvjm1EGAggj6JS5u0Eo091mUcZ0wXyfEge-w";

// === Local Server (Prometheus Pushgateway, HTTPS + Basic auth) ===
// Full push URL = pushBaseURL + device id, e.g.
//   https://dhap-api.dbf.ooo/metrics/job/sensors/sensor_id/ac_1
const char *pushBaseURL = "https://dhap-api.dbf.ooo/metrics/job/sensors/sensor_id/";
const char *pgUser = SECRET_PG_USER;
const char *pgPass = SECRET_PG_PASS;

// === Relay command API (plain HTTP, port 8000, Basic auth — same creds as push) ===
// GET relayCmdBaseURL + deviceId + "/relays"  (polled every RELAY_POLL_MS).
//   -> {"status":"success","sensor_id":"ac_1","relay1":0,"relay2":1}  (AC uses relay1 only)
const char *relayCmdBaseURL = "http://dhap-api.dbf.ooo:8000/devices/"; // TEMP: plain HTTP on :8000 (may move to HTTPS/standard port later)
#define RELAY_POLL_MS    20000
#define SEND_INTERVAL_MS 30000

// Value pushed for a failed/disconnected temperature sensor, so the dashboard
// shows an explicit failure marker instead of the last (stale) value.
// 0 is never a real AC temperature, so it reads as an unambiguous fault.
#define SENSOR_FAIL_VALUE 0

// Relay-API-only mode: drive relay1 from the new API and bypass the Supabase
// schedule/NTP/manual-override engine. Set to 0 to restore the schedule engine.
#define RELAY_API_ONLY 1

// TEMP (testing): try this static network before the WiFiManager portal.
#define STATIC_SSID            SECRET_WIFI_SSID
#define STATIC_PASS            SECRET_WIFI_PASS
#define STATIC_WIFI_TIMEOUT_MS 15000

// === GPIO Definitions ===
#define ONE_WIRE_BUS_1 23
#define ONE_WIRE_BUS_2 22
#define RELAY1_PIN 32

OneWire oneWire1(ONE_WIRE_BUS_1);
OneWire oneWire2(ONE_WIRE_BUS_2);
DallasTemperature sensor1(&oneWire1);
DallasTemperature sensor2(&oneWire2);

bool relayState1 = false;
unsigned long lastRelayCheck = 0;
unsigned long lastSensorSend = 0;
unsigned long lastWiFiCheck = 0;
unsigned long lastNVSWrite = 0;
// lastScheduleCheck is used to run schedule checks every minute
unsigned long lastScheduleCheck = 0;
unsigned long lastScheduleDump = 0;
// Whether schedule logic is allowed (requires valid NTP time)
bool scheduleAllowed = false;
// NTP time sync status - track if we've EVER successfully synced (not just current attempt)
bool ntpEverSynced = false;  // Set to true once NTP sync succeeds at least once
bool ntpCurrentlyFailing = false;  // True if recent sync attempts are failing
unsigned long ntpLastRetryMs = 0;
// Track manual override: when a manual command is issued, ignore schedule until next boundary
bool manualOverridePending = false; // whether manual override is still active
time_t manualOverrideExpiryEpoch = 0; // epoch timestamp when override expires (0 = no schedule boundary, only timer can expire it)
bool manualOverrideState = false; // the relay state set by manual override

// === Data Usage Tracking (Supabase egress/ingress) ===
unsigned long supabaseEgressBytes = 0;  // Data downloaded from Supabase (GET responses)
unsigned long supabaseIngressBytes = 0; // Data uploaded to Supabase (POST payloads)
unsigned long lastDataUsagePrintMs = 0;
const unsigned long DATA_USAGE_PRINT_INTERVAL = 60000; // Print every 1 minute

// Track and log data usage for an API call
void trackDataUsage(const char *apiName, unsigned long requestBytes, unsigned long responseBytes) {
  supabaseIngressBytes += requestBytes;
  supabaseEgressBytes += responseBytes;
  Serial.printf("[DATA] %s: ↑ %lu bytes (ingress), ↓ %lu bytes (egress)\n", apiName, requestBytes, responseBytes);
}

// Print cumulative data usage statistics
void printDataUsageStats() {
  unsigned long now = millis();
  if (now - lastDataUsagePrintMs < DATA_USAGE_PRINT_INTERVAL) return;
  lastDataUsagePrintMs = now;
  
  unsigned long totalBytes = supabaseEgressBytes + supabaseIngressBytes;
  
  Serial.println("\n📊 === SUPABASE DATA USAGE (1 minute) ===");
  Serial.printf("Total Egress (⬇️ downloaded): %lu bytes (%.2f KB)\n", supabaseEgressBytes, supabaseEgressBytes / 1024.0);
  Serial.printf("Total Ingress (⬆️ uploaded):   %lu bytes (%.2f KB)\n", supabaseIngressBytes, supabaseIngressBytes / 1024.0);
  Serial.printf("Total Combined:                %lu bytes (%.2f KB)\n", totalBytes, totalBytes / 1024.0);
  
  // Extrapolate to daily usage for 3 devices
  // bytes/min → KB/min (÷1024) → KB/day (×1440 min/day) → Total for 3 devices (×3)
  float dailyUsageKB = (totalBytes / 1024.0) * 1440.0 * 3.0;
  Serial.printf("Estimated Daily (3 devices):   %.2f KB/day (%.2f MB/day)\n", dailyUsageKB, dailyUsageKB / 1024.0);
  Serial.println("📊 === END ===\n");
  
  // Reset counters for next minute
  supabaseEgressBytes = 0;
  supabaseIngressBytes = 0;
}

// === Fetch Relay Command ===
bool fetchRelayCommand(const char *sensor_id, const char *target, bool currentState) {
  HTTPClient http;
  String url = String(getURL) + "?sensor_id=eq." + sensor_id + "&target=eq." + target + "&order=issued_at.desc&limit=1";
  http.begin(url);
  http.addHeader("apikey", apikey);
  http.addHeader("Authorization", "Bearer " + String(apikey));

  int httpCode = http.GET();
  if (httpCode == 200) {
    String response = http.getString();
    // Track egress (response from Supabase)
    unsigned long requestSize = url.length() + 100; // Approximate request size (URL + headers)
    unsigned long responseSize = response.length();
    trackDataUsage("fetchRelayCommand", requestSize, responseSize);
    
    int tsStart = response.indexOf("\"issued_at\":\"") + 13;
    int tsEnd = response.indexOf("\"", tsStart);
    if (tsStart > 12 && tsEnd > tsStart) {
      String timestampStr = response.substring(tsStart, tsEnd);
      struct tm tm;
      if (sscanf(timestampStr.c_str(), "%4d-%2d-%2dT%2d:%2d:%2d",
                 &tm.tm_year, &tm.tm_mon, &tm.tm_mday,
                 &tm.tm_hour, &tm.tm_min, &tm.tm_sec) == 6) {
        tm.tm_year -= 1900;
        tm.tm_mon -= 1;
        time_t issuedEpoch = mktime(&tm);
        time_t nowEpoch = time(nullptr);
        if (difftime(nowEpoch, issuedEpoch) > 120) return currentState;
        if (response.indexOf("\"state\":true") != -1) return true;
        if (response.indexOf("\"state\":false") != -1) return false;
      }
    }
  }
  http.end();
  return currentState;
}

// === Prometheus Pushgateway helpers ===
// Append one "name value" line in Prometheus text exposition format.
static void addMetric(String &body, const char *name, const String &value) {
  body += name;
  body += ' ';
  body += value;
  body += '\n';
}

// POST a Prometheus text body to the Pushgateway for the given device id.
bool pushToGateway(const String &id, const String &body) {
  if (WiFi.status() != WL_CONNECTED) return false;

  WiFiClientSecure client;
  client.setInsecure();                 // TLS without cert validation

  HTTPClient http;
  String url = String(pushBaseURL) + id;
  http.begin(client, url);
  http.setConnectTimeout(3000);
  http.setTimeout(5000);
  http.addHeader("Content-Type", "text/plain");

  String creds = String(pgUser) + ":" + String(pgPass);
  http.addHeader("Authorization", "Basic " + base64::encode(creds));

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

// === Send Sensor Data (pushes t1, t2, relay1 to the Pushgateway) ===
// Always sends t1/t2; a failed/disconnected sensor sends SENSOR_FAIL_VALUE so the
// dashboard shows an explicit failure marker instead of the last (stale) value.
void sendSensorData(String id, float t1, float t2, bool valid1, bool valid2, bool relay1) {
  if (WiFi.status() != WL_CONNECTED) return;

  String body;
  addMetric(body, "t1", valid1 ? String(t1, 2) : String(SENSOR_FAIL_VALUE));
  addMetric(body, "t2", valid2 ? String(t2, 2) : String(SENSOR_FAIL_VALUE));
  addMetric(body, "relay1", relay1 ? "1" : "0");

  pushToGateway(id, body);
}

// Parse an integer 0/1 value for `key` from a small JSON body. Returns -1 if absent.
static int parseRelayValue(const String &resp, const char *key) {
  String needle = String("\"") + key + "\"";
  int idx = resp.indexOf(needle);
  if (idx < 0) return -1;
  idx = resp.indexOf(':', idx);
  if (idx < 0) return -1;
  idx++;
  while (idx < (int)resp.length() && (resp[idx] == ' ' || resp[idx] == '\t')) idx++;
  if (idx >= (int)resp.length()) return -1;
  if (resp[idx] == '1') return 1;
  if (resp[idx] == '0') return 0;
  return -1;
}

// Fetch relay1 command from the local server and apply it (Basic auth). AC has one relay.
//   GET http://13.200.74.140:8000/devices/<deviceId>/relays
//   -> {"status":"success","sensor_id":"ac_1","relay1":0,"relay2":1}
void fetchRelayCommands() {
  if (WiFi.status() != WL_CONNECTED) return;

  HTTPClient http;
  String url = String(relayCmdBaseURL) + deviceId + "/relays";
  Serial.println("🌐 Relay GET: " + url);
  http.begin(url);
  http.setConnectTimeout(3000);
  http.setTimeout(5000);
  http.addHeader("Authorization", "Basic " + base64::encode(String(pgUser) + ":" + String(pgPass)));
  int code = http.GET();
  if (code == 200) {
    String resp = http.getString();
    Serial.printf("✅ Relay GET 200: %s\n", resp.c_str());
    int r1 = parseRelayValue(resp, "relay1");
    if (r1 == 0 || r1 == 1) {
      bool oldR1 = relayState1;
      relayState1 = (r1 == 1);
      digitalWrite(RELAY1_PIN, relayState1 ? HIGH : LOW);  // AC relay is active-HIGH
      if (relayState1 != oldR1)
        Serial.printf("🔄 Relay updated from server: relay1=%d\n", relayState1);
    }
  } else {
    Serial.printf("❌ Relay GET failed. Code: %d\n", code);
  }
  http.end();
}

// Try the static/default network first; if unavailable, open the WiFiManager portal.
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

  Serial.println("\n⚠️ Static SSID not available — opening config portal...");
  WiFiManager wm;
  wm.setConfigPortalTimeout(120);
  wm.setWiFiAutoReconnect(true);
  String setupName = String(deviceId) + "_SETUP";
  if (!wm.autoConnect(setupName.c_str())) {
    Serial.println("❌ WiFiManager failed. Restarting...");
    ESP.restart();
  }
}

// === Check WiFi and fallback to WiFiManager if failed ===
void checkWiFi() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("⚠️ WiFi disconnected! Attempting reconnect...");
    WiFi.disconnect();
    WiFi.begin();

    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < 10000) {
      delay(500);
      Serial.print(".");
    }

    if (WiFi.status() == WL_CONNECTED) {
      Serial.println("\n✅ WiFi reconnected!");
      return;
    }

    Serial.println("\n❌ Reconnect failed. Starting WiFiManager portal...");
    WiFiManager wm;
    wm.setConfigPortalTimeout(120);  // 2 minutes
    String setupName = String(deviceId) + "_SETUP";
    if (!wm.autoConnect(setupName.c_str())) {
      Serial.println("⏱ Portal timeout. Restarting...");
      delay(1000);
      ESP.restart();
    }

    Serial.println("✅ Connected via WiFiManager");
  }
}

// === Read Sensors and return individual validity ===
void readSensors(float &temp1, float &temp2, bool &valid1, bool &valid2) {
  sensor1.requestTemperatures();
  sensor2.requestTemperatures();
  delay(750);
  temp1 = sensor1.getTempCByIndex(0);
  temp2 = sensor2.getTempCByIndex(0);
  valid1 = (temp1 != 85.0 && temp1 != -127.0);
  valid2 = (temp2 != 85.0 && temp2 != -127.0);
}
// Wait for NTP sync and return epoch time (or 0 if not synced within timeout)
time_t fetchNetworkTime(unsigned long timeoutMs = 5000) {
  time_t now = time(nullptr);
  const time_t validThreshold = 1000000000; // ~2001-09-09, any sane current time will be > this
  unsigned long start = millis();
  while (now < validThreshold && (millis() - start) < timeoutMs) {
    // Use a short sleep to remain responsive; this loop will exit after timeoutMs
    delay(50);
    now = time(nullptr);
  }
  // If we exited because timeout and no valid time, return -1 to indicate timeout
  if (now < validThreshold) return (time_t)-1;
  return now;
}

// === TimeManager ===
// Keeps a local epoch running using millis() and periodically attempts to sync
// with NTP. If offline or NTP fails, local time continues advancing.
class TimeManager {
public:
  TimeManager()
      : lastSyncedEpoch(0), lastSyncMillis(0), lastAttemptMillis(0),
        syncIntervalMs(TIME_SYNC_INTERVAL_MS), retryIntervalMs(TIME_RETRY_INTERVAL_MS),
        lastSyncSuccess(false), lastNoUpdateLogMillis(0) {}

  // Initialize the manager and attempt an immediate sync (timeout ms)
  void begin(unsigned long intervalMs = TIME_SYNC_INTERVAL_MS, unsigned long initialTimeoutMs = TIME_INITIAL_TIMEOUT_MS) {
    syncIntervalMs = intervalMs;
    // Record baseline attempt time so retry/sync intervals are calculated
    lastAttemptMillis = millis();
    // Try initial sync; if it fails, seed with current system time (may be 0)
    if (!trySync(initialTimeoutMs)) {
      lastSyncedEpoch = time(nullptr);
      lastSyncSuccess = false;
      lastAttemptMillis = millis();
      Serial.println("⚠️ TimeManager: Initial NTP sync failed — continuing with local clock");
    }
  }

  // Attempt to sync with network time. Returns true if successful.
  bool trySync(unsigned long timeoutMs = TIME_SYNC_ATTEMPT_TIMEOUT_MS) {
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("⚠️ TimeManager: NTP sync skipped — WiFi not connected");
      lastSyncSuccess = false;
      lastAttemptMillis = millis();
      return false;
    }
    // Ensure SNTP is (re)configured before waiting for time
    configTime(0, 0, "pool.ntp.org", "time.nist.gov");
    unsigned long attemptStart = millis();
    Serial.printf("⏳ TimeManager: attempting NTP sync (timeout %lums)...\n", timeoutMs);
    time_t epoch = fetchNetworkTime(timeoutMs);
    unsigned long took = millis() - attemptStart;
    if (epoch == (time_t)-1) {
      Serial.printf("⚠️ TimeManager: NTP attempt timed out after %lums (time() still %ld)\n", took, (long)time(nullptr));
      lastSyncSuccess = false;
      lastAttemptMillis = millis();
      return false;
    }
    Serial.printf("⏱ TimeManager: NTP returned epoch %ld after %lums\n", (long)epoch, took);
    if (epoch >= validThreshold) {
      // Determine whether this is a fresh sync (i.e. NTP updated the clock)
      time_t expected = lastSyncedEpoch + (time_t)((attemptStart - lastSyncMillis) / 1000);
      long diff = (long)epoch - (long)expected;
      if (lastSyncMillis == 0 || llabs(diff) > 2) {
        // Fresh sync: update base epoch and millis
        lastSyncedEpoch = epoch;
        lastSyncMillis = millis();
        lastSyncSuccess = true;
        struct tm tinfo;
        gmtime_r(&epoch, &tinfo);
        char buf[32];
        strftime(buf, sizeof(buf), "%FT%TZ", &tinfo);
        Serial.printf("⏱ TimeManager: NTP sync succeeded: %s\n", buf);
        return true;
      } else {
        // No observable change — this likely means local clock already had correct time
        // Update lastSyncMillis so we don't re-check immediately
        lastSyncSuccess = true; // treat as success for scheduling
        lastSyncMillis = millis();
        // Rate-limit informational logs to avoid spam (use retry interval)
        if ((millis() - lastNoUpdateLogMillis) >= retryIntervalMs) {
          Serial.println("ℹ️ TimeManager: Time check OK — no new NTP update (using local clock)");
          lastNoUpdateLogMillis = millis();
        }
        return false;
      }
    }
    // Received a time but it's invalid/too small
    Serial.printf("⚠️ TimeManager: NTP returned invalid epoch: %ld\n", (long)epoch);
    lastSyncSuccess = false;
    lastAttemptMillis = millis();
    return false;
  }

  // Call from loop() regularly; will trigger a sync when the interval elapsed.
  void update() {
    // Check if NTP sync has just succeeded and update the global flags
    if (lastSyncSuccess && !ntpEverSynced) {
      ntpEverSynced = true;
      ntpCurrentlyFailing = false;
      // Save the fact that we've synced at least once to NVS
      preferences.begin("esp32iot", false);
      preferences.putBool("ntpSynced", true);
      preferences.end();
      Serial.println("✅ NTP sync succeeded - enabling schedule engine (persisted to NVS)");
    }
    
    // Update current failure status
    if (lastSyncSuccess) {
      ntpCurrentlyFailing = false;
    }
    
    // If NTP has never been synced, use aggressive 10-second retry
    // If already synced once, use normal intervals even if temporarily offline
    unsigned long nowMs = millis();
    unsigned long effectiveInterval = !ntpEverSynced ? 10000 : (lastSyncSuccess ? syncIntervalMs : retryIntervalMs);
    
    if (lastSyncSuccess && ntpEverSynced) {
      // Normal operation: periodic re-sync every hour
      if ((nowMs - lastSyncMillis) >= effectiveInterval) {
        bool success = trySync(TIME_SYNC_ATTEMPT_TIMEOUT_MS);
        if (!success) ntpCurrentlyFailing = true;
      }
    } else {
      // Initial sync or recovery: retry with appropriate interval
      if ((nowMs - ntpLastRetryMs) >= effectiveInterval) {
        ntpLastRetryMs = nowMs;
        bool success = trySync(TIME_SYNC_ATTEMPT_TIMEOUT_MS);
        if (!success) ntpCurrentlyFailing = true;
      }
    }
  }

  // Returns the current epoch as maintained locally (advances while offline)
  time_t now() const {
    unsigned long elapsedMs = millis() - lastSyncMillis;
    return lastSyncedEpoch + (time_t)(elapsedMs / 1000);
  }

  // Fill a struct tm with current UTC time
  void getUTCTime(struct tm &out) const {
    time_t t = now();
    gmtime_r(&t, &out);
  }

  // Adjust sync interval (ms)
  void setSyncInterval(unsigned long ms) { syncIntervalMs = ms; }
  // Set a shorter retry interval to use when sync fails (e.g. 10 minutes)
  void setRetryInterval(unsigned long ms) { retryIntervalMs = ms; }

private:
  time_t lastSyncedEpoch;
  unsigned long lastSyncMillis;
  unsigned long syncIntervalMs;
  unsigned long retryIntervalMs;
  bool lastSyncSuccess;
  unsigned long lastAttemptMillis;
  unsigned long lastNoUpdateLogMillis;
  static const time_t validThreshold = 1000000000; // same threshold used above
};

// Global instance
TimeManager timeManager;

// === Schedule engine (placed after TimeManager so it can use its API) ===
// === Schedule state ===
struct ScheduleRow {
  bool enable = false;
  String setting = ""; // "schedule" or "timer"
  long row_id = 0; // Added to track Supabase row id
  // schedule times (time of day)
  int on_h = 0, on_m = 0, on_s = 0;
  int off_h = 0, off_m = 0, off_s = 0;
  // timer durations (seconds)
  unsigned long on_duration_s = 0;
  unsigned long off_duration_s = 0;
  // weekdays: 0=Sun,1=Mon,...6=Sat
  bool weekday[7] = {false, false, false, false, false, false, false};
  // timer runtime state
  bool timer_state = false; // current state during timer mode (true=ON) (unchanged)
  unsigned long last_toggle_ms = 0; // last change timestamp for timer mode
  bool initialized = false; // whether we've initialized timer_state
};

// Array to hold multiple schedules fetched from Supabase
static ScheduleRow scheduleRows[MAX_SCHEDULES];
static int scheduleCount = 0;

// Helper: parse interval strings like "08:00:00" into h,m,s
static void parseIntervalToHMS(const String &s, int &h, int &m, int &sec) {
  h = m = sec = 0;
  if (s.length() == 0) return;
  // Accept formats: HH:MM or HH:MM:SS
  int first = s.indexOf(':');
  int second = s.indexOf(':', first + 1);
  if (first < 0) return;
  h = s.substring(0, first).toInt();
  if (second < 0) {
    m = s.substring(first + 1).toInt();
    sec = 0;
  } else {
    m = s.substring(first + 1, second).toInt();
    sec = s.substring(second + 1).toInt();
  }
}

// Helper: parse interval string to total seconds (for timer durations)
static unsigned long parseIntervalToSeconds(const String &s) {
  int h, m, sec;
  parseIntervalToHMS(s, h, m, sec);
  return (unsigned long)h * 3600UL + (unsigned long)m * 60UL + (unsigned long)sec;
}

// Helpers to compute next ON/OFF epochs (UTC) for schedule rows using local IST
static time_t nextOnForScheduleRow(const ScheduleRow &r, time_t nowUtc) {
  time_t nowLocal = nowUtc + IST_OFFSET_SECONDS;
  struct tm localTm;
  gmtime_r(&nowLocal, &localTm);
  int cur_h = localTm.tm_hour;
  int cur_m = localTm.tm_min;
  int cur_s = localTm.tm_sec;
  // local midnight epoch
  time_t localMidnight = nowLocal - (cur_h * 3600 + cur_m * 60 + cur_s);
  for (int d = 0; d < 7; ++d) {
    int dayIndex = (localTm.tm_wday + d) % 7; // weekday index for candidate day
    if (!r.weekday[dayIndex]) continue;
    time_t candidateLocal = localMidnight + (time_t)d * 86400 + (time_t)r.on_h * 3600 + (time_t)r.on_m * 60 + (time_t)r.on_s;
    if (candidateLocal > nowLocal) {
      // convert local epoch back to UTC
      return candidateLocal - IST_OFFSET_SECONDS;
    }
  }
  // fallback: return 0 if none
  return (time_t)0;
}

static time_t nextOffForScheduleRow(const ScheduleRow &r, time_t nowUtc) {
  time_t nowLocal = nowUtc + IST_OFFSET_SECONDS;
  struct tm localTm;
  gmtime_r(&nowLocal, &localTm);
  int cur_h = localTm.tm_hour;
  int cur_m = localTm.tm_min;
  int cur_s = localTm.tm_sec;
  time_t localMidnight = nowLocal - (cur_h * 3600 + cur_m * 60 + cur_s);
  for (int d = 0; d < 7; ++d) {
    int dayIndex = (localTm.tm_wday + d) % 7;
    if (!r.weekday[dayIndex]) continue;
    time_t candidateLocal = localMidnight + (time_t)d * 86400 + (time_t)r.off_h * 3600 + (time_t)r.off_m * 60 + (time_t)r.off_s;
    if (candidateLocal > nowLocal) {
      return candidateLocal - IST_OFFSET_SECONDS;
    }
  }
  return (time_t)0;
}

// For timer rows compute next change epoch (UTC). If timer_state==true, next change is OFF, else next is ON.
static time_t nextChangeForTimerRow(const ScheduleRow &r, unsigned long nowMs) {
  if (r.on_duration_s == 0 && r.off_duration_s == 0) return (time_t)0;
  unsigned long lastToggle = r.last_toggle_ms;
  if (lastToggle == 0) {
    // not initialized: assume it will toggle after on_duration from now if initialized as ON
    if (r.timer_state) {
      return (time_t)((time(nullptr)) + (time_t)r.on_duration_s);
    } else {
      return (time_t)((time(nullptr)) + (time_t)r.off_duration_s);
    }
  }
  unsigned long elapsed = (nowMs - lastToggle) / 1000UL;
  if (r.timer_state) {
    if (r.on_duration_s > elapsed) return (time_t)(time(nullptr) + (time_t)(r.on_duration_s - elapsed));
    else return (time_t)(time(nullptr));
  } else {
    if (r.off_duration_s > elapsed) return (time_t)(time(nullptr) + (time_t)(r.off_duration_s - elapsed));
    else return (time_t)(time(nullptr));
  }
}

// Compute and print next ON and OFF times (local IST) across all enabled schedules/timers
static void printNextOnOffTimes() {
  if (scheduleCount <= 0) {
    Serial.println("ℹ️ No schedules loaded");
    return;
  }
  time_t nowUtc = timeManager.now();
  if (nowUtc <= 0) {
    Serial.println("⚠️ Time not available — can't compute next events");
    return;
  }
  unsigned long nowMs = millis();

  time_t bestNextOn = (time_t)0;
  time_t bestNextOff = (time_t)0;

  for (int i = 0; i < scheduleCount; ++i) {
    ScheduleRow &r = scheduleRows[i];
    if (!r.enable) continue;
    if (r.setting.equalsIgnoreCase("schedule")) {
      time_t candOn = nextOnForScheduleRow(r, nowUtc);
      time_t candOff = nextOffForScheduleRow(r, nowUtc);
      if (candOn != 0 && (bestNextOn == 0 || candOn < bestNextOn)) bestNextOn = candOn;
      if (candOff != 0 && (bestNextOff == 0 || candOff < bestNextOff)) bestNextOff = candOff;
    } else if (r.setting.equalsIgnoreCase("timer")) {
      // timer rows: derive next on/off from timer_state and durations
      if (r.timer_state) {
        // currently ON; next change is OFF
        time_t candOff = nextChangeForTimerRow(r, nowMs);
        if (candOff != 0 && (bestNextOff == 0 || candOff < bestNextOff)) bestNextOff = candOff;
      } else {
        // currently OFF; next change is ON
        time_t candOn = nextChangeForTimerRow(r, nowMs);
        if (candOn != 0 && (bestNextOn == 0 || candOn < bestNextOn)) bestNextOn = candOn;
      }
    }
  }

  auto printLocal = [&](time_t t, const char *label) {
    if (t == 0) {
      Serial.printf("%s: none\n", label);
      return;
    }
    time_t local = t + IST_OFFSET_SECONDS;
    struct tm lt;
    gmtime_r(&local, &lt);
    char buf[64];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S IST", &lt);
    Serial.printf("%s: %s\n", label, buf);
  };

  Serial.println("=== Next schedule events (device: " + String(deviceId) + ") ===");
  printLocal(bestNextOn, "Next ON");
  printLocal(bestNextOff, "Next OFF");
  Serial.println("=== end next events ===");
}

// Print all ON/OFF events in the upcoming 24 hours (IST) across enabled schedules/timers
static void printNext24hSchedule() {
  if (scheduleCount <= 0) {
    Serial.println("ℹ️ No schedules loaded");
    return;
  }
  time_t nowUtc = timeManager.now();
  if (nowUtc <= 0) {
    Serial.println("⚠️ Time not available — can't compute next events");
    return;
  }
  unsigned long nowMs = millis();
  time_t nowLocal = nowUtc + IST_OFFSET_SECONDS;
  time_t endLocal = nowLocal + 86400; // 24 hours ahead in local time

  const int MAX_EVENTS = 256;
  time_t evTimes[MAX_EVENTS];
  int evTypes[MAX_EVENTS]; // 1 = ON, 0 = OFF
  long evRowId[MAX_EVENTS];
  int evCount = 0;

  // Collect schedule-based events
  for (int i = 0; i < scheduleCount; ++i) {
    ScheduleRow &r = scheduleRows[i];
    if (!r.enable) continue;
    if (r.setting.equalsIgnoreCase("schedule")) {
      // check today and next day in local time
      for (int d = 0; d < 2; ++d) {
        // compute local midnight for today + d
        struct tm lt;
        gmtime_r(&nowLocal, &lt);
        int cur_h = lt.tm_hour, cur_m = lt.tm_min, cur_s = lt.tm_sec;
        time_t localMidnight = nowLocal - (cur_h * 3600 + cur_m * 60 + cur_s) + (time_t)d * 86400;
        int dayIndex = (lt.tm_wday + d) % 7;
        if (r.weekday[dayIndex]) {
          time_t onLocal = localMidnight + (time_t)r.on_h * 3600 + (time_t)r.on_m * 60 + (time_t)r.on_s;
          time_t offLocal = localMidnight + (time_t)r.off_h * 3600 + (time_t)r.off_m * 60 + (time_t)r.off_s;
          if (onLocal > nowLocal && onLocal <= endLocal && evCount < MAX_EVENTS) {
            evTimes[evCount] = onLocal - IST_OFFSET_SECONDS; // store as UTC
            evTypes[evCount] = 1;
            evRowId[evCount] = r.row_id;
            evCount++;
          }
          if (offLocal > nowLocal && offLocal <= endLocal && evCount < MAX_EVENTS) {
            evTimes[evCount] = offLocal - IST_OFFSET_SECONDS;
            evTypes[evCount] = 0;
            evRowId[evCount] = r.row_id;
            evCount++;
          }
        }
      }
    } else if (r.setting.equalsIgnoreCase("timer")) {
      // Simulate toggles starting from current state up to 24 hours
      bool curState = r.timer_state;
      unsigned long lastToggle = r.last_toggle_ms;
      unsigned long simNowMs = nowMs;
      unsigned long simLastToggleMs = lastToggle ? lastToggle : simNowMs;
      time_t simEventUtc = nowUtc;
      // limit to avoid runaway
      int iter = 0;
      while (evCount < MAX_EVENTS && iter < 64) {
        iter++;
        unsigned long elapsed = (simNowMs - simLastToggleMs) / 1000UL;
        unsigned long remaining = 0;
        if (curState) remaining = (r.on_duration_s > elapsed) ? (r.on_duration_s - elapsed) : 0;
        else remaining = (r.off_duration_s > elapsed) ? (r.off_duration_s - elapsed) : 0;
        time_t nextUtc;
        if (remaining == 0) {
          nextUtc = time(nullptr); // immediate toggle
        } else {
          nextUtc = nowUtc + (time_t)remaining;
        }
        time_t nextLocal = nextUtc + IST_OFFSET_SECONDS;
        if (nextLocal > nowLocal && nextLocal <= endLocal) {
          if (evCount < MAX_EVENTS) {
            evTimes[evCount] = nextUtc;
            evTypes[evCount] = curState ? 0 : 1; // if currently ON, next is OFF
            evRowId[evCount] = r.row_id;
            evCount++;
          }
        } else if (nextLocal > endLocal) {
          break;
        }
        // advance simulation
        simLastToggleMs = simNowMs + remaining * 1000UL;
        simNowMs = simLastToggleMs;
        nowUtc = nextUtc;
        curState = !curState;
      }
    }
  }

  // Simple insertion sort by evTimes
  for (int i = 1; i < evCount; ++i) {
    time_t keyT = evTimes[i];
    int keyType = evTypes[i];
    long keyId = evRowId[i];
    int j = i - 1;
    while (j >= 0 && evTimes[j] > keyT) {
      evTimes[j + 1] = evTimes[j];
      evTypes[j + 1] = evTypes[j];
      evRowId[j + 1] = evRowId[j];
      j--;
    }
    evTimes[j + 1] = keyT;
    evTypes[j + 1] = keyType;
    evRowId[j + 1] = keyId;
  }

  Serial.println("=== Upcoming events (next 24h) ===");
  for (int i = 0; i < evCount; ++i) {
    time_t local = evTimes[i] + IST_OFFSET_SECONDS;
    struct tm lt;
    gmtime_r(&local, &lt);
    char buf[64];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S IST", &lt);
    Serial.printf("%s - %s (row id: %ld)\n", buf, evTypes[i] ? "ON" : "OFF", evRowId[i]);
  }
  if (evCount == 0) Serial.println("No events in the next 24 hours.");
  Serial.println("=== end upcoming events ===");
}

// Fetch all schedules for this device from Supabase and populate scheduleRows[]
static void fetchSchedulesFromSupabase() {
  // Preserve previous runtime state to avoid restarting timers on every fetch
  ScheduleRow prevRows[MAX_SCHEDULES];
  int prevCount = scheduleCount;
  for (int i = 0; i < prevCount && i < MAX_SCHEDULES; ++i) prevRows[i] = scheduleRows[i];
  
  // If WiFi is not connected, keep existing schedules (work offline with cached schedules)
  if (WiFi.status() != WL_CONNECTED) {
    if (scheduleCount > 0) {
      Serial.println("📡 WiFi offline - using cached schedules");
    }
    return;
  }
  
  scheduleCount = 0;
  HTTPClient http;
  String url = String(getURLschedule) + "?sensor_id=eq." + String(deviceId) + "&target=eq.relay1&order=id.asc";
  http.begin(url);
  http.addHeader("apikey", apikey);
  http.addHeader("Authorization", "Bearer " + String(apikey));
  int code = http.GET();
  if (code != 200) {
    http.end();
    // Restore previous schedules on fetch failure
    scheduleCount = prevCount;
    for (int i = 0; i < prevCount && i < MAX_SCHEDULES; ++i) scheduleRows[i] = prevRows[i];
    Serial.printf("⚠️ Schedule fetch failed (HTTP %d) - using cached schedules\n", code);
    return;
  }
  String resp = http.getString();
  
  // Track egress (response from Supabase)
  unsigned long requestSize = url.length() + 100; // Approximate request size
  unsigned long responseSize = resp.length();
  trackDataUsage("fetchSchedulesFromSupabase", requestSize, responseSize);
  
  http.end();

  // Use ArduinoJson to parse the array of schedule rows
  const size_t capacity = 8192;
  DynamicJsonDocument doc(capacity);
  DeserializationError err = deserializeJson(doc, resp);
  if (err) {
    Serial.printf("❌ fetchSchedulesFromSupabase: JSON parse failed: %s\n", err.c_str());
    // Restore previous schedules on parse failure
    scheduleCount = prevCount;
    for (int i = 0; i < prevCount && i < MAX_SCHEDULES; ++i) scheduleRows[i] = prevRows[i];
    return;
  }

  if (!doc.is<JsonArray>()) return;
  JsonArray arr = doc.as<JsonArray>();
  for (JsonObject obj : arr) {
    if (scheduleCount >= MAX_SCHEDULES) break;
    ScheduleRow &r = scheduleRows[scheduleCount];
    r.enable = obj["enable"] | false;
    r.row_id = obj["id"] | 0;
    const char *s = obj["setting"] | "";
    r.setting = String(s);

    // parse timer_on_duration / timer_off_duration as strings when present
    if (!obj["timer_on_duration"].isNull()) {
      String val = String((const char *)obj["timer_on_duration"]);
      parseIntervalToHMS(val, r.on_h, r.on_m, r.on_s);
      r.on_duration_s = parseIntervalToSeconds(val);
    } else {
      r.on_h = r.on_m = r.on_s = 0;
      r.on_duration_s = 0;
    }
    if (!obj["timer_off_duration"].isNull()) {
      String val = String((const char *)obj["timer_off_duration"]);
      parseIntervalToHMS(val, r.off_h, r.off_m, r.off_s);
      r.off_duration_s = parseIntervalToSeconds(val);
    } else {
      r.off_h = r.off_m = r.off_s = 0;
      r.off_duration_s = 0;
    }

    // weekdays
    r.weekday[1] = obj["mon"] | false;
    r.weekday[2] = obj["tue"] | false;
    r.weekday[3] = obj["wed"] | false;
    r.weekday[4] = obj["thu"] | false;
    r.weekday[5] = obj["fri"] | false;
    r.weekday[6] = obj["sat"] | false;
    r.weekday[0] = obj["sun"] | false;

    // Attempt to restore runtime state from previous fetch (match by row_id)
    bool restored = false;
    for (int k = 0; k < prevCount; ++k) {
      if (prevRows[k].row_id != 0 && prevRows[k].row_id == r.row_id) {
        r.timer_state = prevRows[k].timer_state;
        r.last_toggle_ms = prevRows[k].last_toggle_ms;
        r.initialized = prevRows[k].initialized;
        restored = true;
        break;
      }
    }
    if (!restored) {
      r.timer_state = false;
      r.last_toggle_ms = 0;
      r.initialized = false;
    }

    scheduleCount++;
  }
}

// Print the in-memory schedule table with detailed info
static void printScheduleTableFromMemory() {
  Serial.println("\n📋 === SCHEDULE TABLE (IN-MEMORY) ===");
  Serial.printf("Total schedules loaded: %d\n", scheduleCount);
  
  if (scheduleCount == 0) {
    Serial.println("(empty)");
    Serial.println("📋 === END ===\n");
    return;
  }
  
  for (int i = 0; i < scheduleCount; ++i) {
    ScheduleRow &r = scheduleRows[i];
    Serial.printf("\n[%d] ID=%ld, Enable=%d, Type=%s\n", i, r.row_id, r.enable, r.setting.c_str());
    
    if (r.setting.equalsIgnoreCase("schedule")) {
      // Print days
      Serial.print("    Days: ");
      const char* dayNames[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
      for (int d = 0; d < 7; d++) {
        if (r.weekday[d]) Serial.printf("%s ", dayNames[d]);
      }
      Serial.println();
      Serial.printf("    ON: %02d:%02d, OFF: %02d:%02d\n", r.on_h, r.on_m, r.off_h, r.off_m);
    } else if (r.setting.equalsIgnoreCase("timer")) {
      Serial.printf("    ON Duration: %dh %dm %ds (total %lu sec)\n", r.on_h, r.on_m, r.on_s, r.on_duration_s);
      Serial.printf("    OFF Duration: %dh %dm %ds (total %lu sec)\n", r.off_h, r.off_m, r.off_s, r.off_duration_s);
      Serial.printf("    Current State: %s, Initialized: %d\n", r.timer_state ? "ON" : "OFF", r.initialized);
    }
  }
  Serial.println("\n📋 === END ===\n");
}

// Read the entire schedule table from Supabase and print to Serial

// Find and print next schedule/timer event
static void printNextEvent() {
  if (scheduleCount <= 0) {
    Serial.println("ℹ️ No schedules or timers configured");
    return;
  }

  time_t epoch = timeManager.now();
  if (epoch <= 0) return;
  time_t epochLocal = epoch + IST_OFFSET_SECONDS;
  struct tm t;
  gmtime_r(&epochLocal, &t);
  int today = t.tm_wday;
  int nowMinutes = t.tm_hour * 60 + t.tm_min;
  unsigned long nowMs = millis();
  
  // Print today's date and day
  Serial.printf("\n📅 === TODAY: %s, %04d-%02d-%02d %02d:%02d ===\n",
                (const char*[]){"Sun","Mon","Tue","Wed","Thu","Fri","Sat"}[today],
                t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
                t.tm_hour, t.tm_min);

  time_t nextScheduleEvent = 0;
  time_t nextTimerEvent = 0;
  char scheduleDesc[128] = "";
  char timerDesc[128] = "";

  // Find next schedule event
  for (int i = 0; i < scheduleCount; ++i) {
    ScheduleRow &r = scheduleRows[i];
    if (!r.enable || !r.setting.equalsIgnoreCase("schedule")) continue;

    int enabledDayCount = 0;
    for (int d = 0; d < 7; d++) {
      if (r.weekday[d]) enabledDayCount++;
    }
    if (enabledDayCount == 0) continue;

    int onMinutes = r.on_h * 60 + r.on_m;
    int offMinutes = r.off_h * 60 + r.off_m;

    // Determine if we're currently in this schedule window
    bool currentlyInWindow = false;
    
    // If ON < OFF (same-day schedule), treat as daily repeating schedule regardless of enabled day count
    // Only use multi-day span logic when ON > OFF (overnight schedule that spans multiple days)
    if (onMinutes < offMinutes) {
      // Same-day schedule: check if today is enabled and we're within the time window
      if (r.weekday[today]) {
        currentlyInWindow = (nowMinutes >= onMinutes && nowMinutes < offMinutes);
      }
    } else if (onMinutes > offMinutes) {
      // Overnight schedule: spans multiple days
      if (enabledDayCount == 1) {
        // Single day overnight schedule
        int enabledDay = -1;
        for (int d = 0; d < 7; d++) {
          if (r.weekday[d]) { enabledDay = d; break; }
        }
        if (today == enabledDay) {
          currentlyInWindow = (nowMinutes >= onMinutes || nowMinutes < offMinutes);
        }
      } else {
        // Multi-day overnight schedule: find span
        int spanStartDay = -1, spanEndDay = -1;
        for (int offset = 0; offset < 7; offset++) {
          int prevDay = (7 + offset - 1) % 7;
          int curDay = offset;
          if (!r.weekday[prevDay] && r.weekday[curDay]) {
            spanStartDay = curDay;
            break;
          }
        }
        if (spanStartDay == -1) spanStartDay = 0;
        for (int offset = 0; offset < 7; offset++) {
          int curDay = (spanStartDay + offset) % 7;
          int nextDay = (spanStartDay + offset + 1) % 7;
          if (r.weekday[curDay] && !r.weekday[nextDay]) {
            spanEndDay = curDay;
            break;
          }
        }
        if (spanEndDay == -1) spanEndDay = (spanStartDay + 6) % 7;

        bool todayInSpan = r.weekday[today];
        if (todayInSpan) {
          bool isFirstDay = (today == spanStartDay);
          bool isLastDay = (today == spanEndDay);
          if (isLastDay) {
            currentlyInWindow = (nowMinutes < offMinutes);
          } else if (isFirstDay) {
            currentlyInWindow = (nowMinutes >= onMinutes);
          } else {
            currentlyInWindow = true;  // Middle day
          }
        }
      }
    }

    // Find next event for this schedule
    time_t candidateEvent = 0;
    char candidateDesc[64] = "";

    if (currentlyInWindow) {
      // Currently in window, next event is OFF
      struct tm boundaryTm = t;
      boundaryTm.tm_hour = offMinutes / 60;
      boundaryTm.tm_min = offMinutes % 60;
      boundaryTm.tm_sec = 0;

      if (onMinutes < offMinutes) {
        // Same-day schedule: OFF is today
        candidateEvent = mktime(&boundaryTm);
      } else {
        // Overnight schedule: OFF might be today or later depending on span
        if (enabledDayCount == 1) {
          candidateEvent = mktime(&boundaryTm);
        } else {
          // Multi-day: find last day of span
          int spanStartDay = -1, spanEndDay = -1;
          for (int offset = 0; offset < 7; offset++) {
            int prevDay = (7 + offset - 1) % 7;
            int curDay = offset;
            if (!r.weekday[prevDay] && r.weekday[curDay]) {
              spanStartDay = curDay;
              break;
            }
          }
          if (spanStartDay == -1) spanStartDay = 0;
          for (int offset = 0; offset < 7; offset++) {
            int curDay = (spanStartDay + offset) % 7;
            int nextDay = (spanStartDay + offset + 1) % 7;
            if (r.weekday[curDay] && !r.weekday[nextDay]) {
              spanEndDay = curDay;
              break;
            }
          }
          if (spanEndDay == -1) spanEndDay = (spanStartDay + 6) % 7;

          int daysUntilEnd = (spanEndDay >= today) ? (spanEndDay - today) : (7 - today + spanEndDay);
          boundaryTm = t;
          boundaryTm.tm_mday += daysUntilEnd;
          boundaryTm.tm_hour = offMinutes / 60;
          boundaryTm.tm_min = offMinutes % 60;
          boundaryTm.tm_sec = 0;
          candidateEvent = mktime(&boundaryTm);
        }
      }
      snprintf(candidateDesc, sizeof(candidateDesc), "Schedule OFF");
    } else {
      // Not in window, next event is ON
      struct tm boundaryTm = t;
      boundaryTm.tm_hour = onMinutes / 60;
      boundaryTm.tm_min = onMinutes % 60;
      boundaryTm.tm_sec = 0;

      if (onMinutes < offMinutes) {
        // Same-day schedule: find next enabled day
        // First check if today is enabled and time hasn't passed yet
        if (r.weekday[today] && nowMinutes < onMinutes) {
          candidateEvent = mktime(&boundaryTm);
        } else {
          // Find next enabled day
          int daysToAdd = 0;
          for (int d = 1; d <= 7; d++) {
            int checkDay = (today + d) % 7;
            if (r.weekday[checkDay]) {
              daysToAdd = d;
              break;
            }
          }
          if (daysToAdd > 0) {
            boundaryTm.tm_mday += daysToAdd;
            candidateEvent = mktime(&boundaryTm);
          }
        }
      } else {
        // Overnight schedule: find next start day in span
        int spanStartDay = -1;
        for (int offset = 0; offset < 7; offset++) {
          int prevDay = (7 + offset - 1) % 7;
          int curDay = offset;
          if (!r.weekday[prevDay] && r.weekday[curDay]) {
            spanStartDay = curDay;
            break;
          }
        }
        if (spanStartDay == -1) spanStartDay = 0;

        int daysUntilStart = (spanStartDay >= today) ? (spanStartDay - today) : (7 - today + spanStartDay);
        
        // If same day but time has passed, it's next week
        if (daysUntilStart == 0 && nowMinutes >= onMinutes) {
          daysUntilStart = 7;
        }

        boundaryTm.tm_mday += daysUntilStart;
        candidateEvent = mktime(&boundaryTm);
      }
      snprintf(candidateDesc, sizeof(candidateDesc), "Schedule ON");
    }

    // Keep the earliest event
    if (candidateEvent > 0) {
      if (nextScheduleEvent == 0 || candidateEvent < nextScheduleEvent) {
        nextScheduleEvent = candidateEvent;
        snprintf(scheduleDesc, sizeof(scheduleDesc), "%s", candidateDesc);
      }
    }
  }

  // Find next timer event
  for (int i = 0; i < scheduleCount; ++i) {
    ScheduleRow &r = scheduleRows[i];
    if (!r.enable || !r.setting.equalsIgnoreCase("timer")) continue;

    if (!r.initialized) {
      r.timer_state = true;
      r.last_toggle_ms = nowMs;
      r.initialized = true;
    }

    unsigned long elapsed = (nowMs - r.last_toggle_ms) / 1000UL;
    unsigned long remainingSeconds = 0;

    if (r.timer_state) {
      if (r.on_duration_s > 0) {
        if (elapsed >= r.on_duration_s) {
          remainingSeconds = 0;  // Should toggle now
        } else {
          remainingSeconds = r.on_duration_s - elapsed;
        }
      }
    } else {
      if (r.off_duration_s > 0) {
        if (elapsed >= r.off_duration_s) {
          remainingSeconds = 0;  // Should toggle now
        } else {
          remainingSeconds = r.off_duration_s - elapsed;
        }
      }
    }

    if (remainingSeconds > 0) {
      time_t timerEvent = epochLocal + remainingSeconds;
      if (nextTimerEvent == 0 || timerEvent < nextTimerEvent) {
        nextTimerEvent = timerEvent;
        snprintf(timerDesc, sizeof(timerDesc), "Timer %s (in %lu sec)", r.timer_state ? "OFF" : "ON", remainingSeconds);
      }
    }
  }

  // Print next event
  Serial.println("\n📅 === NEXT EVENTS ===");
  
  if (nextScheduleEvent > 0) {
    struct tm eventTm;
    gmtime_r(&nextScheduleEvent, &eventTm);
    Serial.printf("  Schedule: %s at %s %02d:%02d\n", scheduleDesc,
                  (const char*[]){"Sun","Mon","Tue","Wed","Thu","Fri","Sat"}[eventTm.tm_wday],
                  eventTm.tm_hour, eventTm.tm_min);
  }

  if (nextTimerEvent > 0) {
    struct tm eventTm;
    gmtime_r(&nextTimerEvent, &eventTm);
    Serial.printf("  Timer: %s at %s %02d:%02d\n", timerDesc,
                  (const char*[]){"Sun","Mon","Tue","Wed","Thu","Fri","Sat"}[eventTm.tm_wday],
                  eventTm.tm_hour, eventTm.tm_min);
  }

  if (nextScheduleEvent == 0 && nextTimerEvent == 0) {
    Serial.println("  No upcoming events");
  }
  Serial.println("📅 === END NEXT EVENTS ===\n");
}

// Evaluate and apply schedule; call every 1 minute
static void checkSchedule() {
  if (!scheduleAllowed) return;
  
  // NTP time sync is mandatory for initial operation
  // Once synced, scheduler can work offline with local clock (even if NTP temporarily fails)
  if (!ntpEverSynced) {
    return;
  }

  // Fetch all schedules for this device (works offline with cached schedules)
  fetchSchedulesFromSupabase();
  
  // Debug: Print the schedule table from memory
  printScheduleTableFromMemory();
  
  // Debug: Print the next events
  printNextEvent();
  
  // If no schedules exist, maintain current relay state
  // Don't clear manual override - that state should persist
  if (scheduleCount <= 0) {
    Serial.println("ℹ️ No schedules/timers - maintaining current relay state");
    return;
  }

  time_t epoch = timeManager.now();
  if (epoch <= 0) return;
  // Use local IST time for schedule comparisons
  time_t epochUtc = epoch;
  time_t epochLocal = epochUtc + IST_OFFSET_SECONDS;
  struct tm t;
  gmtime_r(&epochLocal, &t);
  int today = t.tm_wday; // 0=Sun
  int cur_h = t.tm_hour;
  int cur_m = t.tm_min;
  int nowMinutes = cur_h * 60 + cur_m;

  // Check if manual override is still pending
  // Manual override expires when we reach or pass the next schedule/timer boundary (epoch-based)
  if (manualOverridePending && manualOverrideExpiryEpoch > 0) {
    if (epochLocal >= manualOverrideExpiryEpoch) {
      manualOverridePending = false;
      struct tm expiryTm;
      gmtime_r(&manualOverrideExpiryEpoch, &expiryTm);
      Serial.printf("⏱ Manual override expired at schedule boundary (%s %02d:%02d): schedule control resumed\n", 
                    (const char*[]){"Sun","Mon","Tue","Wed","Thu","Fri","Sat"}[expiryTm.tm_wday],
                    expiryTm.tm_hour, expiryTm.tm_min);
    }
  }

  // Note: Don't return early for manual override - we need to process timer state changes
  // Timer state change will also clear the override (handled below)

  unsigned long nowMs = millis();

  // Determine if schedule should be in ON or OFF state based on current time
  // Multiple schedules are ORed: any schedule in ON window = effective schedule ON
  bool scheduleShouldBeOn = false;
  int scheduleCount_active = 0;

  // Multiple timers are ORed: any timer ON = effective timer ON
  bool timerShouldBeOn = false;
  int timerCount_active = 0;

  for (int i = 0; i < scheduleCount; ++i) {
    ScheduleRow &r = scheduleRows[i];
    if (!r.enable) continue;     
    if (r.setting.equalsIgnoreCase("schedule")) {
      // Multi-day schedule support:
      // Find the contiguous span of enabled days
      // For week-wrapping schedules (e.g., Fri-Mon), we need to find where the span starts/ends
      
      int enabledDayCount = 0;
      for (int d = 0; d < 7; ++d) {
        if (r.weekday[d]) enabledDayCount++;
      }
      
      if (enabledDayCount == 0) continue; // No days enabled
      
      int onMinutes = r.on_h * 60 + r.on_m;
      int offMinutes = r.off_h * 60 + r.off_m;
      
      bool isInWindow = false;
      
      // If ON < OFF (same-day schedule), treat as daily repeating schedule regardless of enabled day count
      // Only use multi-day span logic when ON > OFF (overnight schedule that spans multiple days)
      if (onMinutes < offMinutes) {
        // Same-day schedule: check if today is enabled and we're within the time window
        if (r.weekday[today]) {
          if (nowMinutes >= onMinutes && nowMinutes < offMinutes) {
            isInWindow = true;
          }
        }
      } else if (onMinutes > offMinutes) {
        // Overnight schedule: requires span logic
        if (enabledDayCount == 1) {
          // Single day overnight schedule
          int enabledDay = -1;
          for (int d = 0; d < 7; ++d) {
            if (r.weekday[d]) { enabledDay = d; break; }
          }
          if (today == enabledDay) {
            if (nowMinutes >= onMinutes || nowMinutes < offMinutes) {
              isInWindow = true;
            }
          }
        } else {
          // Multi-day overnight schedule: find contiguous span
          int spanStartDay = -1;
          int spanEndDay = -1;
          
          // Look for a gap (disabled day followed by enabled day) to find span start
          for (int offset = 0; offset < 7; ++offset) {
            int prevDay = (7 + offset - 1) % 7;
            int curDay = offset;
            if (!r.weekday[prevDay] && r.weekday[curDay]) {
              spanStartDay = curDay;
              break;
            }
          }
          
          // If no gap found (all 7 days enabled), span starts at day 0
          if (spanStartDay == -1) spanStartDay = 0;
          
          // Find span end (last enabled day before a gap)
          for (int offset = 0; offset < 7; ++offset) {
            int curDay = (spanStartDay + offset) % 7;
            int nextDay = (spanStartDay + offset + 1) % 7;
            if (r.weekday[curDay] && !r.weekday[nextDay]) {
              spanEndDay = curDay;
              break;
            }
          }
          
          // If no gap found after start, all days are enabled
          if (spanEndDay == -1) spanEndDay = (spanStartDay + 6) % 7;
          
          // Check if today is in the span
          bool todayInSpan = r.weekday[today];
          bool isFirstDay = (today == spanStartDay);
          bool isLastDay = (today == spanEndDay);
          bool isMiddleDay = todayInSpan && !isFirstDay && !isLastDay;
          
          if (todayInSpan) {
            if (isMiddleDay) {
              // Middle days are always ON (full 24 hours)
              isInWindow = true;
            } else if (isFirstDay && isLastDay) {
              // Same day is both first and last (shouldn't happen with overnight schedule)
              if (nowMinutes >= onMinutes || nowMinutes < offMinutes) isInWindow = true;
            } else if (isFirstDay) {
              // First day: ON from onMinutes until midnight
              if (nowMinutes >= onMinutes) {
                isInWindow = true;
              }
            } else if (isLastDay) {
              // Last day: ON from midnight until offMinutes
              if (nowMinutes < offMinutes) {
                isInWindow = true;
              }
            }
          }
        }
      }
      
      // OR logic: any schedule in ON window makes effective schedule ON
      if (isInWindow) {
        scheduleShouldBeOn = true;
        scheduleCount_active++;
      }
    } else if (r.setting.equalsIgnoreCase("timer")) {
      // Timers always run regardless of schedule state (for AND logic)
      // initialize timer state if needed
      if (!r.initialized) {
        r.timer_state = true;
        r.last_toggle_ms = nowMs;
        r.initialized = true;
      }
      unsigned long elapsed = (nowMs - r.last_toggle_ms) / 1000UL;
      bool timerStateChanged = false;
      if (r.timer_state) {
        if (r.on_duration_s > 0 && elapsed >= r.on_duration_s) {
          r.timer_state = false;
          r.last_toggle_ms = nowMs;
          timerStateChanged = true;
          Serial.printf("⏱ Timer row %ld: timer OFF at %lu ms\n", r.row_id, nowMs);
        }
      } else {
        if (r.off_duration_s > 0 && elapsed >= r.off_duration_s) {
          r.timer_state = true;
          r.last_toggle_ms = nowMs;
          timerStateChanged = true;
          Serial.printf("⏱ Timer row %ld: timer ON at %lu ms\n", r.row_id, nowMs);
        }
      }
      // If timer state changed and manual override was active, clear override
      if (timerStateChanged && manualOverridePending) {
        manualOverridePending = false;
        manualOverrideExpiryEpoch = 0;
        Serial.println("⏱ Manual override expired due to timer state change");
      }
      // OR logic: any timer ON makes effective timer ON
      if (r.timer_state) {
        timerShouldBeOn = true;
        timerCount_active++;
      }
    }
    Serial.printf("[DEBUG] Row %d: setting=%s, timer_state=%d, initialized=%d, last_toggle_ms=%lu, on_duration_s=%lu, off_duration_s=%lu\n", i, r.setting.c_str(), r.timer_state, r.initialized, r.last_toggle_ms, r.on_duration_s, r.off_duration_s);
  }

  bool desired = relayState1; // default: no change
  bool hasAnySchedule = false;
  bool hasAnyTimer = false;
  
  // Count enabled schedules and timers
  for (int i = 0; i < scheduleCount; ++i) {
    if (scheduleRows[i].enable) {
      if (scheduleRows[i].setting.equalsIgnoreCase("schedule")) {
        hasAnySchedule = true;
      } else if (scheduleRows[i].setting.equalsIgnoreCase("timer")) {
        hasAnyTimer = true;
      }
    }
  }

  // Apply logic:
  // - Multiple schedules: ORed together (scheduleShouldBeOn already computed with OR)
  // - Multiple timers: ORed together (timerShouldBeOn already computed with OR)
  // - Final: (Schedule1 OR Schedule2 OR ...) AND (Timer1 OR Timer2 OR ...)
  
  if (scheduleCount_active > 1) {
    Serial.printf("ℹ️ %d schedule windows active (ORed together)\n", scheduleCount_active);
  }
  if (timerCount_active > 1) {
    Serial.printf("ℹ️ %d timers ON (ORed together)\n", timerCount_active);
  }
  
  if (hasAnySchedule && hasAnyTimer) {
    // AND Logic: Relay = (Schedule1 OR Schedule2 OR ...) AND (Timer1 OR Timer2 OR ...)
    desired = scheduleShouldBeOn && timerShouldBeOn;
    Serial.printf("⏱ AND mode: EffectiveSchedule=%s, EffectiveTimer=%s => Relay=%s\n", 
                  scheduleShouldBeOn ? "ON" : "OFF",
                  timerShouldBeOn ? "ON" : "OFF",
                  desired ? "ON" : "OFF");
  } else if (hasAnySchedule) {
    // Schedule-only mode: relay follows effective schedule (ORed)
    desired = scheduleShouldBeOn;
    Serial.printf("⏱ Schedule-only mode: relay=%s\n", desired ? "ON" : "OFF");
  } else if (hasAnyTimer) {
    // Timer-only mode: relay follows effective timer (ORed)
    desired = timerShouldBeOn;
    Serial.printf("⏱ Timer-only mode: relay=%s\n", desired ? "ON" : "OFF");
  } else {
    // No schedules or timers - maintain current state
    Serial.println("ℹ️ No active schedules or timers - maintaining current relay state");
  }

  // If manual override is active, don't change relay state (keep manual state)
  if (manualOverridePending) {
    Serial.printf("⏱ Manual override active: keeping relay %s (auto would be %s)\n", 
                  relayState1 ? "ON" : "OFF",
                  desired ? "ON" : "OFF");
    return;
  }

  // IMMEDIATE COMPLIANCE: Always update relay to match effective state
  // Don't wait for transitions - if effective state differs from relay, change it now
  if (desired != relayState1) {
    relayState1 = desired;
    digitalWrite(RELAY1_PIN, relayState1 ? HIGH : LOW);
    Serial.printf("🔁 Relay updated to effective state: %s\n", relayState1 ? "ON" : "OFF");    
  }
}

// === Setup ===
void setup() {
  // Initialize NVS (Non-Volatile Storage) for persisting relay state and NTP sync status
  preferences.begin("esp32iot", false);
  
  delay(5);
  pinMode(ONE_WIRE_BUS_1, INPUT_PULLUP);
  pinMode(ONE_WIRE_BUS_2, INPUT_PULLUP);
  pinMode(RELAY1_PIN, OUTPUT);

  // Restore relay state from NVS
  relayState1 = preferences.getBool("relayState", false);
  digitalWrite(RELAY1_PIN, relayState1 ? HIGH : LOW);
  
  // Restore NTP sync status from NVS (if we synced before, we can work offline)
  ntpEverSynced = preferences.getBool("ntpSynced", false);
  preferences.end();
  
  Serial.begin(460800);
  delay(1000);
  Serial.printf("🔁 Relay restored from NVS: %s\n", relayState1 ? "ON" : "OFF");
  if (ntpEverSynced) {
    Serial.println("✅ NTP was previously synced - scheduler can work offline");
  }

  sensor1.begin();
  sensor2.begin();

  connectWiFi();

#if !RELAY_API_ONLY
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");
  // Initialize TimeManager: it will attempt an immediate sync and then
  // keep local time running and sync every `TIME_SYNC_INTERVAL_MS`.
  timeManager.begin(TIME_SYNC_INTERVAL_MS, TIME_INITIAL_TIMEOUT_MS);
  // Ensure retry interval is set (used when sync fails)
  timeManager.setRetryInterval(TIME_RETRY_INTERVAL_MS);
  time_t nowEpoch = timeManager.now();
  if (nowEpoch >= 1000000000) {
    struct tm tinfo;
    timeManager.getUTCTime(tinfo);
    char buf[32];
    strftime(buf, sizeof(buf), "%FT%TZ", &tinfo);
    Serial.printf("⏱ Time initialized: %s\n", buf);
    scheduleAllowed = true;
  } else {
    Serial.println("⚠️ Time not synchronized yet; attempting one additional NTP sync at boot...");
    // Try one more sync attempt at boot to obtain a valid time
    if (timeManager.trySync(TIME_INITIAL_TIMEOUT_MS)) {
      time_t newEpoch = timeManager.now();
      if (newEpoch >= 1000000000) {
        struct tm tinfo;
        timeManager.getUTCTime(tinfo);
        char buf[32];
        strftime(buf, sizeof(buf), "%FT%TZ", &tinfo);
        Serial.printf("✅ NTP sync success on second attempt: %s\n", buf);
        scheduleAllowed = true;
      } else {
        Serial.println("❌ NTP sync returned invalid time on second attempt; schedule disabled until NTP available");
        scheduleAllowed = false;
      }
    } else {
      Serial.println("❌ Additional NTP sync attempt failed; schedule disabled until NTP available");
      scheduleAllowed = false;
    }
  }
#endif  // !RELAY_API_ONLY

#if RELAY_API_ONLY
  fetchRelayCommands();  // initial relay state from the new relay API
#else
  relayState1 = fetchRelayCommand(deviceId, "relay1", relayState1);
  digitalWrite(RELAY1_PIN, relayState1 ? HIGH : LOW);
  Serial.printf("🔄 Relay updated from Supabase: %s\n", relayState1 ? "ON" : "OFF");
#endif

  lastRelayCheck = millis();
  lastSensorSend = millis();
  lastWiFiCheck = millis();
  lastNVSWrite = millis();
}

// === Main Loop ===
void loop() {
  unsigned long now = millis();

  if (now - lastWiFiCheck > 10000) {
    lastWiFiCheck = now;
    checkWiFi();
  }

#if RELAY_API_ONLY
  // Relay-API mode: poll relay1 from the new API + push sensors; the Supabase
  // schedule/command engine below is bypassed. Set RELAY_API_ONLY 0 to restore it.
  if (now - lastRelayCheck >= RELAY_POLL_MS) {
    lastRelayCheck = now;
    fetchRelayCommands();
  }
  if (now - lastSensorSend >= SEND_INTERVAL_MS) {
    lastSensorSend = now;
    float t1, t2;
    bool valid1, valid2;
    readSensors(t1, t2, valid1, valid2);
    sendSensorData(deviceId, t1, t2, valid1, valid2, relayState1);
  }
  if (now - lastNVSWrite >= 10000) {
    lastNVSWrite = now;
    preferences.begin("esp32iot", false);
    bool stored = preferences.getBool("relayState", false);
    if (stored != relayState1) {
      preferences.putBool("relayState", relayState1);
      Serial.println("💾 Relay state saved to NVS.");
    }
    preferences.end();
  }
  delay(10);
  return;
#endif

  // Update TimeManager so it can perform hourly syncs (or keep local time running)
  timeManager.update();

  // If schedule was disabled at boot, enable it when NTP becomes available
  // or if we've synced before (can work offline with local clock)
  if (!scheduleAllowed) {
    time_t maybe = timeManager.now();
    if (maybe >= 1000000000 || ntpEverSynced) {
      scheduleAllowed = true;
      Serial.println("✅ Schedule engine enabled (NTP available or previously synced)");
    }
  }

  // Schedule check: run every 1 minute
  if (now - lastScheduleCheck >= 60000UL) {
    lastScheduleCheck = now;
    checkSchedule();
  }
  
  // Print data usage statistics every 1 minute
  printDataUsageStats();

  // Periodically dump the schedule table and next event to Serial (non-blocking)
  // Uses in-memory cached schedules (no additional API calls)
  if (now - lastScheduleDump >= SCHEDULE_DUMP_INTERVAL_MS) {
    lastScheduleDump = now;
    printScheduleTableFromMemory();  // Print from cached data (no API call)
    printNextEvent();  // Print next schedule/timer event (no API call)
  }

  if (now - lastRelayCheck >= 20000) {
    lastRelayCheck = now;
    bool newState = fetchRelayCommand(deviceId, "relay1", relayState1);
    if (newState != relayState1) {
      // Manual command detected. Set override flag.
      // Override expires on next automatic state change (timer toggle OR schedule boundary)
      
      if (scheduleCount > 0) {
        // Find the next schedule boundary (for schedule-based expiry)
        // Search up to 7 days ahead for multi-day schedule support
        time_t epochUtc = timeManager.now();
        time_t epochLocal = epochUtc + IST_OFFSET_SECONDS;
        struct tm t;
        gmtime_r(&epochLocal, &t);
        int nowMinutes = t.tm_hour * 60 + t.tm_min;
        
        time_t nextBoundaryEpoch = 0; // 0 = no boundary found
        
        // Search up to 7 days ahead for the next schedule boundary
        for (int dayOffset = 0; dayOffset < 7 && nextBoundaryEpoch == 0; dayOffset++) {
          int checkDay = (t.tm_wday + dayOffset) % 7;
          
          for (int i = 0; i < scheduleCount; ++i) {
            ScheduleRow &r = scheduleRows[i];
            if (!r.enable || !r.setting.equalsIgnoreCase("schedule")) continue;
            if (!r.weekday[checkDay]) continue;
            
            // Count enabled days
            int enabledDayCount = 0;
            for (int d = 0; d < 7; ++d) {
              if (r.weekday[d]) enabledDayCount++;
            }
            if (enabledDayCount == 0) continue;
            
            int onMinutes = r.on_h * 60 + r.on_m;
            int offMinutes = r.off_h * 60 + r.off_m;
            
            // Find span start and end (same logic as checkSchedule)
            int spanStartDay = -1, spanEndDay = -1;
            for (int offset = 0; offset < 7; ++offset) {
              int prevDay = (7 + offset - 1) % 7;
              int curDay = offset;
              if (!r.weekday[prevDay] && r.weekday[curDay]) {
                spanStartDay = curDay;
                break;
              }
            }
            if (spanStartDay == -1) spanStartDay = 0;
            for (int offset = 0; offset < 7; ++offset) {
              int curDay = (spanStartDay + offset) % 7;
              int nextDay = (spanStartDay + offset + 1) % 7;
              if (r.weekday[curDay] && !r.weekday[nextDay]) {
                spanEndDay = curDay;
                break;
              }
            }
            if (spanEndDay == -1) spanEndDay = (spanStartDay + 6) % 7;
            
            bool isFirstDay = (checkDay == spanStartDay);
            bool isLastDay = (checkDay == spanEndDay);
            bool isSingleDay = (enabledDayCount == 1);
            
            // Candidate boundaries for this day
            int candidateMinutes = -1;
            
            if (isSingleDay) {
              // Single day schedule: both ON and OFF are boundaries
              if (dayOffset == 0) {
                // Today: find next boundary after now
                if (onMinutes > nowMinutes) candidateMinutes = onMinutes;
                else if (offMinutes > nowMinutes) candidateMinutes = offMinutes;
              } else {
                // Future day: earliest boundary
                candidateMinutes = (onMinutes < offMinutes) ? onMinutes : offMinutes;
              }
            } else {
              // Multi-day schedule
              if (isFirstDay) {
                // Only ON time is a boundary on first day
                if (dayOffset == 0 && onMinutes > nowMinutes) candidateMinutes = onMinutes;
                else if (dayOffset > 0) candidateMinutes = onMinutes;
              }
              if (isLastDay) {
                // Only OFF time is a boundary on last day
                if (dayOffset == 0 && offMinutes > nowMinutes) {
                  if (candidateMinutes == -1 || offMinutes < candidateMinutes) candidateMinutes = offMinutes;
                }
                else if (dayOffset > 0) {
                  if (candidateMinutes == -1 || offMinutes < candidateMinutes) candidateMinutes = offMinutes;
                }
              }
            }
            
            if (candidateMinutes != -1) {
              // Convert to epoch: current day's midnight + dayOffset days + candidateMinutes
              struct tm boundaryTm = t;
              boundaryTm.tm_hour = candidateMinutes / 60;
              boundaryTm.tm_min = candidateMinutes % 60;
              boundaryTm.tm_sec = 0;
              time_t candidateEpoch = mktime(&boundaryTm) + (dayOffset * 86400);
              
              if (nextBoundaryEpoch == 0 || candidateEpoch < nextBoundaryEpoch) {
                nextBoundaryEpoch = candidateEpoch;
              }
            }
          }
        }
        
        // Set override - will expire on timer toggle or schedule boundary
        manualOverridePending = true;
        manualOverrideExpiryEpoch = nextBoundaryEpoch;
        manualOverrideState = newState;
        
        if (nextBoundaryEpoch > 0) {
          struct tm expiryTm;
          gmtime_r(&nextBoundaryEpoch, &expiryTm);
          Serial.printf("🔄 Manual command: override active until %s %02d:%02d\n", 
                        (const char*[]){"Sun","Mon","Tue","Wed","Thu","Fri","Sat"}[expiryTm.tm_wday],
                        expiryTm.tm_hour, expiryTm.tm_min);
        } else {
          Serial.println("🔄 Manual command: no schedule boundary found, override until timer toggle");
        }
      } else {
        manualOverridePending = true;
        manualOverrideExpiryEpoch = 0;
        manualOverrideState = newState;
        Serial.println("🔄 Manual command: no schedules, override until timer toggle");
      }

      // DO NOT reset timers on manual command - let them continue cycling
      // This ensures timer toggle can also expire the override

      relayState1 = newState;
      digitalWrite(RELAY1_PIN, relayState1 ? HIGH : LOW);
      Serial.printf("🔄 Relay changed (manual): %s\n", relayState1 ? "ON" : "OFF");

      float t1, t2;
      bool valid1, valid2;
      readSensors(t1, t2, valid1, valid2);
      sendSensorData(deviceId, t1, t2, valid1, valid2, relayState1);
      lastSensorSend = now;
    }
  }

  if (now - lastSensorSend >= 40000) {
    float t1, t2;
    bool valid1, valid2;
    readSensors(t1, t2, valid1, valid2);
    if (valid1 || valid2) {
      Serial.println("📊 Periodic sensor update.");
    } else {
      Serial.println("⚠️ Both sensors invalid — sending relay only.");
    }
    sendSensorData(deviceId, t1, t2, valid1, valid2, relayState1);
    lastSensorSend = now;
  }

  if (now - lastNVSWrite >= 10000) {
    lastNVSWrite = now;
    preferences.begin("esp32iot", false);
    bool stored = preferences.getBool("relayState", false);
    if (stored != relayState1) {
      preferences.putBool("relayState", relayState1);
      Serial.println("💾 Relay state saved to NVS.");
    }
    preferences.end();
  }

  delay(10);
}
