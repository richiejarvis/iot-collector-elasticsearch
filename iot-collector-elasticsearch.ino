// iot-collector-elasticsearch.ino - Richie Jarvis - richie@helkit.com
// Version: v1.0.1 - 2020-03-09
// Github: https://github.com/richiejarvis/iot-collector-elasticsearch
// Version History
// v0.0.1 - Initial Release
// v0.0.2 - Added ES params
// v0.0.3 - I2C address change tolerance & lat/long
// v0.0.4 - SSL support
// v0.1.0 - Display all the variables
// v0.1.1 - Store seconds since epoch, and increment as time passes to reduce ntp call
// v0.1.2 - Fix reset issue (oops! Connecting Pin 12 and GND does not reset AP password).
//          Added indoor/outdoor parameter.
//          Added Fahrenheit conversion.
// v0.1.3 - Changed the schema slightly and added a Buffer for the data, and logging to the webpage
// v0.1.4 - Bug fixes
// v1.0.0 - Name change, own repo, and slowed dump of cached data...
//          Added Reboot facility
//          Reversed log display in webpage
//          Improved time storage to properly use time.h :)
//          Added Heap size to metrics stored...
// v1.0.1 - Removed delay time to allow easier wifi access
//          Config Reset!


// Store the IotWebConf config version.  Changing this forces IotWebConf to ignore previous settings
// A useful alternative to the Pin 12 to GND reset
#define CONFIG_VERSION "017"
#define CONFIG_VERSION_NAME "v1.0.1m"

#include <IotWebConf.h>
#include <Adafruit_Sensor.h>
#include <Adafruit_BME280.h>
#include <WiFi.h>
#include <Wire.h>
#include <SPI.h>
#include "time.h"
#include <HTTPClient.h>
#include <RingBuf.h>

// IotWebConf max lengths
#define STRING_LEN 255
#define NUMBER_LEN 32
// IotWebConf -- When CONFIG_PIN is pulled to ground on startup, the Thing will use the initial
//      password to build an AP. (E.g. in case of lost password)
#define CONFIG_PIN 12
// IotWebConf -- Status indicator pin.
////      First it will light up (kept LOW), on Wifi connection it will blink,
////      when connected to the Wifi it will turn off (kept HIGH).
#define STATUS_PIN 13

// Setup the Adafruit library
Adafruit_BME280 bme;
Adafruit_Sensor *bme_temp = bme.getTemperatureSensor();
Adafruit_Sensor *bme_pressure = bme.getPressureSensor();
Adafruit_Sensor *bme_humidity = bme.getHumiditySensor();

// Here are my Variables
// Constants
const char thingName[] = "WeatherSensor";
const char wifiInitialApPassword[] = "WeatherSensor";
const char* ntpServer = "pool.ntp.org";
// Other Variables
// I want to figure how to change the AP name to the one on the form.
// The thingName[] appears in the form, but I cannot seem to get it out
// and into the JSON.  Annoying...  One to fix later!
// More IotWebConf vars to hold the settings between power cycles
char iwcThingName[STRING_LEN] = "WeatherSensor";
char elasticPrefixForm[STRING_LEN] = "https://";
char elasticUsernameForm[STRING_LEN] = "weather";
char elasticPassForm[STRING_LEN] = "password";
char elasticHostForm[STRING_LEN] = "sensor.helkit.com";
char elasticPortForm[STRING_LEN] = "443";
char elasticIndexForm[STRING_LEN] = "weather-alias";
char latForm[NUMBER_LEN] = "50.0";
char lngForm[NUMBER_LEN] = "0.0";
char envForm[STRING_LEN] = "indoor";

long nextNtpTime = 0;
long prevTime = 0;
// Store data that is not sent for later delivery
RingBuf<String, 600> storageBuffer;
// Log store
RingBuf<String, 20> logBuffer;

long upTime = 0;
// -- Callback method declarations.
void configSaved();
bool formValidator();
// DNS and Webserver Initialisation
HTTPClient http;
DNSServer dnsServer;
HTTPUpdateServer httpUpdater;
String url = "";
WebServer server(80);
// Setup the Form Value to Parameter
IotWebConf iotWebConf(iwcThingName, &dnsServer, &server, wifiInitialApPassword, CONFIG_VERSION);
IotWebConfParameter elasticPrefix = IotWebConfParameter("Elasticsearch URL Scheme:", "elasticPrefix", elasticPrefixForm, STRING_LEN);
IotWebConfParameter elasticUsername = IotWebConfParameter("Elasticsearch Username", "elasticUsername", elasticUsernameForm, STRING_LEN);
IotWebConfParameter elasticPass = IotWebConfParameter("Elasticsearch Password", "elasticPass", elasticPassForm, STRING_LEN, "password");
IotWebConfParameter elasticHost = IotWebConfParameter("Elasticsearch Hostname", "elasticHost", elasticHostForm, STRING_LEN);
IotWebConfParameter elasticPort = IotWebConfParameter("Elasticsearch Port", "elasticPort", elasticPortForm, STRING_LEN);
IotWebConfParameter elasticIndex = IotWebConfParameter("Elasticsearch Index", "elasticIndex", elasticIndexForm, STRING_LEN);
IotWebConfParameter latValue = IotWebConfParameter("Decimal Longitude", "latValue", latForm, NUMBER_LEN, "number", "e.g. 41.451", NULL, "step='0.001'");
IotWebConfParameter lngValue = IotWebConfParameter("Decimal Latitude", "lngValue", lngForm, NUMBER_LEN, "number", "e.g. -23.712", NULL, "step='0.001'");
IotWebConfParameter environment = IotWebConfParameter("Environment Type (indoor/outdoor)", "environment", envForm, STRING_LEN);

// Setup everything...
void setup() {
  Serial.begin(115200);
  debugOutput("INFO: Starting WeatherSensor " + (String)CONFIG_VERSION_NAME);
  // Initialise IotWebConf
  iotWebConf.setStatusPin(STATUS_PIN);
  iotWebConf.setConfigPin(CONFIG_PIN);
  iotWebConf.addParameter(&elasticPrefix);
  iotWebConf.addParameter(&elasticUsername);
  iotWebConf.addParameter(&elasticPass);
  iotWebConf.addParameter(&elasticHost);
  iotWebConf.addParameter(&elasticPort);
  iotWebConf.addParameter(&elasticIndex);
  iotWebConf.addParameter(&latValue);
  iotWebConf.addParameter(&lngValue);
  iotWebConf.addParameter(&environment);
  iotWebConf.setConfigSavedCallback(&configSaved);
  iotWebConf.setupUpdateServer(&httpUpdater);
  iotWebConf.setFormValidator(&formValidator);
  iotWebConf.getApTimeoutParameter()->visible = true;

  // IotWebConf initialise
  iotWebConf.init();
  // IotWebConf -- Set up required URL handlers on the web server.
  server.on("/", handleRoot);
  server.on("/config", [] { iotWebConf.handleConfig(); });
  server.on("/reboot", handleReboot);
  server.onNotFound([]() {
    iotWebConf.handleNotFound();
  });

  /* Initialise the sensor */
  // This part tries I2C address 0x77 first, and then falls back to using 0x76.
  // If there is no I2C data with these addresses on the bus, then it reports a Fatal error, and stops
  if (!bme.begin(0x77, &Wire)) {
    debugOutput("INFO: BME280 not using 0x77 I2C address - checking 0x76");
    if (!bme.begin(0x76, &Wire)) {
      debugOutput("FATAL: Could not find a valid BME280 sensor, check wiring!");
      while (1) delay(10);
    }
  }
  // Output Sensor details from the I2C bus
  bme_temp->printSensorDetails();
  bme_pressure->printSensorDetails();
  bme_humidity->printSensorDetails();
  buildUrl();
  upTime = millis() / 1000;
  debugOutput("INFO: Initialisation completed");
}


//  This is where we do stuff again and again...
void loop() {
  upTime = millis() / 1000;
  iotWebConf.doLoop();
  // Get the real time via NTP for the first time
  // Or when the refresh timer expires
  // Don't try if not connected
  if (isConnected() && nextNtpTime < upTime) {
    struct tm timeinfo;
    configTime(0, 0, ntpServer);
    if (!getLocalTime(&timeinfo))
    {
      debugOutput("ERROR: Cannot connect to NTP server");
    } else {
      debugOutput("INFO: NTP Server Time Now: " + (String)time(NULL));
      nextNtpTime = upTime + 600 ;
    }
  }
  // The meat of the reading and sending is here
  if (nextNtpTime > 0 && prevTime != upTime) {
    storeSample();
    if (isConnected()) {
      if (sendData()) {
        delay(20);
        sendData();
      }
    }
  }
  prevTime = upTime;
  delay(20);
}


boolean isConnected() {
  if (iotWebConf.getState() == 4) {
    return true;
  }
  return false;
}


boolean storeSample() {
  sensors_event_t temp_event, pressure_event, humidity_event;
  bme_temp->getEvent(&temp_event);
  bme_pressure->getEvent(&pressure_event);
  bme_humidity->getEvent(&humidity_event);
  // Store the values from the BME280 in local vars
  float temperature = temp_event.temperature;
  float humidity = humidity_event.relative_humidity;
  float pressure = pressure_event.pressure;
  // Store whether the sensor was connected
  // Sanity check to make sure we are not underwater, or in space!
  if (temperature < -40.00 || temperature > 60) {
    debugOutput("ERROR: Sensor Failure");
    if (pressure > 1100.00 || pressure < 300.00) {
      debugOutput("ERROR: Sensor Failure");
    }
    return false;
  }
  // Build the dataset to send
  String dataSet = "{\"@timestamp\":";
  dataSet += (String)time(NULL);
  dataSet += ",\"pressure\":";
  dataSet += (String)pressure;
  dataSet += ",\"temperature\":";
  dataSet += (String)temperature;
  dataSet += ",\"humidity\":";
  dataSet += (String)humidity;
  dataSet += ",\"freeHeap\":";
  dataSet += (String)(ESP.getFreeHeap()/1000);
  dataSet += ",\"upTime\":";
  dataSet += (String)upTime;
  dataSet += ",\"sensorName\":\"";
  if (storageBuffer.lockedPush(dataSet)) {
    return true;
  }
  return false;
}

boolean sendData() {
  int httpCode = 0;
  String dataSet = "";
  if (storageBuffer.lockedPop(dataSet) && httpCode >= 0)
  {
    http.begin(url);
    http.addHeader("Content-Type", "application/json");
    dataSet += (String)iotWebConf.getThingName();
    dataSet += "\",\"firmwareVersion\":\"";
    dataSet += (String)CONFIG_VERSION_NAME;
    dataSet += "\",\"environment\":\"";
    dataSet += (String)envForm;
    dataSet += "\",\"location\":\"";
    dataSet += (String)latForm;
    dataSet += ",";
    dataSet += (String)lngForm;
    dataSet += "\"}";
    httpCode = http.POST(dataSet);
    if (httpCode > 200 && httpCode < 299) {
      debugOutput("INFO: waiting:" + (String)storageBuffer.size() + " status:" + (String)httpCode + " dataset: " + dataSet);
      http.end();
    } else {
      rollingStorageBuffer(dataSet);
      debugOutput("ERROR:" + (String)httpCode + ":" + http.errorToString(httpCode).c_str());
      http.end();
      return false;
    }
  }
  return true;
}

void rollingLogBuffer(String line) {
  String throwAway = "";
  if (logBuffer.isFull()){
    logBuffer.lockedPop(throwAway);
  }
  logBuffer.lockedPush(line);
}

void rollingStorageBuffer(String line) {
  String throwAway = "";
  if (storageBuffer.isFull()){
    storageBuffer.lockedPop(throwAway);
  }
  storageBuffer.lockedPush(line);
}

void handleReboot()
{
  // -- Let IotWebConf test and handle captive portal requests.
  if (iotWebConf.handleCaptivePortal())
  {
    // -- Captive portal request were already served.
    return;
  }
  server.sendHeader("Location", "/");
  server.send(303);
  delay(100);
  ESP.restart();
}
/**
   Handle web requests to "/" path.
*/
void handleRoot()
{
  // -- Let IotWebConf test and handle captive portal requests.
  if (iotWebConf.handleCaptivePortal())
  {
    // -- Captive portal request were already served.
    return;
  }
  String s = "<!DOCTYPE html><html lang='en'><head><meta http-equiv='refresh' content='60'><meta name='viewport' content='width=device-width, initial-scale=1, user-scalable=no'/>";
  s += "<title>" + (String)iotWebConf.getThingName() + " - WeatherStation - " + (String)CONFIG_VERSION_NAME + "</title></head><body>";
  s += "<h1>" + (String)iotWebConf.getThingName() + " - WeatherStation - " + (String)CONFIG_VERSION_NAME + "</h1>";
  s += "<h2>Uptime:";
  s += (String)upTime;
  s += "<h2>Free Heap:";
  s += (String)(ESP.getFreeHeap()/1000);
  s += "</h2><h2>Current Settings</h2>";
  s += "<ul>";
  s += "<p>";
  s += "<li>SensorName/Config AP: ";
  s += (String)iotWebConf.getThingName();
  s += "<li>Elasticsearch Username: ";
  s += elasticUsernameForm;
  s += "<li>Elasticsearch Hostname: ";
  s += (String)elasticPrefixForm + (String)elasticHostForm + ":" + (String)elasticPortForm;
  s += "<li>Elasticsearch Index/Alias: ";
  s += elasticIndexForm;
  s += "<li>Sensor Environment Type: ";
  s += envForm;
  s += "<li>Sensor Latitude: ";
  s += latForm;
  s += "<li>Sensor Longitude: ";
  s += lngForm;
  s += "</ul>";
  s += "<p>";
  s += "<p>";
  s += "Click <a href='config'>Configure</a> to setup this unit.<br>";
  s += "Click <a href='reboot'>Reboot</a> to reboot this unit.<br>";
  s += "<p><i>Connect pin " + (String) CONFIG_PIN + " to GND and Reset the board to reset the Configuration AP password to: " + (String)wifiInitialApPassword + "</i>";
  s += "<p>Offline storageBuffer Records in Storage:";
  s += storageBuffer.size();
  s += "<p>Last ";
  s += (String)logBuffer.size();
  s += " loglines:<br><code>";
  for (int count = logBuffer.size() - 1 ; count >= 0 ; count--) {
    s += logBuffer[count];
    s += "<br>";
  }
  s += "</code><br><p> ";
  s += "</body></html>\n";
  server.send(200, "text/html", s);
}

void configSaved()
{
  buildUrl();
  debugOutput("INFO: Configuration was updated.");
  handleReboot();
}

bool formValidator()
{
  bool valid = true;
  int countOfVars = server.args();
  debugOutput("INFO: Number of Variables Stored: " + String(countOfVars));
  // Check we have the right number of variables (14 at last count)
  if (countOfVars != 15)
  {
    debugOutput("ERROR: Form validation failed");
    valid = false;
  }
  // TODO: Add some validation of params here.
  return valid;
}

// Simple Logging Output...
void debugOutput(String textToSend)
{
  String text = "t:" + (String)upTime + ":m:" + (String)ESP.getFreeHeap() + ":" + textToSend;
  Serial.println(text);
  rollingLogBuffer(text);
}

void buildUrl() {
  // Build the URL to send the JSON structure to
  url = elasticPrefixForm;
  url += elasticUsernameForm;
  url += ":";
  url += elasticPassForm;
  url += "@";
  url += elasticHostForm;
  url += ":";
  url += elasticPortForm;
  url += "/";
  url += elasticIndexForm;
  url += "/_doc";
}
