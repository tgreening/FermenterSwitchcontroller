// Import required libraries
#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <SPI.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <CircularBuffer.h>
#include <ESP8266WebServer.h>
#include <FS.h>
#include <ESP8266mDNS.h>
#include <WiFiUdp.h>
#include <ArduinoOTA.h>

#define OLED_RESET 4
Adafruit_SSD1306 display(OLED_RESET);

// WiFi parameters
const char* ssid = "";
const char* password = "";
const int heatPin = D1;
const int coolPin = D2;
const int IR_PIN = D8;
const String apiKey = "";
const char* thingSpeakserver = "api.thingspeak.com";
const long READ_MILLIS = 5000UL;
const long POST_MILLIS = 300000UL;

float lastPostTime = 0;
float tolerance = 1.0F;
float desiredTemperature = 63.0;
int mode = 0; //1 - heating, 2 - cooling
HTTPClient http;
unsigned long lastReadingTime = millis() + 60000UL;
float lastReading, chamberReading;
float totalTemperatureChange = 0.0F;

// Setup a oneWire instance to communicate with any OneWire devices (not just Maxim/Dallas temperature ICs)
OneWire oneWire(D4);
OneWire oneWire2(D3);

DallasTemperature fermenterSensor(&oneWire);
DallasTemperature chamberSensor(&oneWire2);

ESP8266WebServer httpServer(80);

void setup(void)
{
  httpServer.on("/", HTTP_GET, []() {
    yield();
    httpServer.sendHeader("Connection", "close");
    httpServer.sendHeader("Access-Control-Allow-Origin", "*");
    Serial.println("Serving up HTML...");
    String html = "<html><body>Fermenter temperature: ";
    html += lastReading;
    html += "<br>Chamber temperature: ";
    html += chamberReading;
    html += "<br>Desired temperature: ";
    html += desiredTemperature;
    html += "<br>Tolerance: ";
    html += tolerance;
    html += "<br>Total Temperature Change: ";
    html += totalTemperatureChange;
    html += "<br>Mode: ";
    if (mode == 0) {
      html += "Off<br>";
    }
    if (mode == 1) {
      html += "Heating<br>";
    }
    if (mode == 2) {
      html += "Cooling<br>";
    }
    html += "</body></html>";
    Serial.println("Done serving up HTML...");
    httpServer.send(200, "text/html", html);
  });
  httpServer.on("/update", HTTP_GET, []() {
    yield();
    float desired, tol;
    if (httpServer.arg("desired") != "") {
      desired = httpServer.arg("desired").toFloat();
    } else {
      desired = desiredTemperature;
    }
    if (httpServer.arg("tolerance") != "") {
      tol = httpServer.arg("tolerance").toFloat();
    } else {
      tol = tolerance;
    }
    httpServer.sendHeader("Connection", "close");
    httpServer.sendHeader("Access - Control - Allow - Origin", "*");
    updateFile(desired, tol);
    httpServer.send(200, "text / plain", "OK");

  });
  pinMode(heatPin, OUTPUT);
  pinMode(coolPin, OUTPUT);
  // Start Serial
  Serial.begin(115200);
  Wire.begin(D5, D6);
  display.begin(SSD1306_SWITCHCAPVCC, 0x3C);  // initialize with the I2C addr 0x3D (for the 128x64)
  display.display();

  // Connect to WiFi
  WiFi.hostname("fermenterswitch");
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  ArduinoOTA.setHostname("FermenterSwitch");
  ArduinoOTA.onStart([]() {
    String type;
    if (ArduinoOTA.getCommand() == U_FLASH)
      type = "sketch";
    else // U_SPIFFS
      type = "filesystem";

    // NOTE: if updating SPIFFS this would be the place to unmount SPIFFS using SPIFFS.end()
    Serial.println("Start updating " + type);
  });
  ArduinoOTA.onEnd([]() {
    Serial.println("\nEnd");
  });
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    Serial.printf("Progress: %u%%\r", (progress / (total / 100)));
  });
  ArduinoOTA.onError([](ota_error_t error) {
    Serial.printf("Error[%u]: ", error);
    if (error == OTA_AUTH_ERROR) Serial.println("Auth Failed");
    else if (error == OTA_BEGIN_ERROR) Serial.println("Begin Failed");
    else if (error == OTA_CONNECT_ERROR) Serial.println("Connect Failed");
    else if (error == OTA_RECEIVE_ERROR) Serial.println("Receive Failed");
    else if (error == OTA_END_ERROR) Serial.println("End Failed");
  });
  ArduinoOTA.begin();

  fermenterSensor.begin();
  Serial.println("");
  Serial.println("WiFi connected");

  // Print the IP address
  Serial.println(WiFi.localIP());
  httpServer.begin();
  display.setTextSize(2);
  display.setTextColor(WHITE);
  fermenterSensor.requestTemperatures(); // Send the command to get temperatures
  lastReading = getReading(fermenterSensor);
  SPIFFS.begin();
  readFile();

}

void loop() {
  float currentReading;
  if ((long) (millis() - lastReadingTime) >= 0) {
    int retryCount = 0;
    lastReadingTime += READ_MILLIS;
    fermenterSensor.requestTemperatures(); // Send the command to get temperatures
    chamberSensor.requestTemperatures(); // Send the command to get temperatures
    currentReading = getReading(fermenterSensor);
    chamberReading = getReading(chamberSensor);
    float temperatureChange = currentReading - lastReading;
    totalTemperatureChange += temperatureChange;
    lastReading = currentReading;

    if ((long)(millis() - lastPostTime) >= 0) {
      Serial.println("Updating checks...");
      lastPostTime += POST_MILLIS;
      if (currentReading - desiredTemperature != 0.0F) {
        Serial.println("We need to do something");
        if (currentReading - desiredTemperature > tolerance) {
          Serial.println("Too hot");
          if (currentReading + (totalTemperatureChange * 2) > desiredTemperature + tolerance) {
            turnOnCooling();
          } else if (mode == 2) {
            Serial.println("Will get cool in 10 minutes");
            turnOff();
          }
        } else if (desiredTemperature - currentReading > tolerance) {
          Serial.println("Too cold");
          if ((currentReading + (totalTemperatureChange * 2)) < desiredTemperature - tolerance) {
            Serial.println("Should turn heat on");
            turnOnHeat();
          } else if (mode == 1) {
            Serial.println("Will get hot in 10 minutes");
            turnOff();
          }
        } else {
          turnOff();
        }
      } else {
        Serial.println("In the range");
        turnOff();
      }
      postThingSpeak(currentReading, chamberReading, desiredTemperature, totalTemperatureChange);
      totalTemperatureChange = 0.0F;
      yield();
    }
    display.setCursor(0, 0);
    display.clearDisplay();
    display.print("F: ");
    display.println(currentReading);
    display.setCursor(0, 15);
    display.print("C: ");
    display.println(chamberReading);
    display.setCursor(0, 30);
    display.print("De: ");
    display.println(desiredTemperature);
    display.setCursor(0, 45);
    display.print("Mode: ");
    if (mode == 0) {
      display.println("Off");
    }
    if (mode == 1) {
      display.println("Heat");
    }
    if (mode == 2) {
      display.println("Cool");
    }
    display.display();
    delay(100);
  }
  httpServer.handleClient();
  ArduinoOTA.handle();
}

float getReading(DallasTemperature sensor) {
  int retryCount = 0;
  float celsius = sensor.getTempCByIndex(0);
  while ((celsius == 85 || celsius == -85) && retryCount < 10) {
    celsius = sensor.getTempCByIndex(0);
    retryCount++;
    if (retryCount != 10) {
      delay(retryCount * 1000);
    }
  }
  if (retryCount == 10) {
    ESP.restart();
  }
  return (celsius * 9 / 5) + 32;
}

void turnOnHeat() {
  Serial.println("Turning heat on....");
  digitalWrite(coolPin, LOW);
  digitalWrite(heatPin, HIGH);
  mode = 1;
}

void turnOnCooling() {
  Serial.println("Turning cooling on....");
  digitalWrite(coolPin, HIGH);
  digitalWrite(heatPin, LOW);
  mode = 2;
}

void turnOff() {
  Serial.println("Turning everything off....");
  digitalWrite(coolPin, LOW);
  digitalWrite(heatPin, LOW);
  mode = 0;
}

void postThingSpeak(float fermenter, float chamber, int desired, float avgChange) {
  WiFiClient client;
  if (client.connect(thingSpeakserver, 80)) { // "184.106.153.149" or api.thingspeak.com
    String postStr = apiKey;
    postStr += "&field1=";
    postStr += String(fermenter);
    postStr += "&field2=";
    postStr += String(chamber);
    postStr += "&field3=";
    postStr += String(desired);
    postStr += "&field4=";
    postStr += String(mode);
    postStr += "&field5=";
    postStr += String(avgChange);
    postStr += "\r\n\r\n";

    client.print("POST /update HTTP/1.1\n");
    client.print("Host: api.thingspeak.com\n");
    client.print("Connection: close\n");
    client.print("X-THINGSPEAKAPIKEY: " + apiKey + "\n");
    client.print("Content-Type: application/x-www-form-urlencoded\n");
    client.print("Content-Length: ");
    client.print(postStr.length());
    client.print("\n\n");
    client.print(postStr);
  }
}

void readFile() {
  //  Serial.printf("read file heap size start: %u\n", ESP.getFreeHeap());
  File f = SPIFFS.open("settings.txt", "r");
  if (!f) {
    Serial.println("file open failed");
  }
  else {
    Serial.println("====== Reading from SPIFFS file =======");
    bool stillReading = true;
    while (stillReading) {
      String s = f.readStringUntil('\n');
      if (s != "") {
        Serial.println(s);
        int commaIndex = s.indexOf(',');
        desiredTemperature = s.substring(0, commaIndex).toFloat();
        tolerance = s.substring(commaIndex + 1).toFloat();
      } else {
        Serial.println("Done Reading....");
        stillReading = false;
      }
    }
  }
  f.close();
  //  Serial.printf("red file heap size end: %u\n", ESP.getFreeHeap());

}

void updateFile(float desired, float tolerance) {
  //  Serial.printf("update file settings heap size: %u\n", ESP.getFreeHeap());
  if (SPIFFS.exists("settings.txt")) {
    if (SPIFFS.exists("settings.old")) {
      SPIFFS.remove("settings.old");
    }
    SPIFFS.rename("settings.txt", "settings.old");
    SPIFFS.remove("settings.txt");
  }
  File f = SPIFFS.open("settings.txt", "w");
  if (!f) {
    Serial.println("file open failed");
  } else {
    f.printf("%f,%f\n", desired, tolerance);
    Serial.printf("Wrote: %f,%f\n", desired, tolerance);
    f.close();
    yield();
  }
}
