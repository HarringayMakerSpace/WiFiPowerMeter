/*
 Sends voltage and power from China-style energy meter to the Internet.
 Collecting data by eavesdropping on the MOSI-line (master in slave out)
 between the energy monitoring chip (ECH1560) and the main processor.

 Based on the amazing work of Karl Hagström at http://gizmosnack.blogspot.co.uk/2014/10/power-plug-energy-meter-hack.html

 Updated to use ESP8266/Arduino to send the power use data to the IoT Foundation.
 Uses WiFiManager library to start an Access Point for WiFi config on first uses.
 Uses Arduino Over-The-Air (OTA) library for updating sketch over WiFi.

 Ant Elder
 License: Apache License v2
*/
#include <ESP8266WiFi.h>
extern "C" {
  #include "user_interface.h" // for wifi_station_connect
}

//these for WifiManager
#include <DNSServer.h>
#include <ESP8266WebServer.h>
#include <WiFiManager.h>          //https://github.com/tzapu/WiFiManager
                         // WifiManager bug needs to be unreleased MASTER due to https://github.com/tzapu/WiFiManager/issues/81

// these for OTA upport
#include <ESP8266mDNS.h>
#include <WiFiUdp.h>
#include <ArduinoOTA.h>  // OTA currentlr requires Arduino IDE 1.6.7
                         // OTA needs ESP module with large enough flash. Basic ESP-01/ESP-12 not big enough, ESP-12E works
                                  
#define CONFIG_BUTTON 12

const char* host = "data.sparkfun.com";
const char* streamId   = "5JpMjVaVVjcz5EA7ORjK";
const char* privateKey = "xxxxxxxxxxxxxxxxxxxx";

const int CLKPin = 4; // Pin connected to CLK (D2 & INT0)
const int MISOPin = 5;  // Pin connected to MISO (D5)

const int SEND_INTERVAL = 60 * 60 * 1000; // 60 minutes

//All variables that are changed in the interrupt function must be volatile to make sure changes are saved.
volatile int Ba = 0;   //Store MISO-byte 1
volatile int Bb = 0;   //Store MISO-byte 2
volatile int Bc = 0;   //Store MISO-byte 2
float U = 0;    //voltage
float P = 0;    //power

volatile long CountBits = 0;      //Count read bits
volatile long ClkHighCount = 0;   //Number of CLK-highs (find start of a Byte)
volatile boolean inSync = false;  //as long as we ar in SPI-sync
volatile boolean NextBit = true;  //A new bit is detected

volatile unsigned int isrTriggers; // for debugging to see if ISR routine is being called

float avgVolts, minVolts, maxVolts;
float avgWatts, minWatts, maxWatts;
int numReadings;

unsigned long lastSend;

unsigned long debugOps;

// Arduino IDE 1.6.7 bug requires functions defined above setup() and loop() - https://github.com/arduino/arduino-builder/issues/68

void doOTA() {
  ArduinoOTA.setHostname("PowerMeter-" + ESP.getChipId());

  ArduinoOTA.onStart([]() {
    Serial.println("Start");
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

  Serial.println("Ready and waiting for OTA update...");
  while (true) { // 120 secs = 2 minutes
     ArduinoOTA.handle();
     delay(10);
  } 
}

void doWifiManager() {
  WiFiManager wifiManager;
  wifiManager.setTimeout(180);
  if(!wifiManager.autoConnect("PowerMeter-" + ESP.getChipId())) {
    Serial.println("failed to connect and hit timeout. Restarting...");
    delay(3000);
    ESP.reset();
    delay(5000);
  }

  //if you get here you have connected to the WiFi
  Serial.print("WiFi connected to "); Serial.print(WiFi.SSID());
  Serial.print(", IP address: "); Serial.println(WiFi.localIP());
}

void doConfig() {
   doWifiManager();
   doOTA();
}

void clearTallys() {
  numReadings = 0;
  minVolts = 9999;
  maxVolts = -9999;
  minWatts = 9999;
  maxWatts = -9999;
}

void updateTallys(float volts, float watts) {

  // a running average calculation
  avgVolts = (volts + (numReadings * avgVolts)) / (numReadings + 1);
  avgWatts = (watts + (numReadings * avgWatts)) / (numReadings + 1);

// min and max appear not to be working on ESP8266! See https://github.com/esp8266/Arduino/issues/398
//  minVolts = min(minVolts, volts);
//  maxVolts = max(maxVolts, volts);
  if (volts < minVolts) minVolts = volts;
  if (volts > maxVolts) maxVolts = volts;
//  minWatts = min(minWatts, watts);
//  maxWatts = max(maxWatts, watts);
  if (watts < minWatts) minWatts = watts;
  if (watts > maxWatts) maxWatts = watts;

  numReadings += 1;

  Serial.print("Readings="); Serial.println(numReadings);
  Serial.print("Volts: "); Serial.print(volts);Serial.print(" avg="); Serial.print(avgVolts); Serial.print(" min="); Serial.print(minVolts); Serial.print(" max="); Serial.println(maxVolts);
  Serial.print("Watts: "); Serial.print(watts); Serial.print(" avg="); Serial.print(avgWatts); Serial.print(" min="); Serial.print(minWatts); Serial.print(" max="); Serial.println(maxWatts);
}

void doInSync() {
    CountBits = 0;  //CLK-interrupt increments CountBits when new bit is received
    while (CountBits < 40) {} //skip the uninteresting 5 first bytes
    CountBits = 0;
    Ba = 0;
    Bb = 0;
    while (CountBits < 24) { //Loop through the next 3 Bytes (6-8) and save byte 6 and 7 in Ba and Bb
      if (NextBit == true) { //when rising edge on CLK is detected, NextBit = true in in interrupt.
        if (CountBits < 9) { //first Byte/8 bits in Ba
          Ba = (Ba << 1);  //Shift Ba one bit to left and store MISO-value (0 or 1) (see http://arduino.cc/en/Reference/Bitshift)
          //read MISO-pin, if high: make Ba[0] = 1
          if (digitalRead(MISOPin) == HIGH) {
            Ba |= (1 << 0); //changes first bit of Ba to "1"
          }   //doesn't need "else" because BaBb[0] is zero if not changed.
          NextBit = false; //reset NextBit in wait for next CLK-interrupt
        }
        else if (CountBits < 17) { //bit 9-16 is byte 7, stor in Bb
          Bb = Bb << 1;  //Shift Ba one bit to left and store MISO-value (0 or 1)
          //read MISO-pin, if high: make Ba[0] = 1
          if (digitalRead(MISOPin) == HIGH) {
            Bb |= (1 << 0); //changes first bit of Bb to "1"
          }
          NextBit = false; //reset NextBit in wait for next CLK-interrupt
        }
      }
    }
    if (Bb != 3) { //if bit Bb is not 3, we have reached the important part, U is allready in Ba and Bb and next 8 Bytes will give us the Power.

      //Voltage = 2*(Ba+Bb/255)
      U = 2.0 * ((float)Ba + (float)Bb / 255.0);

      //Power:
      CountBits = 0;
      while (CountBits < 40) {} //Start reading the next 8 Bytes by skipping the first 5 uninteresting ones

      CountBits = 0;
      Ba = 0;
      Bb = 0;
      Bc = 0;
      while (CountBits < 24) { //store byte 6, 7 and 8 in Ba and Bb & Bc.
        if (NextBit == true) {
          if (CountBits < 9) {
            Ba = (Ba << 1);  //Shift Ba one bit to left and store MISO-value (0 or 1)
            //read MISO-pin, if high: make Ba[0] = 1
            if (digitalRead(MISOPin) == HIGH) {
              Ba |= (1 << 0); //changes first bit of Ba to "1"
            }
            NextBit = false;
          }
          else if (CountBits < 17) {
            Bb = Bb << 1;  //Shift Ba one bit to left and store MISO-value (0 or 1)
            //read MISO-pin, if high: make Ba[0] = 1
            if (digitalRead(MISOPin) == HIGH) {
              Bb |= (1 << 0); //changes first bit of Bb to "1"
            }
            NextBit = false;
          }
          else {
            Bc = Bc << 1;  //Shift Bc one bit to left and store MISO-value (0 or 1)
            //read MISO-pin, if high: make Bc[0] = 1
            if (digitalRead(MISOPin) == HIGH) {
              Bc |= (1 << 0); //changes first bit of Bc to "1"
            }
            NextBit = false;
          }
        }

      }

      //Power = (Ba*255+Bb)/2
      P = ((float)Ba * 255 + (float)Bb + (float)Bc / 255.0) / 2;

      if (U > 200 && U < 300 && P >= 0 && P < 4000) { // ignore spurious readings with voltage or power out of normal range
         updateTallys(U, P);
      } else {
        Serial.print(".");
      }

      inSync = false; //reset sync variable to make sure next reading is in sync.
    }

    if (Bb == 0) { //If Bb is not 3 or something else than 0, something is wrong!
      inSync = false;
      Serial.println("Nothing connected, or out of sync!");
    }
}

//Function that triggers whenever CLK-pin is rising (goes high)
void CLK_ISR() {
  isrTriggers += 1;
  //if we are trying to find the sync-time (CLK goes high for 1-2ms)
  if (inSync == false) {
    ClkHighCount = 0;
    //Register how long the ClkHigh is high to evaluate if we are at the part wher clk goes high for 1-2 ms
    while (digitalRead(CLKPin) == HIGH) {
      ClkHighCount += 1;
      delayMicroseconds(30);  //can only use delayMicroseconds in an interrupt.
    }
    //if the Clk was high between 1 and 2 ms than, its a start of a SPI-transmission
    if (ClkHighCount >= 33 && ClkHighCount <= 67) {
      inSync = true;
    }
  }
  else { //we are in sync and logging CLK-highs
    //increment an integer to keep track of how many bits we have read.
    CountBits += 1;
    NextBit = true;
  }
}

void wifiOff() {
  WiFi.mode(WIFI_OFF);
  WiFi.forceSleepBegin();
  delay(1);
  Serial.println("Wifi off");
}

boolean wifiOn() {
  WiFi.forceSleepWake();
  WiFi.mode(WIFI_STA);
  wifi_station_connect();
  WiFi.begin();
  int timeout = 40; // 20 seconds
  Serial.println("Connecting to WiFi");
  while ((WiFi.status() != WL_CONNECTED) && timeout-- > 0) {
     delay(500);
     Serial.print(".");
  }
  if (timeout < 1) {
     Serial.println("***FAILED");
     wifiOff();
     return false;
  }

  Serial.print("connected, IP address: "); Serial.println(WiFi.localIP());
  return true;
}

boolean sendToSparkfun() {
  WiFiClient client;
  int retries = 6;
  Serial.print("connecting to:"); Serial.print(host);
  while (!!!client.connect(host, 80) && (retries-- > 0)) {
    Serial.print(".");
  }
  Serial.println();

  if (!!!client.connected()) {
    Serial.println("*** connection failed");
    return false;
  }

  String url = "/input/";
  url += streamId;
  url += "?private_key=";
  url += privateKey;
  url += "&avgvolts=";
  url += avgVolts;
  url += "&minvolts=";
  url += minVolts;
  url += "&maxvolts=";
  url += maxVolts;
  url += "&avgwatts=";
  url += avgWatts;
  url += "&minwatts=";
  url += minWatts;
  url += "&maxwatts=";
  url += maxWatts;
  url += "&readings=";
  url += numReadings;
 
  Serial.print("Requesting URL: ");
  Serial.println(url);
 
  client.print(String("GET ") + url +
               " HTTP/1.1\r\n" +
               "Host: " + host + "\r\n" +
               "Connection: close\r\n\r\n");

  int timeout = 20*100; // 20 seconds             
  while(!!!client.available() && (timeout-- > 0)){
    delay(10);
  }
  if (timeout <= 0) {
    return false;
  }
 
  // Read response from server and print to Serial
  while(client.available()){
    Serial.write(client.read());
  }
  Serial.println();
  return true;
}

/* Only seems to run reliably with WiFi off while  monitoring the SPI signals.
 * Guessing thats due to the the WiFi timing and SPI monitoring timing interfering with each other
 * resulting in random WDT resets. Its possible that could be fixed with some yield() calls but for
 * now it works ok batching up the WiFi sends instead of streaming the individual power use readings.
 */
boolean sendReading() {
  detachInterrupt(digitalPinToInterrupt(CLKPin));

  boolean sentOk = false;
  if (wifiOn()) {
   sentOk = sendToSparkfun();
   wifiOff();
  }

  attachInterrupt(digitalPinToInterrupt(CLKPin), CLK_ISR, RISING);
  return sentOk;
}

void setup() {
   Serial.begin(115200); Serial.println();

   pinMode(CONFIG_BUTTON, INPUT_PULLUP);
   if (digitalRead(CONFIG_BUTTON) == LOW) {
      Serial.println("config button pressed");   
      doConfig();
   }

   Serial.println("Setup config done and ready to go...");
  
   wifiOff();
 
   //Setting up interrupt ISR on D2 (INT0), trigger function "CLK_ISR()" when INT0 (CLK)is rising
   attachInterrupt(digitalPinToInterrupt(CLKPin), CLK_ISR, RISING);

   //Set the CLK-pin (D5) to input
   pinMode(CLKPin, INPUT);

   //Set the MISO-pin (D5) to input
   pinMode(MISOPin, INPUT);

   clearTallys();
 }

void loop() {
  if ((millis() - lastSend) > SEND_INTERVAL) {
    if (sendReading()) {
       clearTallys();
    }
    lastSend = millis();
  }

  if ((millis() - debugOps) > 10000) { // debug output every 10 secs to see its running
    Serial.print("ISR triggers = "); Serial.println(isrTriggers);
    debugOps = millis();
  }

  //do nothing until the CLK-interrupt occures and sets inSync=true
  if (inSync == true) {
     doInSync();
  }
}

