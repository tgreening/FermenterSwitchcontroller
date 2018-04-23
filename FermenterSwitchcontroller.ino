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

// WiFi parameters
const int heatPin = D1;
const int coolPin = D2;
const int IR_PIN = D8;
const char* thingSpeakserver = "api.thingspeak.com";
const long READ_MILLIS = 5000UL;
const long WAIT_MILLIS = 300000UL;
const long POST_MILLIS = 1800000UL;
const char* host = "fermenterswitch";
int timezone = -5 * 3600;
int dst = 1;

float tolerance = 1.0F;
float desiredTemperature = 63.0;
int mode = 0; //1 - heating, 2 - cooling
HTTPClient http;
unsigned long waitTime = millis() + WAIT_MILLIS;
unsigned long lastReadingTime = millis() + 60000UL;
long unsigned lastPostTime = millis() + POST_MILLIS;
float lastReading, chamberReading;
float temperatureChange = 0.0F;
char apiKey[16];
int HighMillis = 0;
int Rollover = 0;

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
    html += "<br>Desired temperature: ";
    html += desiredTemperature;
    html += "<br>Tolerance: ";
    html += tolerance;
    html += "<br>Temperature Change: ";
    html += temperatureChange;
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
    html += "API Key: ";
    html += String(apiKey);
    html += "<br>Uptime: ";
    html += uptimeString();
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
    httpServer.sendHeader("Connection", "close");
    httpServer.sendHeader("Access - Control - Allow - Origin", "*");
    updateSettingsFile(desiredTemperature, tolerance);
    httpServer.send(200, "text / plain", "OK");
  });
  httpServer.on("/log", HTTP_GET, []() {
    yield();
    httpServer.sendHeader("Connection", "close");
    httpServer.sendHeader("Access-Control-Allow-Origin", "*");
    httpServer.send(200, "text / plain", readRestartFile());

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
          strcpy(apiKey, json["ThingSpeakWriteKey"]);
        } else {
          Serial.println("failed to load json config");
        }
      }
    }
  } else {
    Serial.println("failed to mount FS");
  }
  WiFiManagerParameter custom_thingspeak_api_key("key", "API key", apiKey, 40);
  wifiManager.setSaveConfigCallback(saveConfigCallback);

  //add all your parameters here
  wifiManager.addParameter(&custom_thingspeak_api_key);
  WiFi.hostname(String(host));

  if (!wifiManager.autoConnect(host)) {
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

  //save the custom parameters to FS
  if (shouldSaveConfig) {
    Serial.println("saving config");
    DynamicJsonBuffer jsonBuffer;
    JsonObject& json = jsonBuffer.createObject();
    json["ThingSpeakWriteKey"] = apiKey;

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
  configTime(timezone, dst, "pool.ntp.org", "time.nist.gov");
  Serial.println("\nWaiting for Internet time");

  while (!time(nullptr)) {
    Serial.print("*");
    delay(1000);
  }
  Serial.println("\nTime response....OK");
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
    temperatureChange = currentReading - lastReading;
    lastReading = currentReading;

    if (currentReading - desiredTemperature != 0.0F && millis() - waitTime > 0) {
      float targetTemperature = 0.0F;
      Serial.println("We need to do something");
      if (currentReading > desiredTemperature) {
        Serial.println("Too hot");
        if (mode == 2) {
          targetTemperature = desiredTemperature;
        } else {
          targetTemperature = desiredTemperature + tolerance;
        }
        if (currentReading + (temperatureChange * 3) > targetTemperature) {
          turnOnCooling();
        } else if (mode == 2) {
          Serial.println("Will get cool in 10 minutes");
          turnOff();
          waitTime = millis() + WAIT_MILLIS;
        }
      } else if (desiredTemperature > currentReading ) {
        if (mode == 1) {
          targetTemperature = desiredTemperature;
        } else {
          targetTemperature = desiredTemperature - tolerance;
        }
        Serial.println("Too cold");
        if ((currentReading + (temperatureChange * 3)) < targetTemperature) {
          Serial.println("Should turn heat on");
          turnOnHeat();
        } else if (mode == 1) {
          Serial.println("Will get hot in 10 minutes");
          turnOff();
          waitTime = millis() + WAIT_MILLIS;
        }
      } else {
        turnOff();
      }
    } else {
      Serial.println("In the range");
      turnOff();
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
  if ((long)(millis() - lastPostTime) >= 0) {
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
  while (((firstReading - secondReading) > 1.0F || (secondReading - firstReading) > 1.0F) && ((int)firstReading * 100) != 199 && retryCount < 10) {
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

void postRestartData() {
  postToThingSpeak(String(apiKey) + "&field6=0\r\n\r\n");
}

void postReadingData(float fermenter, float chamber, int desired, float avgChange, float tolerance) {
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
  postStr += "&field6=";
  postStr += String(tolerance);
  postStr += "\r\n\r\n";
  postToThingSpeak(postStr);
}

void postToThingSpeak(String data) {
  WiFiClient client;
  if (client.connect(thingSpeakserver, 80)) { // "184.106.153.149" or api.thingspeak.com

    client.print("POST /update HTTP/1.1\n");
    client.print("Host: api.thingspeak.com\n");
    client.print("Connection: close\n");
    client.print("X-THINGSPEAKAPIKEY: " + String(apiKey) + "\n");
    client.print("Content-Type: application/x-www-form-urlencoded\n");
    client.print("Content-Length: ");
    client.print(data.length());
    client.print("\n\n");
    client.print(data);
  }
}

void readSettingsFile() {
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

void updateSettingsFile(float desired, float tolerance) {
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

void writeRestartFile() {
  time_t now = time(nullptr);
  struct tm* p_tm = localtime(&now);

  //  Serial.printf("update file settings heap size: %u\n", ESP.getFreeHeap());
  File f = SPIFFS.open("restart.txt", "w");
  if (!f) {
    Serial.println("file open failed");
  } else {
    f.printf("%i/%i/%i %i:%i:%i\n", p_tm->tm_mon, p_tm->tm_mday, p_tm->tm_year, p_tm->tm_hour, p_tm->tm_min, p_tm->tm_sec);
    f.close();
    yield();
  }
}

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
}

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
  sprintf(buff, "%3d Days %2d:%2d:%2d", Day, Hour, Minute, Second);
  String retVal = String(buff);
  Serial.print("Uptime String: ");
  Serial.println(retVal);
  return retVal;
}

