/*
   Copyright (C) 2018 SFini

   This program is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, either version 3 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/
/**
  * @file tracker.ino 
  *
  * Main file with setup() and loop() functions
  */


#include <SoftwareSerial.h>
#include <ArduinoOTA.h>
#include <FS.h>

#include "Config.h"
#define USE_CONFIG_OVERRIDE //!< Switch to use ConfigOverride
#ifdef USE_CONFIG_OVERRIDE
  #include "ConfigOverride.h"
#endif

#include "Utils.h"
#include "StringList.h"
#include "Gps.h"
#include "Options.h"
#include "Data.h"
#include "Voltage.h"
#include "DeepSleep.h"
#include "WebServer.h"
#include "GsmPower.h"
#include "GsmGps.h"
#include "SmsCmd.h"
#include "Mqtt.h"
#include "BME280.h"


#define     PIN_POWER     D3                               //!< power on/off to DC-DC LM2596
#define     PIN_BME_POWER D4                               //!< Ground pin to the BME280 module
#define     PIN_TX        D5                               //!< Transmit-pin to the sim808 RX
#define     PIN_RX        D6                               //!< Receive-pin to the sim808 TX

MyOptions   myOptions;                                     //!< The global options.
MyData      myData;                                        //!< The global collected data.
MyVoltage   myVoltage(myOptions, myData);                  //!< Helper class for deep sleeps.
MyDeepSleep myDeepSleep(myOptions, myData);                //!< Helper class for deep sleeps.
MyWebServer myWebServer(myOptions, myData);                //!< The Webserver
MyGsmPower  myGsmPower(myData, PIN_POWER);                 //!< Helper class to switch on/off the sim808 power.
MyGsmGps    myGsmGps(myOptions, myData, PIN_RX, PIN_TX);   //!< sim808 gsm/gps communication class.
MySmsCmd    mySmsCmd(myGsmGps, myOptions, myData);         //!< sms controller class for the sms handling.
MyMqtt      myMqtt(myGsmGps, myOptions, myData);           //!< Helper class for the mqtt communication.
MyBME280    myBME280(myOptions, myData, PIN_BME_POWER);    //!< Helper class for the BME280 sensor communication.

bool        gsmHasPower = false;                           //!< Is the DC-DC modul switched on?
bool        isStarting  = false;                           //!< Are we in a starting process?
bool        isStopping  = false;                           //!< Are we in a stopping process?

/**
 * ***** IMPORTANT *****
 * It seems that the yield function of ESP does not feed the watch dog timer.
 * So we overwrite the yield() function here. If not the program crashed on some waits.
 * ***** IMPORTANT *****
 */
void yield(void)
{
   ESP.wdtFeed();
}

/** Overwritten Debug Function 
  * It logs all the debug calls to the console string-list
  * And call a refresh of the webserver for not blocking the system.
  */
void myDebugInfo(String info, bool fromWebserver, bool newline)
{
   static bool lastNewLine = true;
   
   if (newline || lastNewLine != newline) {
      String secs = String(secondsSincePowerOn()) + F(": ");

      myData.logInfos.addTail(secs + info);
      if (!lastNewLine) {
         Serial.println("");
      }
      Serial.println(secs + info);
   } else {
      String tmp = myData.logInfos.removeTail();

      Serial.print(tmp + info);
      myData.logInfos.addTail(tmp + info);
   }
   lastNewLine = newline;

   if (!fromWebserver) {
      myWebServer.handleClient();   
      myWebServer.handleClient();   
      myWebServer.handleClient();   
   } 
   delay(1);
   yield();
}

/** Overwritten delay loop for refreshing the webserver on waiting processes. */
void myDelayLoop()
{
   myWebServer.handleClient();   
   myWebServer.handleClient();   
   myWebServer.handleClient(); 
   delay(1);
   yield();
}

/** Returns the seconds since power up (not since last deep sleep). */
long secondsSincePowerOn()
{
   return myData.secondsSincePowerOn();
}

/** Main setup function. This is also called after every deep sleep. 
  * Do the initialization of every sub-component. */
void setup() 
{
   Serial.begin(115200); 
   MyDbg(F("Start SnorkTracker ..."));

   myGsmPower.begin();
   SPIFFS.begin();
   myOptions.load();
   myVoltage.begin();
   myDeepSleep.begin();
   
   myWebServer.begin();
   myMqtt.begin();
   mySmsCmd.begin();
   myBME280.begin();
}

/** Main loop function.
  * Read the power supply voltage.
  * Checks for console inputs and send them to the SIM808 modul.
  * Checks for OTA activities.
  * Start or stop the gsm and/or gps activities.
  * Check for receiving sms and process them.
  * Checks the gps and send the overall information to a mqtt server if needed.
  * Starts the deep sleep mode if needed.
  */
void loop() 
{
   myVoltage.readVoltage();
   myBME280.readValues();

   if (!myData.consoleCmds.isEmpty()) {
      String cmd = myData.consoleCmds.removeHead();
      
      myGsmGps.sendAT(cmd);
   }
   
   myWebServer.handleClient();
   
   if (myData.isOtaActive) {
      ArduinoOTA.handle();    
   }

   // Starting gsm ?
   if (myOptions.gsmPower && !gsmHasPower) {
      if (!isStarting && !isStopping) {
         isStarting = true;
         myGsmPower.on(); 
         gsmHasPower = true;
         if (!myGsmGps.begin()) {
            myGsmGps.stop();
            myGsmPower.off();
            gsmHasPower = false;
         }
         isStarting = false;
      }
   }

   // Stopping gsm ?
   if (!myOptions.gsmPower && gsmHasPower) {
      if (!isStarting && !isStopping) {
         isStopping = true;
         myGsmGps.stop();
         myGsmPower.off();
         gsmHasPower = false;
         isStopping  = false;   
      }
   }

   // Do gsm processing.
   if (gsmHasPower && !isStarting && !isStopping) {
      myGsmGps.handleClient();

      // No sms or mqtt if we are waiting for a gps position.
      // Otherwise we are sending invalid gps values.
      if (!myGsmGps.waitingForGps()) {
         if (myOptions.isSmsEnabled) {
            mySmsCmd.handleClient();
         }
         if (myOptions.isMqttEnabled) {
            myMqtt.handleClient();
         }
      }
   }

   // Deep Sleep?
   // (No deep sleep if we are waiting for a valid gps position).
   if (!myGsmGps.waitingForGps() && !myMqtt.waitingForMqtt()) {
      if (myDeepSleep.haveToSleep()) {
         if (myGsmGps.isGsmActive) {
            myGsmGps.stop();
         }
         myGsmPower.off();
         WiFi.disconnect();
         WiFi.mode(WIFI_OFF);
         yield();
         myDeepSleep.sleep();
      }
   }
   
   yield();
   delay(10); 
}
