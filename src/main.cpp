#include <WiFiManager.h>
#include <WiFi.h>
#include <time.h>
#include <HTTPClient.h>
#include <EEPROM.h>
#include <Wire.h>
#include <Adafruit_SHT31.h>

// === EEPROM Setup ===
#define EEPROM_SIZE 2
#define EEPROM_RELAY1_ADDR 0
#define EEPROM_RELAY2_ADDR 1

// === Supabase API Info ===
const char *getURL = "https://akxcjabakrvfaevdfwru.supabase.co/rest/v1/commands";
const char *postURL = "https://akxcjabakrvfaevdfwru.supabase.co/rest/v1/sensor_data";
const char *apikey = "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJpc3MiOiJzdXBhYmFzZSIsInJlZiI6ImFreGNqYWJha3J2ZmFldmRmd3J1Iiwicm9sZSI6ImFub24iLCJpYXQiOjE3NDkxMjMwMjUsImV4cCI6MjA2NDY5OTAyNX0.kykki4uVVgkSVU4lH-wcuGRdyu2xJ1CQkYFhQq_u08w"; // truncated

// === GPIO Definitions ===
#define RELAY1_PIN 32 // Air Purifier
#define RELAY2_PIN 33 // Dehumidifier

// === I2C Buses ===
TwoWire I2CBus1 = TwoWire(0); // GPIO 21/22
TwoWire I2CBus2 = TwoWire(1); // GPIO 25/26
Adafruit_SHT31 sht1;
Adafruit_SHT31 sht2;

bool relayState1 = false;
bool relayState2 = true;
unsigned long lastRelayCheck = 0;
unsigned long lastSensorSend = 0;
unsigned long lastWiFiCheck = 0;
unsigned long lastEEPROMWrite = 0;

bool fetchRelayCommand(const char *sensor_id, const char *target, bool currentState) {
  HTTPClient http;
  String url = String(getURL) + "?sensor_id=eq." + sensor_id + "&target=eq." + target + "&order=issued_at.desc&limit=1";
  http.begin(url);
  http.addHeader("apikey", apikey);
  http.addHeader("Authorization", "Bearer " + String(apikey));
  int httpCode = http.GET();
  if (httpCode == 200) {
    String response = http.getString();
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

void sendSensorData(String id, float t1, float t2, float rh1, float rh2, bool v1, bool v2, bool r1, bool r2) {
  if (WiFi.status() != WL_CONNECTED) return;

  HTTPClient http;
  http.begin(postURL);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("apikey", apikey);
  http.addHeader("Authorization", "Bearer " + String(apikey));

  String payload = "{";
  payload += "\"sensor_id\":\"" + id + "\",";
  payload += "\"t1\":" + (v1 ? String(t1, 2) : "null") + ",";
  payload += "\"t2\":" + (v2 ? String(t2, 2) : "null") + ",";
  if (v1 && v2)
    payload += "\"rh\":" + String((rh1 + rh2) / 2.0, 2) + ",";
  else if (v1)
    payload += "\"rh\":" + String(rh1, 2) + ",";
  else if (v2)
    payload += "\"rh\":" + String(rh2, 2) + ",";
  else
    payload += "\"rh\":null,";
  payload += "\"relay1\":" + String(r1 ? "true" : "false") + ",";
  payload += "\"relay2\":" + String(r2 ? "true" : "false");
  payload += "}";

  Serial.println("📤 POST: " + payload);
  int code = http.POST(payload);
  if (code > 0) Serial.println("✅ Supabase: " + http.getString());
  else Serial.println("❌ POST failed");

  http.end();
}


bool readSensors(float &t1, float &t2, float &rh1, float &rh2, bool &v1, bool &v2) {
  Wire = I2CBus1;
  t1 = sht1.readTemperature();
  rh1 = sht1.readHumidity();
  Wire = I2CBus2;
  t2 = sht2.readTemperature();
  rh2 = sht2.readHumidity();

  v1 = !isnan(t1) && !isnan(rh1);
  v2 = !isnan(t2) && !isnan(rh2);

  return v1 || v2;
}


void checkWiFi() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("⚠️ WiFi disconnected! Reconnecting...");
    WiFi.disconnect();
    WiFi.begin();
    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < 10000) {
      delay(500);
      Serial.print(".");
    }
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("\n❌ WiFi reconnect failed. Launching portal...");
      WiFiManager wm;
      wm.setConfigPortalTimeout(120);
      if (!wm.autoConnect("ECS_R1_SETUP")) {
        Serial.println("⏳ Portal timeout. Restarting...");
        ESP.restart();
      }
    } else {
      Serial.println("✅ Reconnected to WiFi");
    }
  }
}

void setup() {
  EEPROM.begin(EEPROM_SIZE);
  delay(5);
  Serial.begin(115200);

  pinMode(RELAY1_PIN, OUTPUT);
  pinMode(RELAY2_PIN, OUTPUT);
  relayState1 = EEPROM.read(EEPROM_RELAY1_ADDR) == 1;
  relayState2 = EEPROM.read(EEPROM_RELAY2_ADDR) == 1;
  digitalWrite(RELAY1_PIN, relayState1 ? LOW : HIGH);
  digitalWrite(RELAY2_PIN, relayState2 ? LOW : HIGH);

  I2CBus1.begin(21, 22);
  I2CBus2.begin(25, 26);

  Wire = I2CBus1;
  sht1.begin(0x44);
  Wire = I2CBus2;
  sht2.begin(0x44);

  WiFiManager wm;
  wm.setConfigPortalTimeout(120);
  wm.setWiFiAutoReconnect(true);
  if (!wm.autoConnect("ECS_R1_SETUP")) {
    Serial.println("❌ WiFiManager failed. Restarting...");
    ESP.restart();
  }

  configTime(0, 0, "pool.ntp.org", "time.nist.gov");
  delay(1000);

  relayState1 = fetchRelayCommand("ecs_r1", "relay1", relayState1);
  relayState2 = fetchRelayCommand("ecs_r1", "relay2", relayState2);
  digitalWrite(RELAY1_PIN, relayState1 ? LOW : HIGH);
  digitalWrite(RELAY2_PIN, relayState2 ? LOW : HIGH);

  lastRelayCheck = lastSensorSend = lastWiFiCheck = lastEEPROMWrite = millis();
}

void loop() {
  unsigned long now = millis();

  if (now - lastWiFiCheck > 10000) {
    lastWiFiCheck = now;
    checkWiFi();
  }

  if (now - lastRelayCheck >= 5000) {
    lastRelayCheck = now;
    bool newR1 = fetchRelayCommand("ecs_r1", "relay1", relayState1);
    bool newR2 = fetchRelayCommand("ecs_r1", "relay2", relayState2);
    if (newR1 != relayState1 || newR2 != relayState2) {
      relayState1 = newR1;
      relayState2 = newR2;
      digitalWrite(RELAY1_PIN, relayState1 ? LOW : HIGH);
      digitalWrite(RELAY2_PIN, relayState2 ? LOW : HIGH);
      Serial.printf("🔄 Relays changed: R1=%s, R2=%s\n", relayState1 ? "ON" : "OFF", relayState2 ? "ON" : "OFF");

float t1, t2, rh1, rh2;
bool v1, v2;
if (readSensors(t1, t2, rh1, rh2, v1, v2)) {
  sendSensorData("ecs_r1", t1, t2, rh1, rh2, v1, v2, relayState1, relayState2);
} else {
  Serial.println("⚠️ No valid sensors — sending relay states only.");
  sendSensorData("ecs_r1", 0, 0, 0, 0, false, false, relayState1, relayState2);
}
    }
  }

  if (now - lastSensorSend >= 40000) {
float t1, t2, rh1, rh2;
bool v1, v2;
if (readSensors(t1, t2, rh1, rh2, v1, v2)) {
  sendSensorData("ecs_r1", t1, t2, rh1, rh2, v1, v2, relayState1, relayState2);
} else {
  Serial.println("⚠️ No valid sensors — sending relay states only.");
  sendSensorData("ecs_r1", 0, 0, 0, 0, false, false, relayState1, relayState2);
}
    lastSensorSend = now;
  }

  if (now - lastEEPROMWrite >= 10000) {
    lastEEPROMWrite = now;

    bool changed = false;

    if (EEPROM.read(EEPROM_RELAY1_ADDR) != (relayState1 ? 1 : 0)) {
      EEPROM.write(EEPROM_RELAY1_ADDR, relayState1 ? 1 : 0);
      changed = true;
    }

    if (EEPROM.read(EEPROM_RELAY2_ADDR) != (relayState2 ? 1 : 0)) {
      EEPROM.write(EEPROM_RELAY2_ADDR, relayState2 ? 1 : 0);
      changed = true;
    }

    if (changed) {
      EEPROM.commit();
      Serial.println("💾 Relay states saved to EEPROM.");
    }
  }


  delay(10);
}
