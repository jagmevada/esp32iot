#include <WiFi.h>
#include <HTTPClient.h>
#include <OneWire.h>
#include <DallasTemperature.h>

// Wi-Fi credentials
const char *ssid = "Amba";
const char *password = "dadaniruma";

// Supabase Edge Function URL
const char *serverURL = "https://uvyjejutvbozskwbvnmk.supabase.co/functions/v1/mqtt-handler";

// Supabase Bearer Token
const char *bearerToken = "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJpc3MiOiJzdXBhYmFzZSIsInJlZiI6InV2eWplanV0dmJvenNrd2J2bm1rIiwicm9sZSI6ImFub24iLCJpYXQiOjE3NDg4NDY3NDEsImV4cCI6MjA2NDQyMjc0MX0.Xm1IItuH9UCzQx5-rgBt0NZf5HTPXh4kXApg_ma-XVw";

// DS18B20 on GPIO 23
#define ONE_WIRE_BUS 23
OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature sensors(&oneWire);

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
  float temperature = sensors.getTempCByIndex(0);

  if (WiFi.status() == WL_CONNECTED)
  {
    HTTPClient http;
    http.begin(serverURL);
    http.addHeader("Content-Type", "application/json");
    http.addHeader("Authorization", "Bearer " + String(bearerToken));

    // Build JSON payload
    String payload = "{";
    payload += "\"device_id\":\"ESP32_001\",";
    payload += "\"temperature\":" + String(temperature, 2) + ",";
    payload += "\"humidity\":65.2,";
    payload += "\"air_quality\":45,";
    payload += "\"co2\":420,";
    payload += "\"human_count\":2,";
    payload += "\"light_level\":750,";
    payload += "\"noise\":35.8";
    payload += "}";

    Serial.println("📤 Sending data:");
    Serial.println(payload);

    int httpResponseCode = http.POST(payload);

    Serial.print("🌐 Response Code: ");
    Serial.println(httpResponseCode);
    if (httpResponseCode > 0)
    {
      Serial.println(http.getString()); // Server reply
    }
    else
    {
      Serial.println("POST failed");
    }

    http.end();
  }
  else
  {
    Serial.println("⚠️ WiFi not connected");
  }

  delay(5000); // Send every 5 seconds
}
