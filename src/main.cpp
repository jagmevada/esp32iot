
#include <WiFi.h>
#include <time.h>
#include <HTTPClient.h>
#include <OneWire.h>
#include <DallasTemperature.h>

// Wi-Fi credentials
const char *ssid = "S23";
const char *password = "11223344";

// Supabase REST endpoint

const char *getURL = "https://akxcjabakrvfaevdfwru.supabase.co/rest/v1/commands";
const char *postURL = "https://akxcjabakrvfaevdfwru.supabase.co/rest/v1/sensor_data";
const char *apikey = "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJpc3MiOiJzdXBhYmFzZSIsInJlZiI6ImFreGNqYWJha3J2ZmFldmRmd3J1Iiwicm9sZSI6ImFub24iLCJpYXQiOjE3NDkxMjMwMjUsImV4cCI6MjA2NDY5OTAyNX0.kykki4uVVgkSVU4lH-wcuGRdyu2xJ1CQkYFhQq_u08w";

// GPIO Setup for Room1 - AC1
#define ONE_WIRE_BUS_T1 23
#define RELAY_PIN 32

OneWire oneWireT1(ONE_WIRE_BUS_T1);
DallasTemperature sensorT1(&oneWireT1);

// Short and readable sensor IDs
const char *sensor_ids[] = {
    "ac1_r1", "ac2_r1", "ecs_r1",
    "ac1_r2", "ac2_r2", "ecs_r2"};

// Function to fetch relay state from Supabase for a given sensor and target
bool fetchRelayCommand(const char *sensor_id, const char *target, bool currentState)
{
  HTTPClient http;
  String url = String(getURL) + "?sensor_id=eq." + sensor_id + "&target=eq." + target + "&order=issued_at.desc&limit=1";

  http.begin(url);
  http.addHeader("apikey", apikey);
  http.addHeader("Authorization", "Bearer " + String(apikey));

  int httpCode = http.GET();
  if (httpCode == 200)
  {
    String response = http.getString();
    Serial.println("📥 Command Response: " + response);

    if (response.indexOf("\"state\":") != -1 && response.indexOf("\"issued_at\":") != -1)
    {
      int tsStart = response.indexOf("\"issued_at\":\"") + 13;
      int tsEnd = response.indexOf("\"", tsStart);
      String timestampStr = response.substring(tsStart, tsEnd);
      Serial.println("⏱ Timestamp: " + timestampStr);

      struct tm tm;
      if (sscanf(timestampStr.c_str(), "%4d-%2d-%2dT%2d:%2d:%2d",
                 &tm.tm_year, &tm.tm_mon, &tm.tm_mday,
                 &tm.tm_hour, &tm.tm_min, &tm.tm_sec) == 6)
      {

        tm.tm_year -= 1900;
        tm.tm_mon -= 1;
        time_t issuedEpoch = mktime(&tm);
        time_t nowEpoch = time(nullptr);
        double ageSeconds = difftime(nowEpoch, issuedEpoch);

        if (ageSeconds > 120)
        {
          Serial.println("⏳ Command too old, retaining previous state.");
          return currentState; // RETAIN OLD STATE
        }

        // Parse new state
        if (response.indexOf("\"state\":true") != -1)
          return true;
        if (response.indexOf("\"state\":false") != -1)
          return false;
      }
    }
  }
  else
  {
    Serial.println("❌ GET command failed, code: " + String(httpCode));
  }

  http.end();
  return currentState; // RETAIN OLD STATE ON FAILURE
}

void setup()
{
  Serial.begin(115200);

  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, LOW); // OFF initially

  sensorT1.begin();

  WiFi.begin(ssid, password);
  Serial.print("Connecting to WiFi");
  while (WiFi.status() != WL_CONNECTED)
  {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\n✅ Connected to WiFi");
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");
}

void sendSensorData(String id, float t1, float t2, float rh, float pm1, float pm25, float pm10,
                    int nc05, int nc10, int nc25, bool relay1, bool relay2)
{
  if (WiFi.status() == WL_CONNECTED)
  {
    HTTPClient http;
    http.begin(postURL);
    http.addHeader("Content-Type", "application/json");
    http.addHeader("apikey", apikey);
    http.addHeader("Authorization", "Bearer " + String(apikey));

    String payload = "{";
    payload += "\"sensor_id\":\"" + id + "\",";
    if (!isnan(t1))
      payload += "\"t1\":" + String(t1, 2) + ",";
    if (!isnan(t2))
      payload += "\"t2\":" + String(t2, 2) + ",";
    if (!isnan(rh))
      payload += "\"rh\":" + String(rh, 1) + ",";
    if (!isnan(pm1))
      payload += "\"pm1\":" + String(pm1, 1) + ",";
    if (!isnan(pm25))
      payload += "\"pm25\":" + String(pm25, 1) + ",";
    if (!isnan(pm10))
      payload += "\"pm10\":" + String(pm10, 1) + ",";
    if (nc05 >= 0)
      payload += "\"nc0_5\":" + String(nc05) + ",";
    if (nc10 >= 0)
      payload += "\"nc1_0\":" + String(nc10) + ",";
    if (nc25 >= 0)
      payload += "\"nc2_5\":" + String(nc25) + ",";
    payload += "\"relay1\":" + String(relay1 ? "true" : "false");

    if (id.startsWith("ecs_"))
    {
      payload += ",\"relay2\":" + String(relay2 ? "true" : "false");
    }
    payload += "}";

    Serial.println("📤 POST to Supabase: " + payload);

    int code = http.POST(payload);
    if (code > 0)
    {
      Serial.print("✅ Response: ");
      Serial.println(http.getString());
    }
    else
    {
      Serial.println("❌ POST failed");
    }

    http.end();
  }
  else
  {
    Serial.println("⚠️ WiFi disconnected");
  }
}

void loop()
{
  // Fetch latest relay1 state for ac1_r1
  // static bool lastRelay1State = false;
  // bool relayState1 = fetchRelayCommand("ac1_r1", "relay1", lastRelay1State);
  // lastRelay1State = relayState1;
  // digitalWrite(RELAY_PIN, relayState1 ? HIGH : LOW);

  // ==== ROOM 1 ====
  static bool lastRoom1_AC1_Relay = false;
  bool room1_AC1_Relay = fetchRelayCommand("ac1_r1", "relay1", lastRoom1_AC1_Relay);
  lastRoom1_AC1_Relay = room1_AC1_Relay;

  static bool lastRoom1_AC2_Relay = false;
  bool room1_AC2_Relay = fetchRelayCommand("ac2_r1", "relay1", lastRoom1_AC2_Relay);
  lastRoom1_AC2_Relay = room1_AC2_Relay;

  static bool lastRoom1_ECS_Relay1 = false;
  bool room1_ECS_Relay1 = fetchRelayCommand("ecs_r1", "relay1", lastRoom1_ECS_Relay1);
  lastRoom1_ECS_Relay1 = room1_ECS_Relay1;

  static bool lastRoom1_ECS_Relay2 = false;
  bool room1_ECS_Relay2 = fetchRelayCommand("ecs_r1", "relay2", lastRoom1_ECS_Relay2);
  lastRoom1_ECS_Relay2 = room1_ECS_Relay2;

  // ==== ROOM 2 ====
  static bool lastRoom2_AC1_Relay = false;
  bool room2_AC1_Relay = fetchRelayCommand("ac1_r2", "relay1", lastRoom2_AC1_Relay);
  lastRoom2_AC1_Relay = room2_AC1_Relay;

  static bool lastRoom2_AC2_Relay = false;
  bool room2_AC2_Relay = fetchRelayCommand("ac2_r2", "relay1", lastRoom2_AC2_Relay);
  lastRoom2_AC2_Relay = room2_AC2_Relay;

  static bool lastRoom2_ECS_Relay1 = false;
  bool room2_ECS_Relay1 = fetchRelayCommand("ecs_r2", "relay1", lastRoom2_ECS_Relay1);
  lastRoom2_ECS_Relay1 = room2_ECS_Relay1;

  static bool lastRoom2_ECS_Relay2 = false;
  bool room2_ECS_Relay2 = fetchRelayCommand("ecs_r2", "relay2", lastRoom2_ECS_Relay2);
  lastRoom2_ECS_Relay2 = room2_ECS_Relay2;

  // Read Room1 - AC1
  sensorT1.requestTemperatures();
  float t1 = sensorT1.getTempCByIndex(0);
  float t2 = t1 + 0.5; // Simulated t2

  sendSensorData("ac1_r1", t1, t2, NAN, NAN, NAN, NAN, -1, -1, -1, room1_AC1_Relay, NAN);

  // Simulated Room1 - AC2
  sendSensorData("ac2_r1", 25.5, 25.8, NAN, NAN, NAN, NAN, -1, -1, -1, room1_AC2_Relay, NAN);

  // Simulated Room1 - ECS
  sendSensorData("ecs_r1", 26.1, NAN, 55.5, 5.0, 10.2, 15.3, 400, 210, 100, room1_ECS_Relay1, room1_ECS_Relay2);

  // Simulated Room2 - AC1
  sendSensorData("ac1_r2", t1 + 1.5, t2 + 1.5, NAN, NAN, NAN, NAN, -1, -1, -1, room2_AC1_Relay, NAN);

  // Simulated Room2 - AC2
  sendSensorData("ac2_r2", 27.0, 27.5, NAN, NAN, NAN, NAN, -1, -1, -1, room2_AC2_Relay, NAN);

  // Simulated Room2 - ECS
  sendSensorData("ecs_r2", 27.6, NAN, 60.1, 15.0, 22.0, 30.5, 500, 260, 150, room2_ECS_Relay1, room2_ECS_Relay2);

  Serial.println("✅ All 6 devices posted.\n");
  delay(40000); // 40s interval
}
