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
#include <time.h>
#include <WiFiUdp.h>
#include <WiFiManager.h>         //https://github.com/tzapu/WiFiManager
#include <FS.h>                   //this needs to be first, or it all crashes and burns...
#include <ArduinoJson.h>          //https://github.com/bblanchon/ArduinoJson
#define OLED_RESET 4
Adafruit_SSD1306 display(OLED_RESET);
#include <time.h>
#include "ThingSpeak.h"

// WiFi parameters
const int heatPin = D1;
const int coolPin = D2;
const int IR_PIN = D8;
const char* thingSpeakserver = "api.thingspeak.com";
const long READ_MILLIS = 5000UL;
const long WAIT_MILLIS = 300000UL;
long POST_MILLIS = 300000UL;
long NEXT_CHECK = 120000UL;
const char* host = "fermenterswitch";
const int COOLING = 2;
const int HEATING = 1;
const int OFF = 0;

float tolerance = 1.0F;
float desiredTemperature = 63.0;
int mode = 0; //1 - heating, 2 - cooling
HTTPClient http;
unsigned long lastReadingTime = ((long)millis()) + 60000UL;
long unsigned lastPostTime = ((long)millis()) + POST_MILLIS;
unsigned long nextCheck = (long)millis() + 120000UL;
float lastReading, chamberReading;
float temperatureChange = 0.0F;
char apiKey[16], postMinutes[3], checkMinutes[3], tsChannel[6];
int HighMillis = 0;
int Rollover = 0;
bool isHeatEnabled = true;
bool isCoolEnabled = true;

//flag for saving data
bool shouldSaveConfig = false;

// Setup a oneWire instance to communicate with any OneWire devices (not just Maxim/Dallas temperature ICs)
OneWire oneWire(D4);
OneWire oneWire2(D3);

DallasTemperature fermenterSensor(&oneWire);
DallasTemperature chamberSensor(&oneWire2);

ESP8266WebServer httpServer(80);

//callback notifying us of the need to save config
void saveConfigCallback () {
  Serial.println("Should save config");
  shouldSaveConfig = true;
}

void setup(void)
{
  pinMode(heatPin, OUTPUT);
  pinMode(coolPin, OUTPUT);

  //If you don't do this the 40A SSR goes high for some reason -- so turn everything off
  turnOff();

  WiFiManager wifiManager;
  //wifiManager.resetSettings();
  httpServer.on("/", HTTP_GET, []() {
    yield();
    httpServer.sendHeader("Connection", "close");
    httpServer.sendHeader("Access-Control-Allow-Origin", "*");
    Serial.println("Serving up HTML...");
    String html = "<html><body>Fermenter temperature: ";
    html += lastReading;
    html += "<br>Chamber temperature: ";
    html += chamberReading;
    html += "<br>Temperature Change: ";
    html += temperatureChange;
    html += "<br>Mode: ";
    if (mode == OFF) {
      html += "Off<br>";
    }
    if (mode == HEATING) {
      html += "Heating<br>";
    }
    if (mode == COOLING) {
      html += "Cooling<br>";
    }
    html += "<br>Uptime: ";
    html += uptimeString();
    html += "<br><h1>Update</h1><br><form method=\"GET\" action=\"/update\">Desired temperature:<input name=\"desired\" type=\"text\" maxlength=\"5\" size=\"5\" value=\"" + String(desiredTemperature) + "\" />";
    html += "<br>Tolerance: <input name=\"tolerance\" type=\"text\" maxlength=\"5\" size=\"5\" value=\"" + String(tolerance) + "\" />";
    html += "<br>Cooling: <input name=\"cooling\" type=\"radio\" value=\"1\" ";
    if (isCoolEnabled) {
      html += " checked";
    }
    html += ">On<input name=\"cooling\" type=\"radio\" value=\"0\" ";
    if (!isCoolEnabled) {
      html += " checked";
    }
    html += ">Off<br>";
    html += "<br>Heating: <input name=\"heating\" type=\"radio\" value=\"1\" ";
    if (isHeatEnabled) {
      html += " checked";
    }
    html += ">On<input name=\"heating\" type=\"radio\" value=\"0\" ";
    if (!isHeatEnabled) {
      html += " checked";
    }
    html += ">Off<br>";
    html += "<br><INPUT type=\"submit\" value=\"Send\"> <INPUT type=\"reset\"></form>";
    html += "<br></body></html>";
    Serial.print("Done serving up HTML...");
    Serial.println(html);
    httpServer.send(200, "text/html", html);
  });
  httpServer.on("/update", HTTP_GET, []() {
    yield();
    float desired, tol;
    if (httpServer.arg("desired") != "") {
      desiredTemperature = httpServer.arg("desired").toFloat();
    }
    if (httpServer.arg("tolerance") != "") {
      tolerance = httpServer.arg("tolerance").toFloat();
    }
    if (httpServer.arg("cooling") != "") {
      isCoolEnabled = httpServer.arg("cooling").toInt();
    }
    if (httpServer.arg("heating") != "") {
      isHeatEnabled = httpServer.arg("heating").toInt();
    }

    httpServer.sendHeader("Connection", "close");
    httpServer.sendHeader("Access - Control - Allow - Origin", "*");
    writeSettingsFile();
    httpServer.send(200, "text / plain", "OK");
  });
  // Start Serial
  Serial.begin(115200);
  Wire.begin(D5, D6);
  display.begin(SSD1306_SWITCHCAPVCC, 0x3C);  // initialize with the I2C addr 0x3D (for the 128x64)
  display.display();

  if (SPIFFS.begin()) {
    Serial.println("mounted file system");
    if (SPIFFS.exists("/config.json")) {
      //file exists, reading and loading
      Serial.println("reading config file");
      File configFile = SPIFFS.open("/config.json", "r");
      if (configFile) {
        Serial.println("opened config file");
        size_t size = configFile.size();
        // Allocate a buffer to store contents of the file.
        std::unique_ptr<char[]> buf(new char[size]);

        configFile.readBytes(buf.get(), size);
        DynamicJsonBuffer jsonBuffer;
        JsonObject& json = jsonBuffer.parseObject(buf.get());
        json.printTo(Serial);
        if (json.success()) {
          Serial.println("\nparsed json");
          if (json["thinkSpeakChannel"])
            strcpy(tsChannel, json["ThingSpeakChannel"]);
          strcpy(apiKey, json["ThingSpeakWriteKey"]);
          strcpy(postMinutes, json["ThingSpeakPostMinutes"]);

          strcpy(checkMinutes, json["NextCheckMinutes"]);
        } else {
          Serial.println("failed to load json config");
        }
      }
    }
  } else {
    Serial.println("failed to mount FS");
  }
  WiFiManagerParameter custom_thingspeak_api_key("key", "API key", apiKey, 40);
  WiFiManagerParameter custom_thingspeak_post_minutes("Post Minutes", "Minutes between Thingsspeak posts", postMinutes, 5);
  WiFiManagerParameter custom_check_minutes("Check Minutes", "Check Minutes", checkMinutes, 5);
  WiFiManagerParameter custom_thingspeak_channel("ThingSpeak Channel", "Channel Number", tsChannel, 8);
  wifiManager.setSaveConfigCallback(saveConfigCallback);

  //add all your parameters here
  wifiManager.addParameter(&custom_thingspeak_api_key);
  wifiManager.addParameter(&custom_thingspeak_channel);
  wifiManager.addParameter(&custom_thingspeak_post_minutes);
  wifiManager.addParameter(&custom_check_minutes);
  WiFi.hostname(String(host));

  wifiManager.setConfigPortalTimeout(90);
  if (!wifiManager.startConfigPortal(host)) {
    Serial.println("failed to connect and hit timeout");
    delay(3000);
    //reset and try again, or maybe put it to deep sleep
    ESP.reset();
    delay(5000);
  }

  //if you get here you have connected to the WiFi
  Serial.println("connected...yeey :)");
  if (!MDNS.begin(host)) {
    Serial.println("Error setting up MDNS responder!");
  }

  strcpy(apiKey, custom_thingspeak_api_key.getValue());
  strcpy(tsChannel, custom_thingspeak_channel.getValue());
  int  i = atoi(custom_thingspeak_post_minutes.getValue());
  if (i > 0) {
    POST_MILLIS = (unsigned long)(60000 * i);
  }
  i = atoi(custom_check_minutes.getValue());
  if (i > 0) {
    NEXT_CHECK = (unsigned long)(60000 * i);
  }
  Serial.println(POST_MILLIS);
  Serial.println(NEXT_CHECK);
  //save the custom parameters to FS
  if (shouldSaveConfig) {
    Serial.println("saving config");
    DynamicJsonBuffer jsonBuffer;
    JsonObject& json = jsonBuffer.createObject();
    json["ThingSpeakChannel"] = tsChannel;
    json["ThingSpeakWriteKey"] = apiKey;
    json["ThingSpeakPostMinutes"] = POST_MILLIS / 60000;
    json["NextCheckMinutes"] = NEXT_CHECK / 60000;

    File configFile = SPIFFS.open("/config.json", "w");
    if (!configFile) {
      Serial.println("failed to open config file for writing");
    }

    json.printTo(Serial);
    json.printTo(configFile);
    configFile.close();
    //end save
  }

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  ArduinoOTA.setHostname(host);
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
  readSettingsFile();
  turnOff();
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
    if ((long)millis() > nextCheck) {
      Serial.println("Next Check!");
      nextCheck += NEXT_CHECK;
      temperatureChange = currentReading - lastReading;
      lastReading = currentReading;
      if (mode == OFF) {
        if (currentReading  > desiredTemperature + tolerance && isCoolEnabled) {
          turnOnCooling();
        } else if ( desiredTemperature >  currentReading + tolerance && isHeatEnabled) {
          turnOnHeat();
        }
      } else {
        Serial.println("Already doing something check on status");
        int multiplier = 1;
        if (mode == COOLING) {
          Serial.println("Too hot");
          if ( currentReading > chamberReading) {
            multiplier += (currentReading - chamberReading) / 3;
          }
          if ((currentReading + (temperatureChange * multiplier)) < desiredTemperature || currentReading < desiredTemperature || !isCoolEnabled) {
            Serial.println("Will get cool in a few minutes");
            turnOff();
          }
        } else if (mode == HEATING ) {
          Serial.println("Too cold");
          if (chamberReading > currentReading) {
            multiplier += (chamberReading - currentReading) / 3;
          }
          if ((currentReading + (temperatureChange * multiplier)) > desiredTemperature || currentReading > desiredTemperature || !isHeatEnabled) {
            Serial.println("Will get hot in a few minutes");
            turnOff();
          }
        }
      }
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
    if (mode == OFF) {
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
  if ((long)(millis() - lastPostTime) >= 0) {
    Serial.println("Posting....");
    lastPostTime += POST_MILLIS;
    postReadingData(currentReading, chamberReading, desiredTemperature, temperatureChange, tolerance);
    yield();
  }
  uptime();
  httpServer.handleClient();
  ArduinoOTA.handle();
}

float getReading(DallasTemperature sensor) {
  int retryCount = 0;
  float firstReading = sensor.getTempFByIndex(0);
  //always good to wait between readings
  delay(500);
  //Get second reading to ensure that we don't have an anomaly
  float secondReading = sensor.getTempFByIndex(0);
  //If the two readings are more than a degree celsius different - retake both
  while (((firstReading - secondReading) > 1.0F || (secondReading - firstReading) > 1.0F) && ((int)firstReading * 100)  < 200 && retryCount < 10) {
    firstReading = sensor.getTempFByIndex(0);
    retryCount++;
    if (retryCount != 10) {
      delay(retryCount * 1000);
    }
    secondReading = sensor.getTempFByIndex(0);
  }
  //If after ten tries we're still off - restart
  if (retryCount == 10) {
    ESP.restart();
  }
  return secondReading;
}

void turnOnHeat() {
  Serial.println("Turning heat on....");
  digitalWrite(coolPin, LOW);
  digitalWrite(heatPin, HIGH);
  mode = HEATING;
}

void turnOnCooling() {
  Serial.println("Turning cooling on....");
  digitalWrite(coolPin, HIGH);
  digitalWrite(heatPin, LOW);
  mode = COOLING;
}

void turnOff() {
  Serial.println("Turning everything off....");
  digitalWrite(coolPin, LOW);
  digitalWrite(heatPin, LOW);
  mode = OFF;
  nextCheck = millis() + WAIT_MILLIS;
}

void postRestartData() {
  //  postToThingSpeak(String(apiKey) + "&field6=0\r\n\r\n");
}

void postReadingData(float fermenter, float chamber, int desired, float avgChange, float tolerance) {
  WiFiClient client;
  ThingSpeak.begin(client);  // Initialize ThingSpeak
  Serial.println("About to post!");
  int postReading = fermenter;
  if (fermenter < 5) {
    postReading = lastReading + avgChange;
  }
  ThingSpeak.setField(1, postReading);
  ThingSpeak.setField(2, chamber);
  ThingSpeak.setField(3, desired);
  ThingSpeak.setField(4, mode);
  ThingSpeak.setField(5, avgChange);
  ThingSpeak.setField(6, tolerance);
  int x = ThingSpeak.writeFields(atoi(tsChannel), apiKey);
  Serial.println(x);
}

/*void postToThingSpeak(String data) {
  WiFiClient client;
  if (client.connect(thingSpeakserver, 80)) { // "184.106.153.149" or api.thingspeak.com

    client.print("GET /update HTTP/1.1\n");
    client.print("Host: api.thingspeak.com\n");
    client.print("Connection: close\n");
    client.print("X-THINGSPEAKAPIKEY: " + String(apiKey) + "\n");
    client.print("Content-Type: application/x-www-form-urlencoded\n");
    client.print("Content-Length: ");
    client.print(data.length());
    client.print("\n\n");
    client.print(data);
  }
  }*/

void readSettingsFile() {
  //  Serial.printf("read file heap size start: %u\n", ESP.getFreeHeap());
  File f = SPIFFS.open("/settings.json", "r");
  if (!f) {
    Serial.println("file open failed");
    writeSettingsFile();
  }
  else {
    Serial.println("====== Reading from SPIFFS file =======");
    if (SPIFFS.exists("/settings.json")) {
      //file exists, reading and loading
      Serial.println("reading settings file");
      File settingsFile = SPIFFS.open("/settings.json", "r");
      if (settingsFile) {
        Serial.println("opened settings file");
        size_t size = settingsFile.size();
        // Allocate a buffer to store contents of the file.
        std::unique_ptr<char[]> buf(new char[size]);

        settingsFile.readBytes(buf.get(), size);
        DynamicJsonBuffer jsonBuffer;
        JsonObject& json = jsonBuffer.parseObject(buf.get());
        json.printTo(Serial);
        if (json.success()) {
          Serial.println("\nparsed json");
          if (json.containsKey("desiredTemperature")) {
            desiredTemperature = json["desiredTemperature"];
          }
          if (json.containsKey("tolerance")) {
            tolerance = json["tolerance"];
          }
          if (json.containsKey("coolingEnabled")) {
            isCoolEnabled = json["coolingEnabled"];
          }
          if (json.containsKey("heatingEnabled")) {
            isHeatEnabled = json["heatingEnabled"];
          }
        } else {
          Serial.println("failed to load json config");
        }
      }
    }
  }
  f.close();
  //  Serial.printf("red file heap size end: %u\n", ESP.getFreeHeap());

}

void writeSettingsFile() {
  //  Serial.printf("update file settings heap size: %u\n", ESP.getFreeHeap());
  if (SPIFFS.exists("/settings.json")) {
    if (SPIFFS.exists("/settings.old")) {
      SPIFFS.remove("/settings.old");
    }
    SPIFFS.rename("/settings.json", "/settings.old");
    SPIFFS.remove("/settings.json");
  }

  DynamicJsonBuffer jsonBuffer;
  JsonObject& json = jsonBuffer.createObject();
  json["desiredTemperature"] = desiredTemperature;
  json["tolerance"] = tolerance;
  json["coolingEnabled"] = isCoolEnabled;
  json["heatingEnabled"] = isHeatEnabled;

  File configFile = SPIFFS.open("/settings.json", "w");
  if (!configFile) {
    Serial.println("failed to open config file for writing");
  }

  json.printTo(Serial);
  json.printTo(configFile);
  configFile.close();
  //end save
  yield();

}

/*
  String readRestartFile() {
  String retVal = "";
  File f = SPIFFS.open("restart.txt", "r");
  if (!f) {
    Serial.println("file open failed");
  }
  else {
    Serial.println("====== Reading from SPIFFS file =======");
    bool stillReading = true;
    while (stillReading) {
      String s = f.readStringUntil('\n');
      if (s != "") {
        retVal += s;
        retVal += "<br>";
      } else {
        Serial.println("Done Reading....");
        stillReading = false;
      }
    }
  }
  f.close();
  return retVal;
  }*/

void uptime() {
  //** Making Note of an expected rollover *****//
  if (millis() >= 3000000000) {
    HighMillis = 1;

  }
  //** Making note of actual rollover **//
  if (millis() <= 100000 && HighMillis == 1) {
    Rollover++;
    HighMillis = 0;
  }
}
String uptimeString() {
  long Day = 0;
  int Hour = 0;
  int Minute = 0;
  int Second = 0;
  long secsUp = millis() / 1000;
  Second = secsUp % 60;
  Minute = (secsUp / 60) % 60;
  Hour = (secsUp / (60 * 60)) % 24;
  Day = (Rollover * 50) + (secsUp / (60 * 60 * 24)); //First portion takes care of a rollover [around 50 days]
  char buff[32];
  sprintf(buff, "%3d Days %02d:%02d:%02d", Day, Hour, Minute, Second);
  String retVal = String(buff);
  Serial.print("Uptime String: ");
  Serial.println(retVal);
  return retVal;
}
