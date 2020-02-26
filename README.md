# WeatherSensor
__Author:__ Richie Jarvis - richie@helkit.com
## Description
A simple ESP8266 or ESP32 compatible piece of code to read a BME280 sensor data, and send via wifi to an Elastic Stack.

![Wiring Diagram](https://github.com/richiejarvis/iotprojects/blob/master/WeatherSensor/ESP32-BME280.png)

## Features
1. Auto wifi configuration using https://github.com/prampec/IotWebConf
2. Elasticsearch configuration parameters and WiFi are stored in flash on-chip once configured.
2. Read from BME280 and compatible Temperature, Pressure and Humidity sensors via i2c
3. Send JSON with the following Elasticsearch mapping definition (v0.1.3+)
```
{
  "@timestamp":1582039397,
  "pressure":1015.41,
  "temperature":18.25,
  "humidity":44.99,
  "errorState": "NONE",
  "sensorName":"GB-RJ-ObsyTest",
  "firmwareVersion":"v0.1.3",
  "environment":"outdoor",
  "location":"50.0,0.0"
}
```
4. Data Buffering during WiFi outages
5. Display Config and last 100 loglines in webpage

## Template
Elasticsearch will normally detect these datatypes, however, if you wish to create a template, here is what I use:

```
PUT _template/weathersensor_template
{
  "version": 1,
  "order": 1,
  "index_patterns": [
    "weather-*"
  ],
  "settings": {
    "index": {
      "lifecycle": {
        "name": "weather-ilm",
        "rollover_alias": "weather-alias"
      },
      "default_pipeline": "weathersensor-add-fields",
      "number_of_shards": "2",
      "number_of_replicas": "1"
    }
  },
  "mappings": {
    "_doc": {
      "_routing": {
        "required": false
      },
      "numeric_detection": false,
      "_meta": {},
      "dynamic": true,
      "_source": {
        "excludes": [],
        "includes": [],
        "enabled": true
      },
      "dynamic_templates": [],
      "date_detection": false,
      "properties": {
        "sensorName": {
          "type": "keyword"
        },
        "pressure": {
          "type": "float"
        },
        "environment": {
          "type": "keyword"
        },
        "@timestamp": {
          "format": "epoch_second",
          "type": "date"
        },
        "celsius": {
          "type": "float"
        },
        "errorState": {
          "type": "keyword"
        },
        "dewpointCelsius": {
          "type": "float"
        },
        "dewpointFahrenheit": {
          "type": "float"
        },
        "temperature": {
          "type": "float"
        },
        "humidity": {
          "type": "float"
        },
        "location": {
          "type": "geo_point"
        },
        "fahrenheit": {
          "type": "float"
        },
        "firmwareVersion": {
          "type": "keyword"
        }
      }
    }
  }
}
```
I am using an Ingest Pipeline too:
```
{
  "weathersensor-add-fields" : {
    "description" : "Converts Celsius to Fahrenheit, calcs dewpoint and stores in new fields",
    "processors" : [
      {
        "script" : {
          "lang" : "painless",
          "source" : """
            // ctx._source.remove('celcius');
            ctx['celsius'] = ctx['temperature'];
            ctx['fahrenheit'] = (double)Math.round(((ctx['temperature']*9/5)+32) *100 ) /100  ;
            ctx['dewpointCelsius'] = (double)Math.round( (ctx['temperature'] - (100 - ctx['humidity']) / 5 ) *100)/100;
            ctx['dewpointFahrenheit'] = (double)Math.round(((ctx['dewpointCelsius'] * 9 / 5) + 32)*100)/100;

          """
        }
      }
    ]
  }
}
```
and ILM:
```
PUT _ilm/policy/weather-ilm
{
  "policy": {
    "phases": {
      "hot": {
        "min_age": "0ms",
        "actions": {
          "rollover": {
            "max_size": "10gb"
          },
          "set_priority": {
            "priority": 100
          }
        }
      }
    }
  }
}
```
## Version History
* v0.0.1 - Initial Release
* v0.0.2 - Added ES params
* v0.0.3 - I2C address change tolerance & lat/long
* v0.0.4 - SSL support
* v0.1.0 - Display all the variables
* v0.1.1 - Store seconds since epoch, and increment as time passes to reduce ntp call
* v0.1.2 - Fix reset issue (oops! Connecting Pin 12 and GND does not reset AP password).
         Added indoor/outdoor parameter.
         Added Fahrenheit conversion.
* v0.1.3 - Changed the schema slightly and added a Buffer for the data, and logging to the webpage
## Feature Details
### Internet Outage Buffer
The idea was to have a way to store short outages.  1,200 readings at one per second gives 20 minutes of storage.
This is the console output during an outage:
![console1](https://user-images.githubusercontent.com/900210/74748883-2720fc80-5261-11ea-8872-bc12d5c321b1.png)
When the WiFi disconnects, the unit begins to store each reading.  After the deepskyblack WiFi AP had rebooted, after a short delay the WeatherSensor automatically reconnects, and sends the stored data into Elasticsearch.
![console2](https://user-images.githubusercontent.com/900210/74749026-64858a00-5261-11ea-8aba-18154734d947.png)
Once the store is empty, everything goes back to normal.
![console3](https://user-images.githubusercontent.com/900210/74749043-6fd8b580-5261-11ea-9dbc-b2879aed0308.png)

### Logging on the Webpage
I wanted to make the webpage more user-friendly, and report on the current status.  Here it is:
![webpage1](https://user-images.githubusercontent.com/900210/74749151-9f87bd80-5261-11ea-8677-495ab26b0fc5.png)







