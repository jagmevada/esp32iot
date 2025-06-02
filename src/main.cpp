#include <WiFi.h>
#include <OneWire.h>
#include <DallasTemperature.h>

// Wi-Fi credentials
const char *ssid = "Amba";
const char *password = "dadaniruma";

// DS18B20 data pin
#define ONE_WIRE_BUS 23

OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature sensors(&oneWire);

WiFiServer server(80);

void setup()
{
  Serial.begin(115200);
  sensors.begin();

  // Connect to Wi-Fi
  WiFi.begin(ssid, password);
  Serial.print("Connecting to WiFi");
  while (WiFi.status() != WL_CONNECTED)
  {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nConnected!");
  Serial.print("IP Address: ");
  Serial.println(WiFi.localIP());

  server.begin(); // Start server
}

void loop()
{
  WiFiClient client = server.available();
  if (client)
  {
    Serial.println("Client connected");

    sensors.requestTemperatures();
    float tempC = sensors.getTempCByIndex(0);

    // Wait for client data
    while (client.connected())
    {
      if (client.available())
      {
        client.read(); // Read request (we ignore the details)

        // Send basic HTTP response
        client.println("HTTP/1.1 200 OK");
        client.println("Content-Type: text/html");
        client.println("Connection: close");
        client.println();

        client.println("<!DOCTYPE HTML><html>");
        client.println("<h1>ESP32 Temperature</h1>");
        client.print("<p>Temperature: ");
        client.print(tempC);
        client.println(" &deg;C</p>");
        client.println("</html>");

        break;
      }
    }
    delay(10);
    client.stop();
    Serial.println("Client disconnected");
  }
}
