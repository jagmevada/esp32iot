#include <WiFi.h>
#include <HTTPClient.h>
#include <OneWire.h>
#include <DallasTemperature.h>

// Wi-Fi credentials
const char *ssid = "S23";
const char *password = "11223344";

// Supabase REST endpoint
const char *serverURL = "https://bhanhhhttmktpcewqnuf.supabase.co/rest/v1/sensor_data";

// Supabase anon API key (Bearer token)
const char *apikey = "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJpc3MiOiJzdXBhYmFzZSIsInJlZiI6ImJoYW5oaGh0dG1rdHBjZXdxbnVmIiwicm9sZSI6ImFub24iLCJpYXQiOjE3NDkwMzIzNTQsImV4cCI6MjA2NDYwODM1NH0.sdrHcD6d3dmRnifGC_Xd9V30IjqcxEo-JLwRTSNwkwY";

// DS18B20 sensor on GPIO 23
#define ONE_WIRE_BUS 23
OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature sensors(&oneWire);

// Simulated sensor IDs
const char *sensor_ids[] = {"s1", "s2", "s3", "s4", "s5", "s6"};
const int num_sensors = 6;

void setup()
{
  Serial.begin(115200);
  sensors.begin();

  WiFi.begin(ssid, password);
  Serial.print("Connecting to WiFi");
  while (WiFi.status() != WL_CONNECTED)
  {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\n✅ Connected to WiFi");
}

void loop()
{
  sensors.requestTemperatures();
  float realTemp = sensors.getTempCByIndex(0); // Real temp from DS18B20

  for (int i = 0; i < num_sensors; i++)
  {
    String sensor_id = sensor_ids[i];

    // Use real temperature for s1; simulate for others
    float temperature = (i == 0) ? realTemp : realTemp + i * 0.5;
    float humidity = 60.0 + i * 2.0;
    float pm25 = 10.0 + i * 1.5;
    int nc = 400 + i * 30;

    if (WiFi.status() == WL_CONNECTED)
    {
      HTTPClient http;
      http.begin(serverURL);
      http.addHeader("Content-Type", "application/json");
      http.addHeader("apikey", apikey);
      http.addHeader("Authorization", "Bearer " + String(apikey));

      String payload = "{";
      payload += "\"sensor_id\":\"" + sensor_id + "\",";
      payload += "\"t\":" + String(temperature, 2) + ",";
      payload += "\"h\":" + String(humidity, 1) + ",";
      payload += "\"p\":" + String(pm25, 1) + ",";
      payload += "\"nc\":" + String(nc);
      payload += "}";

      Serial.printf("📤 Sending data for sensor %s:\n", sensor_id.c_str());
      Serial.println(payload);

      int httpResponseCode = http.POST(payload);

      Serial.print("🌐 Response Code: ");
      Serial.println(httpResponseCode);
      if (httpResponseCode > 0)
      {
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
      Serial.println("⚠️ WiFi not connected");
    }
  }

  Serial.println("✅ Sent all 6 sensor readings.\n");
  delay(60000); // Wait 30s before next full set
}
