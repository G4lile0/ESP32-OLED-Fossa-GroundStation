/*
The aim of this project is to create an open network of ground stations for the Fossa Satellites distributed all over the world and connected through Internet.
This project is based on ESP32 boards and is compatible with sx126x and sx127x you can build you own board using one of these modules but most of us use a development 
board like the ones listed in the Supported boards section.
The developers of this project have no relation with the Fossa team in charge of the mission, we are passionate about space and created this project to be able to track
and use the satellites as well as supporting the mission.

Supported boards
    Heltec WiFi LoRa 32 V1 (433MHz SX1278)
    Heltec WiFi LoRa 32 V2 (433MHz SX1278) 
    TTGO LoRa32 V1 (433MHz SX1278)
    TTGO LoRa32 V2 (433MHz SX1278)

Supported modules
    sx126x
    sx127x

    World Map with active Ground Stations and satellite stimated possition 
    Main community chat: https://t.me/joinchat/DmYSElZahiJGwHX6jCzB3Q
      In order to onfigure your Ground Station please open a private chat to get your credentials https://t.me/fossa_updates_bot
    Data channel (station status and received packets): https://t.me/FOSSASAT_DATA
    Test channel (simulator packets received by test groundstations): https://t.me/FOSSASAT_TEST

    Developers:
              @gmag12       https://twitter.com/gmag12
              @dev_4m1g0    https://twitter.com/dev_4m1g0
              @g4lile0       https://twitter.com/G4lile0

    LICENSE 	GNU General Public License v3.0 
  */

// include the library
#include "arduino_ota.h"
#include "config_manager.h"
#include <RadioLib.h>
#include <SPI.h>
#include "Comms.h"
#include <Wire.h>  // Only needed for Arduino 1.6.5 and earlier
#include "SSD1306.h" // alias for `#include "SSD1306Wire.h"       https://github.com/ThingPulse/esp8266-oled-ssd1306
#include "OLEDDisplayUi.h"                                  //    https://github.com/ThingPulse/esp8266-oled-ssd1306
#include "graphics.h"
#include <ArduinoJson.h>                                    //    https://github.com/bblanchon/ArduinoJson
#include <WiFi.h>
#include "time.h"
#include <sys/time.h>
#include <ESPmDNS.h>
#include <Update.h>
#include <ArduinoOTA.h>

#if defined(ARDUINO) && ARDUINO >= 100
#include "Arduino.h"
#else
#include "WProgram.h"
#endif

#include "esp32_mqtt_client.h"
#include <WiFi.h>
#include <AsyncTCP.h>										// https://github.com/me-no-dev/AsyncTCP
#include <FS.h>
#include <SPIFFS.h>
#include <DNSServer.h>
#include <ESPAsyncWebServer.h>								// https://github.com/me-no-dev/ESPAsyncWebServer
#include <ESPAsyncWiFiManager.h>							// https://github.com/alanswx/ESPAsyncWiFiManager
#include "BoardConfig.h"


#define RADIO_TYPE        SX1278  // type of radio module to be used
//#define RADIO_SX126X            // also uncomment this line when using SX126x!!!


constexpr auto LOG_TAG = "FOSSAGS";

Esp32_mqtt_clientClass mqtt;

#define WAIT_FOR_BUTTON 3000
#define RESET_BUTTON_TIME 5000
#define PROG_BUTTON 0

//**********************
// User configuration //

boardconfig_t board_config;
Config_managerClass config_manager (&board_config);

// Oled board configuration  uncomment your board
// SSD1306 display( address, OLED_SDA, OLED_SCL)

#ifdef TTGO_V2
  SSD1306 display(0x3c, 21, 22); // configuration for TTGO v2 (SMA antenna connector)
#elif OLED_SDA // TTGO
  SSD1306 display(0x3c, OLED_SDA, OLED_SCL);      
#else
  SSD1306 display(0x3c, SDA_OLED, SCL_OLED);         // configuration for TTGO v1, Heltec v1 and 2  
#endif
#define OLED_RST  16                   // seems that all board until now use pin 16

//*********************

const int fs_version = 1912161;      // version year month day release

const char*  message[32];

OLEDDisplayUi ui     ( &display );

bool mqtt_connected = false;

void manageMQTTEvent (esp_mqtt_event_id_t event) {
  if (event == MQTT_EVENT_CONNECTED) {
	  mqtt_connected = true;
    String topic = "fossa/" + String(board_config.mqtt_user) + "/" + String(board_config.station) + "/data/#";
    mqtt.subscribe (topic.c_str());
    mqtt.subscribe ("fossa/global/sat_pos_oled");
	  welcome_message ();
    
  } else   if (event == MQTT_EVENT_DISCONNECTED) {
    mqtt_connected = false;
  }
}

uint8_t sat_pos_oled[2] = {0,0};
void manageSatPosOled(char* payload, size_t payload_len) {
  DynamicJsonDocument doc(60);
  char payloadStr[payload_len+1];
  memcpy(payloadStr, payload, payload_len);
  payloadStr[payload_len] = '\0';
  deserializeJson(doc, payload);
  sat_pos_oled[0] = doc[0];
  sat_pos_oled[1] = doc[1];
}

void manageMQTTData (char* topic, size_t topic_len, char* payload, size_t payload_len) {
  // Don't use Serial.print here. It will not work. Use ESP_LOG or printf instead.
  ESP_LOGI (LOG_TAG,"Received MQTT message: %.*s : %.*s\n", topic_len, topic, payload_len, payload);
  char topicStr[topic_len+1];
  memcpy(topicStr, topic, topic_len);
  topicStr[topic_len] = '\0';
  if (!strcmp(topicStr, "fossa/global/sat_pos_oled")) {
    manageSatPosOled(payload, payload_len);
  }
}

const char* ntpServer = "pool.ntp.org";
const long  gmtOffset_sec = 0; // 3600;         // 3600 for Spain
const int   daylightOffset_sec = 0; // 3600;

void printLocalTime()
{
  struct tm timeinfo;
  if(!getLocalTime(&timeinfo)){
    Serial.println("Failed to obtain time");
    return;
  }
  Serial.println(&timeinfo, "%A, %B %d %Y %H:%M:%S");
}

//OLED pins to ESP32 TTGO v1 GPIOs 
//OLED_SDA — GPIO4
//OLED_SCL — GPIO15
//OLED_RST — GPIO16
// WIFI_LoRa_32 ports
// GPIO5 — SX1278’s SCK
// GPIO19 — SX1278’s MISO
// GPIO27 — SX1278’s MOSI
// GPIO18 — SX1278’s CS
// GPIO14 — SX1278’s RESET
// GPIO26 — SX1278’s IRQ(Interrupt Request)

// SX1278 - SX1268 has the following connections:

// pin definitions
#define CS                18
#define DIO0              26
#define DIO1              12      // not connected on the ESP32 - TTGO
#define BUSY              12




#ifdef RADIO_SX126X
  RADIO_TYPE lora = new Module(CS, DIO0, BUSY);
#else
  RADIO_TYPE lora = new Module(CS, DIO0, DIO1);
#endif

// modem configuration
#define LORA_CARRIER_FREQUENCY                          436.7f  // MHz
#define LORA_BANDWIDTH                                  125.0f  // kHz dual sideband
#define LORA_SPREADING_FACTOR                           11
#define LORA_SPREADING_FACTOR_ALT                       10
#define LORA_CODING_RATE                                8       // 4/8, Extended Hamming
#define LORA_OUTPUT_POWER                               21      // dBm
#define LORA_CURRENT_LIMIT                              120     // mA
#define SYNC_WORD_7X                                    0xFF    // sync word when using SX127x
#define SYNC_WORD_6X                                    0x0F0F  //                      SX126x




// satellite callsign
char callsign[] = "FOSSASAT-1";

// Last packet received         "22:23:23"
String last_packet_received_time = " Waiting      ";
float last_packet_received_rssi;
float last_packet_received_snr;
float last_packet_received_frequencyerror;

// flag to indicate that a packet was received
volatile bool receivedFlag = false;

// disable interrupt when it's not needed
volatile bool enableInterrupt = true;

// this function is called when a complete packet
// is received by the module
// IMPORTANT: this function MUST be 'void' type
//            and MUST NOT have any arguments!
void setFlag(void) {
  // check if the interrupt is enabled
  if(!enableInterrupt) {
    return;
  }

  // we got a packet, set the flag
  receivedFlag = true;
}

/////// OLED ANIMATION //////

void msOverlay(OLEDDisplay *display, OLEDDisplayUiState* state) {
  display->setTextAlignment(TEXT_ALIGN_RIGHT);
  display->setFont(ArialMT_Plain_10);
 // display->drawString(128, 0, String(millis()));

  struct tm timeinfo;
  if(!getLocalTime(&timeinfo)){
    Serial.println("Failed to obtain time");
    return;
  }

   // ********************* 
  // display time in digital format
  String thisTime="";
  if (timeinfo.tm_hour < 10){ thisTime=thisTime + " ";} // add leading space if required
  thisTime=String(timeinfo.tm_hour) + ":";
  if (timeinfo.tm_min < 10){ thisTime=thisTime + "0";} // add leading zero if required
  thisTime=thisTime + String(timeinfo.tm_min) + ":";
  if (timeinfo.tm_sec < 10){ thisTime=thisTime + "0";} // add leading zero if required
  thisTime=thisTime + String(timeinfo.tm_sec);
  const char* newTime = (const char*) thisTime.c_str();
  display->drawString(128, 0,  newTime  );
}

void drawFrame1(OLEDDisplay *display, OLEDDisplayUiState* state, int16_t x, int16_t y) {
  // draw an xbm image.
  // Please note that everything that should be transitioned
  // needs to be drawn relative to x and y

  display->drawXbm(x , y + 14, Fossa_Logo_width, Fossa_Logo_height, Fossa_Logo_bits);
  display->setFont(ArialMT_Plain_10);
  display->setTextAlignment(TEXT_ALIGN_CENTER);
  display->drawString( x+70, y + 40, "Sta: "+ String(board_config.station));
  }


//Initial dummy System info:
float batteryChargingVoltage = 0.0f;
float batteryChargingCurrent = 0.0f;
float batteryVoltage = 0.0f;
float solarCellAVoltage = 0.0f;
float solarCellBVoltage = 0.0f;
float solarCellCVoltage = 0.0f;
float batteryTemperature = 0.0f;
float boardTemperature = 0.0f;
int mcuTemperature = 0;
int resetCounter = 0;
byte powerConfig = 0b11111111;

// on frame animation
int graphVal = 1;
int delta = 1;
unsigned long tick_interval;
int tick_timing = 100;


void drawFrame2(OLEDDisplay *display, OLEDDisplayUiState* state, int16_t x, int16_t y) {
  // Demo for drawStringMaxWidth:
  // with the third parameter you can define the width after which words will be wrapped.
  // Currently only spaces and "-" are allowed for wrapping
 // display->setTextAlignment(TEXT_ALIGN_LEFT);
 // display->setFont(ArialMT_Plain_10);
 // display->drawStringMaxWidth(0 + x, 10 + y, 128, "Lorem ipsum\n dolor sit amet, consetetur sadipscing elitr, sed diam nonumy eirmod tempor invidunt ut labore.");

  // draw an xbm image.
  // Please note that everything that should be transitioned
  // needs to be drawn relative to x and y
  display->setTextAlignment(TEXT_ALIGN_LEFT);
  display->setFont(ArialMT_Plain_10);
  display->drawXbm(x + 34, y + 22, bat_width, bat_height, bat_bits);
  display->drawString( x+44, y + 10, String(batteryVoltage) + "V");
  display->drawString( x+13,  22+y,  String(batteryChargingVoltage));
  display->drawString( x+13,  35+y,  String(batteryChargingCurrent));
  display->drawString( x+80,  32+y,  String(batteryTemperature) + "ºC" );


  if ((millis()-tick_interval)>200) {
              // Change the value to plot
                  graphVal-=1;
                  tick_interval=millis();
                  if (graphVal <= 1) {graphVal = 8; } // ramp up value
       }


  display->fillRect(x+48, y+32+graphVal, 25 , 13-graphVal);
  
}


void drawFrame3(OLEDDisplay *display, OLEDDisplayUiState* state, int16_t x, int16_t y) {
  display->setTextAlignment(TEXT_ALIGN_LEFT);
  display->setFont(ArialMT_Plain_10);
  display->drawString(0 + x,  10+ y, "Solar panels:");
  //display->drawString( x,  12 +y, "Bat:" + String(batteryVoltage) + "V Ch:" + String(batteryChargingVoltage) + "V " + String(batteryChargingCurrent)+ "A"  );
  display->drawString( x,  21+y, "A:" + String(solarCellAVoltage) + "V  B:" + String(solarCellBVoltage) + "V  C:" + String(solarCellCVoltage)+ "V"  );
  display->drawString( x,  38+y, "T uC: " + String(boardTemperature) + "ºC   Reset: " + String(resetCounter)  );

}

void drawFrame4(OLEDDisplay *display, OLEDDisplayUiState* state, int16_t x, int16_t y) {
  display->setTextAlignment(TEXT_ALIGN_LEFT);
  display->setFont(ArialMT_Plain_10);
  display->drawString(0 + x,  11+ y, "Last Packet: "+last_packet_received_time);
  display->drawString( x,  23+y, "RSSI:" + String(last_packet_received_rssi) + "dBm" );
  display->drawString( x,  34+y, "SNR: "+ String(last_packet_received_snr) +"dB" );
  display->drawString( x, 45+ y, "Freq error: " + String(last_packet_received_frequencyerror) +" Hz");
}

void drawFrame5(OLEDDisplay *display, OLEDDisplayUiState* state, int16_t x, int16_t y) {
  display->setTextAlignment(TEXT_ALIGN_LEFT);
  display->drawString( x+100,  21+y, "MQTT:" );
  if (mqtt_connected ) {display->drawString( x+105,  31+y, "ON" );}  else {display->drawString( x+102,  31+y, "OFF" );}
     display->drawXbm(x + 34, y + 4, WiFi_Logo_width, WiFi_Logo_height, WiFi_Logo_bits);
  // The coordinates define the center of the text
  display->setTextAlignment(TEXT_ALIGN_CENTER);
    display->drawString(64 + x, 42 + y, "Connected "+(WiFi.localIP().toString()));



/*  
 *   
  // Text alignment demo
  display->setFont(ArialMT_Plain_10);

  // The coordinates define the left starting point of the text
  display->setTextAlignment(TEXT_ALIGN_LEFT);
  display->drawString(0 + x, 11 + y, "Left aligned (0,10)");

  // The coordinates define the center of the text
  display->setTextAlignment(TEXT_ALIGN_CENTER);
  display->drawString(64 + x, 22 + y, "Center aligned (64,22)");

  // The coordinates define the right end of the text
  display->setTextAlignment(TEXT_ALIGN_RIGHT);
  display->drawString(128 + x, 33 + y, "Right aligned (128,33)");

 */

}

void drawFrame6(OLEDDisplay *display, OLEDDisplayUiState* state, int16_t x, int16_t y) {
  display->drawXbm(x , y , earth_width, earth_height, earth_bits);
  display->setColor(BLACK);
  display->setTextAlignment(TEXT_ALIGN_CENTER);
  display->fillRect(83,0,128,11);
  display->setFont(ArialMT_Plain_10);
 
  if (sat_pos_oled[0] == 0 && sat_pos_oled[1] == 0) {
    display->drawString( 65+x,  49+y+(x/2), "Waiting for FossaSat Pos" );
    display->drawString( 63+x,  51+y+(x/2), "Waiting for FossaSat Pos" );
    display->setColor(WHITE);
    display->drawString( 64+x,  50+y+(x/2), "Waiting for FossaSat Pos" );
  }
  else {
       if ((millis()-tick_interval)>tick_timing) {
              // Change the value to plot
                  graphVal+=delta;
                  tick_interval=millis();
              // If the value reaches a limit, then change delta of value
                    if (graphVal >= 6)     {delta = -1;  tick_timing=50; }// ramp down value
                    else if (graphVal <= 1) {delta = +1; tick_timing=100;} // ramp up value
       }
    display->fillCircle(sat_pos_oled[0]+x, sat_pos_oled[1]+y, graphVal+1);
    display->setColor(WHITE);
    display->drawCircle(sat_pos_oled[0]+x, sat_pos_oled[1]+y, graphVal);
    display->setColor(BLACK);
    display->drawCircle(sat_pos_oled[0]+x, sat_pos_oled[1]+y, (graphVal/3)+1);
    display->setColor(WHITE);
    display->drawCircle(sat_pos_oled[0]+x, sat_pos_oled[1]+y, graphVal/3);
  }
  
}

// This array keeps function pointers to all frames
// frames are the single views that slide in
FrameCallback frames[] = { drawFrame1, drawFrame2, drawFrame3, drawFrame4, drawFrame5, drawFrame6 };

// how many frames are there?
int frameCount = 6;

// Overlays are statically drawn on top of a frame eg. a clock
OverlayCallback overlays[] = { msOverlay };
int overlaysCount = 1;


void fossaAPStarted (AsyncWiFiManager* wm) {
	String ssid;
	if (wm) {
		ssid = wm->getConfigPortalSSID ();
	}
	ESP_LOGI (LOG_TAG, "AP started. Connect to %s", ssid.c_str ());

  display.clear();
  display.setFont(ArialMT_Plain_10);
  display.setTextAlignment(TEXT_ALIGN_LEFT);
  display.drawString(0, 6,"Connect to AP:");
  display.drawString(0,18,"->"+String(ssid));
  display.drawString(5,32,"to configure your Station");
  display.drawString(10,52,"IP:   192.168.4.1");
  display.display();


	// TODO: Show message to user "Connect to <SSID> to configure board"
}

void configSaved (bool result) {
	ESP_LOGI (LOG_TAG, "Config %ssaved", result? "": "not ");

	// TODO: Show result to user
}

void flashFormatting () {
	ESP_LOGI (LOG_TAG, "Formatting flash");

  display.clear();
  display.setFont(ArialMT_Plain_16);
  display.setTextAlignment(TEXT_ALIGN_LEFT);
  display.drawString(0,5,"FossaSAT-1 Sta");
  display.setFont(ArialMT_Plain_10);
  display.drawString(55,23,"ver. "+String(fs_version));
  display.drawString(2,38,"Formatting Flash ");
  display.drawString(2,52,"Please Wait Don't turn OFF");
  display.display();

	// Done TODO: Inform user that flash is being formatted


}

void setup() {
  pinMode(OLED_RST,OUTPUT);
  digitalWrite(OLED_RST, LOW);      //    Reset the OLED    
  delay(50);
  digitalWrite(OLED_RST, HIGH);

  display.init();
  display.flipScreenVertically();
  display.setFont(ArialMT_Plain_16);
  display.setTextAlignment(TEXT_ALIGN_LEFT);
  display.drawString(0,5,"FossaSAT-1 Sta");
  display.setFont(ArialMT_Plain_10);
  display.drawString(55,23,"ver. "+String(fs_version));
  
  display.drawString(5,38,"by @gmag12 @4m1g0");
  display.drawString(40,52,"& @g4lile0");
  display.display();

  delay (2000);

  Serial.begin (115200);
  Serial.printf("Fossa Ground station Version to %d\n", fs_version);

  display.clear();
  display.drawXbm(34, 0 , WiFi_Logo_width, WiFi_Logo_height, WiFi_Logo_bits);
  display.setTextAlignment(TEXT_ALIGN_CENTER);
  display.drawString(64 , 35 , "Connecting "+String(board_config.ssid));
  display.display();

  delay (500);  
  
  //connect to WiFi
  config_manager.setAPStartedCallback (fossaAPStarted);
  config_manager.setConfigSavedCallback (configSaved);
  config_manager.setFormatFlashCallback (flashFormatting);
  
  // Check WiFi reset button
  unsigned long start_waiting_for_button = millis ();
  unsigned long button_pushed_at;
  pinMode (PROG_BUTTON, INPUT_PULLUP);
  ESP_LOGI (LOG_TAG, "Waiting for reset config button");

  bool button_pushed = false;
  while (millis () - start_waiting_for_button < WAIT_FOR_BUTTON) {

	  if (!digitalRead (PROG_BUTTON)) {
		  button_pushed = true;
		  button_pushed_at = millis ();
		  ESP_LOGI (LOG_TAG, "Reset button pushed");
		  while (millis () - button_pushed_at < RESET_BUTTON_TIME) {
			  if (digitalRead (PROG_BUTTON)) {
				  ESP_LOGI (LOG_TAG, "Reset button released");
				  button_pushed = false;
				  break;
			  }
		  }
		  if (button_pushed) {
			  ESP_LOGI (LOG_TAG, "Reset config triggered");
			  WiFi.begin ("0", "0");
			  WiFi.disconnect ();
		  }
	  }
  }

  ESP_LOGI (LOG_TAG, "Connecting to WiFi %s", board_config.ssid);
  if (config_manager.begin (button_pushed)) {
	  // Connected
  }

  arduino_ota_setup ();

  //WiFi.begin(ssid, password);
  //uint8_t waiting = 0;
  //while (WiFi.status() != WL_CONNECTED) {

  //    display.drawProgressBar(5,53,120,10,waiting ); 
  //    waiting += 10 ;
  //    if (waiting > 90) {
  //      waiting=0;
  //      display.setColor(BLACK);
  //    // display.drawProgressBar(5,53,120,10,100);   seems that doesn't seem to follow setColor
  //      display.fillRect(5, 53, 123, 12);
  //      display.setColor(WHITE);
  //      
  //    }
  //    display.display();
  //    delay(500);
  //    Serial.print(".");

  //}

  display.clear();
  display.drawXbm(34, 0 , WiFi_Logo_width, WiFi_Logo_height, WiFi_Logo_bits);
  
  Serial.println(" CONNECTED");
  display.drawString(64 , 35 , "Connected "+String(board_config.ssid));
  display.drawString(64 ,53 , (WiFi.localIP().toString()));
  display.display();
  delay (1000);  

  mqtt.init (board_config.mqtt_server_name, board_config.mqtt_port, board_config.mqtt_user, board_config.mqtt_pass);

  String topic = "fossa/" + String(board_config.mqtt_user) + "/" + String(board_config.station) + "/status";
   
  mqtt.setLastWill(topic.c_str());
  mqtt.onEvent (manageMQTTEvent);
  mqtt.onReceive (manageMQTTData);
  mqtt.begin();
  
  //init and get the time
  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
  if (strcmp (board_config.tz, "")) {
	  setenv ("TZ", board_config.tz, 1);
	  ESP_LOGD (LOG_TAG, "Set timezone value as %s", board_config.tz);
	  tzset ();
  }

  printLocalTime();

  // initialize SX1278 with default settings
  Serial.print(F("[SX12x8] Initializing ... "));
  // carrier frequency:           436.7 MHz
  // bandwidth:                   125.0 kHz
  // spreading factor:            11
  // coding rate:                 8
  // sync word:                   0xff  for SX1278 and   0x0F0F for SX1268
  // output power:                17 dBm
  // current limit:               100 mA
  // preamble length:             8 symbols
  // amplifier gain:              0 (automatic gain control)
  //  int state = lora.begin();
 
 #ifdef RADIO_SX126X
  int state = lora.begin(LORA_CARRIER_FREQUENCY,
                          LORA_BANDWIDTH,
                          LORA_SPREADING_FACTOR,
                          LORA_CODING_RATE,
                          SYNC_WORD_6X,
                          17,
                          (uint8_t)LORA_CURRENT_LIMIT);
  #else
  int state = lora.begin(LORA_CARRIER_FREQUENCY,
                          LORA_BANDWIDTH,
                          LORA_SPREADING_FACTOR,
                          LORA_CODING_RATE,
                          SYNC_WORD_7X,
                          17,
                          (uint8_t)LORA_CURRENT_LIMIT);
  #endif

                            Serial.println(LORA_CARRIER_FREQUENCY);
                            Serial.println(LORA_SPREADING_FACTOR);
                            Serial.println(LORA_CODING_RATE);




  if (state == ERR_NONE) {
    Serial.println(F("success!"));
  } else {
    Serial.print(F("failed, code "));
    Serial.println(state);
    while (true);
  }

  // set the function that will be called
  // when new packet is received
  // attach the ISR to radio interrupt
  #ifdef RADIO_SX126X
  lora.setDio1Action(setFlag);
  #else
  lora.setDio0Action(setFlag);
  #endif

  // start listening for LoRa packets
  Serial.print(F("[SX12x8] Starting to listen ... "));
  state = lora.startReceive();
  if (state == ERR_NONE) {
    Serial.println(F("success!"));
  } else {
    Serial.print(F("failed, code "));
    Serial.println(state);
    while (true);
  }

  // if needed, 'listen' mode can be disabled by calling
  // any of the following methods:
  //
  // lora.standby()
  // lora.sleep()
  // lora.transmit();
  // lora.receive();
  // lora.readData();
  // lora.scanChannel();

  // TODO: Make this beautiful
  display.clear();
  display.setTextAlignment(TEXT_ALIGN_LEFT);
  display.drawString(20 ,10 , "Waiting for MQTT"); 
  display.drawString(35 ,24 , "Connection...");
  display.drawString(3 ,38 , "Press PROG butt. or send");
  display.drawString(3 ,52 , "e in serial to reset conf.");
  display.display();
  Serial.println ("Waiting for MQTT connection. Send 'e' or press the PROG button to erase board config and return to the config manager AP");
  int i = 0;
  while (!mqtt_connected) {
    if (i++ > 150000) {// 5m
      Serial.println (" MQTT unable to connect after 5m, restarting...");
      ESP.restart();
    }
	  Serial.print ('.');
	  delay (500);
    if (Serial.available()){
      char byte = Serial.read();
      if (byte == 'e') {
        config_manager.eraseConfig();
        ESP.restart();
      }
    }
    if (!digitalRead (PROG_BUTTON)){
      config_manager.eraseConfig();
      ESP.restart();
    }
  }
  Serial.println (" Connected !!!");
  
  printControls();

  // The ESP is capable of rendering 60fps in 80Mhz mode
  // but that won't give you much time for anything else
  // run it in 160Mhz mode or just set it to 30 fps
  ui.setTargetFPS(60);

  // Customize the active and inactive symbol
  ui.setActiveSymbol(activeSymbol);
  ui.setInactiveSymbol(inactiveSymbol);

  // You can change this to
  // TOP, LEFT, BOTTOM, RIGHT
  ui.setIndicatorPosition(BOTTOM);

  // Defines where the first frame is located in the bar.
  ui.setIndicatorDirection(LEFT_RIGHT);

  // You can change the transition that is used
  // SLIDE_LEFT, SLIDE_RIGHT, SLIDE_UP, SLIDE_DOWN
  ui.setFrameAnimation(SLIDE_LEFT);

  // Add frames
  ui.setFrames(frames, frameCount);

  // Add overlays
  ui.setOverlays(overlays, overlaysCount);

  // Initialising the UI will init the display too.
  ui.init();

  display.flipScreenVertically();

}

void loop() {
  int remainingTimeBudget = ui.update();

    if (remainingTimeBudget > 0) {
    // You can do some work here
    // Don't do stuff if you are below your
    // time budget.
    delay(remainingTimeBudget);
  }




/// fossa station code
  // check serial data
  if(Serial.available()) {

    // disable reception interrupt
    enableInterrupt = false;
   // detachInterrupt(digitalPinToInterrupt(DIO1));

    // get the first character
    char serialCmd = Serial.read();

    // wait for a bit to receive any trailing characters
    delay(50);

    // dump the serial buffer
    while(Serial.available()) {
      Serial.read();
    }

    // process serial command
    switch(serialCmd) {
      case 'p':
        sendPing();
        break;
      case 'i':
        requestInfo();
        break;
      case 'l':
        requestPacketInfo();
        break;
      case 'r':
        requestRetransmit();
        break;
      case 'e':
        config_manager.eraseConfig();
        ESP.restart();
        break;
      case 't':
        switchTestmode();
        ESP.restart();
        break;
      case 'b':
        ESP.restart();
        break;
       
      default:
        Serial.print(F("Unknown command: "));
        Serial.println(serialCmd);
        break;
    }

    // set radio mode to reception
    #ifdef RADIO_SX126X
    lora.setDio1Action(setFlag);
    #else
    lora.setDio0Action(setFlag);
    #endif

    lora.startReceive();
    enableInterrupt = true;
  }
//fossa station code

  // check if the flag is set (received interruption)
  if(receivedFlag) {
    // disable the interrupt service routine while
    // processing the data
    enableInterrupt = false;

    // reset flag
    receivedFlag = false;

    // read received data
    size_t respLen = lora.getPacketLength();
    uint8_t* respFrame = new uint8_t[respLen];
    int state = lora.readData(respFrame, respLen);

    // get function ID
    uint8_t functionId = FCP_Get_FunctionID(callsign, respFrame, respLen);
    Serial.print(F("Function ID: 0x"));
    Serial.println(functionId, HEX);

    // check optional data
    uint8_t* respOptData = nullptr;
    uint8_t respOptDataLen = FCP_Get_OptData_Length(callsign, respFrame, respLen);
    Serial.print(F("Optional data ("));
    Serial.print(respOptDataLen);
    Serial.println(F(" bytes):"));
    if(respOptDataLen > 0) {
      // read optional data
      respOptData = new uint8_t[respOptDataLen];
      FCP_Get_OptData(callsign, respFrame, respLen, respOptData);
      PRINT_BUFF(respFrame, respLen);
      
    }

    struct tm timeinfo;
    if(!getLocalTime(&timeinfo)){
      Serial.println("Failed to obtain time");
      return;
    }

    // store time of the last packet received:
    String thisTime="";
    if (timeinfo.tm_hour < 10){ thisTime=thisTime + " ";} // add leading space if required
    thisTime=String(timeinfo.tm_hour) + ":";
    if (timeinfo.tm_min < 10){ thisTime=thisTime + "0";} // add leading zero if required
    thisTime=thisTime + String(timeinfo.tm_min) + ":";
    if (timeinfo.tm_sec < 10){ thisTime=thisTime + "0";} // add leading zero if required
    thisTime=thisTime + String(timeinfo.tm_sec);
    // const char* newTime = (const char*) thisTime.c_str();
    
    last_packet_received_time=  thisTime;
    last_packet_received_rssi= lora.getRSSI();
    last_packet_received_snr=lora.getSNR();
    last_packet_received_frequencyerror=lora.getFrequencyError();

    // print RSSI (Received Signal Strength Indicator)
    Serial.print(F("[SX12x8] RSSI:\t\t"));
    Serial.print(lora.getRSSI());
    Serial.println(F(" dBm"));

    // print SNR (Signal-to-Noise Ratio)
    Serial.print(F("[SX12x8] SNR:\t\t"));
    Serial.print(lora.getSNR());
    Serial.println(F(" dB"));

    // print frequency error
    Serial.print(F("[SX12x8] Frequency error:\t"));
    Serial.print(lora.getFrequencyError());
    Serial.println(F(" Hz"));

      // process received frame
    switch(functionId) {
      case RESP_PONG:
        Serial.println(F("Pong!"));
        json_pong();
        break;

      case RESP_SYSTEM_INFO:
        
        Serial.println(F("System info:"));

        Serial.print(F("batteryChargingVoltage = "));
        batteryChargingVoltage = FCP_Get_Battery_Charging_Voltage(respOptData);
        Serial.println(FCP_Get_Battery_Charging_Voltage(respOptData));
        
        Serial.print(F("batteryChargingCurrent = "));
        batteryChargingCurrent = (FCP_Get_Battery_Charging_Current(respOptData), 4);
        Serial.println(FCP_Get_Battery_Charging_Current(respOptData), 4);

        Serial.print(F("batteryVoltage = "));
        batteryVoltage=FCP_Get_Battery_Voltage(respOptData);
        Serial.println(FCP_Get_Battery_Voltage(respOptData));          

        Serial.print(F("solarCellAVoltage = "));
        solarCellAVoltage= FCP_Get_Solar_Cell_Voltage(0, respOptData);
        Serial.println(FCP_Get_Solar_Cell_Voltage(0, respOptData));

        Serial.print(F("solarCellBVoltage = "));
        solarCellBVoltage= FCP_Get_Solar_Cell_Voltage(1, respOptData);
        Serial.println(FCP_Get_Solar_Cell_Voltage(1, respOptData));

        Serial.print(F("solarCellCVoltage = "));
        solarCellCVoltage= FCP_Get_Solar_Cell_Voltage(2, respOptData);
        Serial.println(FCP_Get_Solar_Cell_Voltage(2, respOptData));

        Serial.print(F("batteryTemperature = "));
        batteryTemperature=FCP_Get_Battery_Temperature(respOptData);
        Serial.println(FCP_Get_Battery_Temperature(respOptData));

        Serial.print(F("boardTemperature = "));
        boardTemperature=FCP_Get_Board_Temperature(respOptData);
        Serial.println(FCP_Get_Board_Temperature(respOptData));

        Serial.print(F("mcuTemperature = "));
        mcuTemperature =FCP_Get_MCU_Temperature(respOptData);
        Serial.println(FCP_Get_MCU_Temperature(respOptData));

        Serial.print(F("resetCounter = "));
        resetCounter=FCP_Get_Reset_Counter(respOptData);
        Serial.println(FCP_Get_Reset_Counter(respOptData));

        Serial.print(F("powerConfig = 0b"));
        powerConfig=FCP_Get_Power_Configuration(respOptData);
        Serial.println(FCP_Get_Power_Configuration(respOptData), BIN);
        json_system_info();
        break;

      case RESP_LAST_PACKET_INFO:
        Serial.println(F("Last packet info:"));

        Serial.print(F("SNR = "));
        Serial.print(respOptData[0] / 4.0);
        Serial.println(F(" dB"));

        Serial.print(F("RSSI = "));
        Serial.print(respOptData[1] / -2.0);
        Serial.println(F(" dBm"));
        break;

      case RESP_REPEATED_MESSAGE:
        Serial.println(F("Got repeated message:"));
        Serial.println((char*)respOptData);
        json_message((char*)respOptData,respLen);
        break;

      default:
        Serial.println(F("Unknown function ID!"));
        break;
    }

    if (state == ERR_NONE) {
      // packet was successfully received
  //     Serial.println(F("[SX12x8] Received packet!"));

      // print data of the packet
 //     Serial.print(F("[SX12x8] Data:\t\t"));
 //      Serial.println(str);
    } else if (state == ERR_CRC_MISMATCH) {
      // packet was received, but is malformed
      Serial.println(F("[SX12x8] CRC error!"));
    } else {
      // some other error occurred
      Serial.print(F("[SX12x8] Failed, code "));
      Serial.println(state);
    }

    // put module back to listen mode
    lora.startReceive();

    // we're ready to receive more packets,
    // enable interrupt service routine
    enableInterrupt = true;
  }
  
  static unsigned long last_connection_fail = millis();
  if (!mqtt_connected){
    if (millis() - last_connection_fail > 300000){ // 5m
      Serial.println("MQTT Disconnected, restarting...");
      ESP.restart();
    }
  } else {
    last_connection_fail = millis();
  }


  ArduinoOTA.handle ();
}


void  welcome_message (void) {
        const size_t capacity = JSON_ARRAY_SIZE(2) + JSON_OBJECT_SIZE(16);
          DynamicJsonDocument doc(capacity);
          doc["station"] = board_config.station;  // G4lile0
          JsonArray station_location = doc.createNestedArray("station_location");
          station_location.add(board_config.latitude);
          station_location.add(board_config.longitude);
          doc["version"] = fs_version;

//          doc["time"] = ;
          serializeJson(doc, Serial);
          String topic = "fossa/" + String(board_config.mqtt_user) + "/" + String(board_config.station) + "/welcome";
          char buffer[512];
          size_t n = serializeJson(doc, buffer);
          mqtt.publish(topic.c_str(), buffer,n );
          ESP_LOGI (LOG_TAG, "Wellcome sent");
}

void  json_system_info(void) {
          //// JSON
          
          time_t now;
          time(&now);
          const size_t capacity = JSON_ARRAY_SIZE(2) + JSON_OBJECT_SIZE(18);
          DynamicJsonDocument doc(capacity);
          doc["station"] = board_config.station;  // G4lile0
          JsonArray station_location = doc.createNestedArray("station_location");
          station_location.add(board_config.latitude);
          station_location.add(board_config.longitude);
          doc["rssi"] = last_packet_received_rssi;
          doc["snr"] = last_packet_received_snr;
          doc["frequency_error"] = last_packet_received_frequencyerror;
          doc["unix_GS_time"] = now;
          doc["batteryChargingVoltage"] = batteryChargingVoltage;
          doc["batteryChargingCurrent"] = batteryChargingCurrent;
          doc["batteryVoltage"] = batteryVoltage;
          doc["solarCellAVoltage"] = solarCellAVoltage;
          doc["solarCellBVoltage"] = solarCellBVoltage;
          doc["solarCellCVoltage"] = solarCellCVoltage;
          doc["batteryTemperature"] = batteryTemperature;
          doc["boardTemperature"] = boardTemperature;
          doc["mcuTemperature"] = mcuTemperature;
          doc["resetCounter"] = resetCounter;
          doc["powerConfig"] = powerConfig;
          serializeJson(doc, Serial);
          String topic = "fossa/" + String(board_config.mqtt_user) + "/" + String(board_config.station) + "/sys_info";
          char buffer[512];
          serializeJson(doc, buffer);
          size_t n = serializeJson(doc, buffer);
          mqtt.publish(topic.c_str(), buffer,n );

/*
 * 
 * 
 * https://arduinojson.org/v6/assistant/
 * {
"station":"g4lile0",
"station_location":[48.756080,2.302038],
"rssi":-34,
"snr": 9.50,
"frequency_error": 6406.27,
"batteryChargingVoltage": 3.46,
"batteryChargingCurrent":  -0.0110,
"batteryVoltage" : 1.96,
"solarCellAVoltage" : 0.92,
"solarCellBVoltage" : 0.96,
"solarCellCVoltage" : 0.96,
"batteryTemperature" : -26.71,
"boardTemperature" : 8.44,
"mcuTemperature" : 3,
"resetCounter" : 0,
"powerConfig" : 255

}
 */
 
}


void  json_message(char* frame, size_t respLen) {
          time_t now;
          time(&now);
          Serial.println(String(respLen));
          char tmp[respLen+1];
          memcpy(tmp, frame, respLen);
          tmp[respLen-12] = '\0';


              // if special miniTTN message   
          Serial.println(String(frame[0]));
          Serial.println(String(frame[1]));
          Serial.println(String(frame[2]));
//          if ((frame[0]=='0x54') &&  (frame[1]=='0x30') && (frame[2]=='0x40'))
          if ((frame[0]=='T') &&  (frame[1]=='0') && (frame[2]=='@'))
          {
          Serial.println("mensaje miniTTN");
          const size_t capacity = JSON_ARRAY_SIZE(2) + JSON_OBJECT_SIZE(11) +JSON_ARRAY_SIZE(respLen-12);
          DynamicJsonDocument doc(capacity);
          doc["station"] = board_config.station;  // G4lile0
          JsonArray station_location = doc.createNestedArray("station_location");
          station_location.add(board_config.latitude);
          station_location.add(board_config.longitude);
          doc["rssi"] = last_packet_received_rssi;
          doc["snr"] = last_packet_received_snr;
          doc["frequency_error"] = last_packet_received_frequencyerror;
          doc["unix_GS_time"] = now;
          JsonArray msgTTN = doc.createNestedArray("msgTTN");

          
          for (byte i=0 ; i<  (respLen-12);i++) {

                msgTTN.add(String(tmp[i], HEX));

            }
          

          
//          doc["len"] = respLen;
//          doc["msg"] = String(tmp);
//          doc["msg"] = String(frame);
 
          
          serializeJson(doc, Serial);
          String topic = "fossa/" + String(board_config.mqtt_user) + "/" + String(board_config.station) + "/miniTTN";
        
          char buffer[256];
          serializeJson(doc, buffer);
          size_t n = serializeJson(doc, buffer);
          mqtt.publish(topic.c_str(), buffer,n );

            
            }

            else

            {
              
          
          const size_t capacity = JSON_ARRAY_SIZE(2) + JSON_OBJECT_SIZE(11);
          DynamicJsonDocument doc(capacity);
          doc["station"] = board_config.station;  // G4lile0
          JsonArray station_location = doc.createNestedArray("station_location");
          station_location.add(board_config.latitude);
          station_location.add(board_config.longitude);
          doc["rssi"] = last_packet_received_rssi;
          doc["snr"] = last_packet_received_snr;
          doc["frequency_error"] = last_packet_received_frequencyerror;
          doc["unix_GS_time"] = now;
//          doc["len"] = respLen;
          doc["msg"] = String(tmp);
//          doc["msg"] = String(frame);
 
          
          serializeJson(doc, Serial);
          String topic = "fossa/" + String(board_config.mqtt_user) + "/" + String(board_config.station) + "/msg";
          
          char buffer[256];
          serializeJson(doc, buffer);
          size_t n = serializeJson(doc, buffer);
          mqtt.publish(topic.c_str(), buffer,n );
              
              }
            
          


          
          
          
         
}



void  json_pong(void) {
          //// JSON
          time_t now;
          time(&now);
          const size_t capacity = JSON_ARRAY_SIZE(2) + JSON_OBJECT_SIZE(6);
          DynamicJsonDocument doc(capacity);
          doc["station"] = board_config.station;  // G4lile0
          JsonArray station_location = doc.createNestedArray("station_location");
          station_location.add(board_config.latitude);
          station_location.add(board_config.longitude);
          doc["rssi"] = last_packet_received_rssi;
          doc["snr"] = last_packet_received_snr;
          doc["frequency_error"] = last_packet_received_frequencyerror;
          doc["unix_GS_time"] = now;
          doc["pong"] = 1;
          serializeJson(doc, Serial);
          String topic = "fossa/" + String(board_config.mqtt_user) + "/" + String(board_config.station) + "/pong";
          char buffer[256];
          serializeJson(doc, buffer);
          size_t n = serializeJson(doc, buffer);
          mqtt.publish(topic.c_str(), buffer,n );
}


// function to print controls
void printControls() {
  Serial.println(F("------------- Controls -------------"));
  Serial.println(F("p - send ping frame"));
  Serial.println(F("i - request satellite info"));
  Serial.println(F("l - request last packet info"));
  Serial.println(F("r - send message to be retransmitted"));
  Serial.println(F("t - change the test mode and restart"));
  Serial.println(F("e - erase board config and reset"));
  Serial.println(F("b - reboot the board"));
  Serial.println(F("------------------------------------"));
}



void sendPing() {
  Serial.print(F("Sending ping frame ... "));

  // data to transmit
  uint8_t functionId = CMD_PING;

  // build frame
  uint8_t len = FCP_Get_Frame_Length(callsign);
  uint8_t* frame = new uint8_t[len];
  FCP_Encode(frame, callsign, functionId);

  // send data
  int state = lora.transmit(frame, len);
  delete[] frame;

  // check transmission success
  if (state == ERR_NONE) {
    Serial.println(F("sent successfully!"));
  } else {
    Serial.print(F("failed, code "));
    Serial.println(state);
  }
}


void  switchTestmode() {
  char temp_station[32];
  if ((board_config.station[0]=='t') &&  (board_config.station[1]=='e') && (board_config.station[2]=='s') && (board_config.station[4]=='_')) {
    Serial.println(F("Changed from test mode to normal mode"));
    for (byte a=5; a<=strlen(board_config.station); a++ ) {
      board_config.station[a-5]=board_config.station[a];
    }
  }
  else
  {
    strcpy(temp_station,"test_");
    strcat(temp_station,board_config.station);
    strcpy(board_config.station,temp_station);
    Serial.println(F("Changed from normal mode to test mode"));
  }

  config_manager.saveFlashData();
}



void requestInfo() {
  Serial.print(F("Requesting system info ... "));

  // data to transmit
  uint8_t functionId = CMD_TRANSMIT_SYSTEM_INFO;

  // build frame
  uint8_t len = FCP_Get_Frame_Length(callsign);
  uint8_t* frame = new uint8_t[len];
  FCP_Encode(frame, callsign, functionId);

  // send data
  int state = lora.transmit(frame, len);
  delete[] frame;

  // check transmission success
  if (state == ERR_NONE) {
    Serial.println(F("sent successfully!"));
  } else {
    Serial.print(F("failed, code "));
    Serial.println(state);
  }
}

void requestPacketInfo() {
  Serial.print(F("Requesting last packet info ... "));

  // data to transmit
  uint8_t functionId = CMD_GET_LAST_PACKET_INFO;

  // build frame
  uint8_t len = FCP_Get_Frame_Length(callsign);
  uint8_t* frame = new uint8_t[len];
  FCP_Encode(frame, callsign, functionId);

  // send data
  int state = lora.transmit(frame, len);
  delete[] frame;

  // check transmission success
  if (state == ERR_NONE) {
    Serial.println(F("sent successfully!"));
  } else {
    Serial.print(F("failed, code "));
    Serial.println(state);
  }
}

void requestRetransmit() {
  Serial.println(F("Enter message to be sent:"));
  Serial.println(F("(max 30 characters, end with LF or CR+LF)"));

  // get data to be retransmited
  char optData[32];
  uint8_t bufferPos = 0;
  while(bufferPos < 31) {
    while(!Serial.available());
    char c = Serial.read();
    Serial.print(c);
    if((c != '\r') && (c != '\n')) {
      optData[bufferPos] = c;
      bufferPos++;
    } else {
      break;
    }
  }

  // wait for a bit to receive any trailing characters
  delay(100);

  // dump the serial buffer
  while(Serial.available()) {
    Serial.read();
  }

  Serial.println();
  Serial.print(F("Requesting retransmission ... "));

  // data to transmit
  uint8_t functionId = CMD_RETRANSMIT;
  optData[bufferPos] = '\0';
  uint8_t optDataLen = strlen(optData);

  // build frame
  uint8_t len = FCP_Get_Frame_Length(callsign, optDataLen);
  uint8_t* frame = new uint8_t[len];
  FCP_Encode(frame, callsign, functionId, optDataLen, (uint8_t*)optData);

  // send data
  int state = lora.transmit(frame, len);
  delete[] frame;

  // check transmission success
  if (state == ERR_NONE) {
    Serial.println(F("sent successfully!"));
  } else {
    Serial.print(F("failed, code "));
    Serial.println(state);
  }
}
