/*********
  Cellular-Solar (CellSol) is a simple interconnect between lora, wifi and com port(s). It is intended to be used for infrastructure-independent comms during or after a disaster.
  (c) 2020 Robots Everywhere, LLC until we are ready to release it under copyleft
  Written by Riley August (HTML/CSS/DHCP/Optimizations), and M K Borri (other). Thanks to Rui Santos for the tutorials. Thanks to Jerry Jenkins for the inspiration. Thanks to Lisa Rein for initiating the project.  Thanks to Robots Everywhere for infrastructure support https://www.robots-everywhere.com
  Originally produced as part of the Aaron Swartz Day project https://www.aaronswartzday.org
  Distributed independently https://www.f3.to/cellsol

  This file is used for esp32-based boards (Lora32, TTGO T-Beam, etc.)
*********/

// Firmware version.
#define VERSIONSTRING "0.32"

#include "config.h" // Please make config changes in config.h and pinout.h instead of here!

#ifdef PROVIDE_APK
#include "btt/btt.h"
#endif

#ifdef PROVIDE_CAT_PICTURE
#include "btt/cat.h"
#endif

#ifdef PROVIDE_SOURCE_CODE
#include "btt/src.h"
#endif


#define uS_TO_S_FACTOR 1000000  /* Conversion factor for micro seconds to seconds. Do not change this. Seriously. */

// Load Wi-Fi library
#include <WiFi.h>
#include "time.h"
// taken from genericwifi.h -1dbm to 19.5dbm
#ifdef WIFI_POWER_LEVEL
const wifi_power_t wifilevels[] = {WIFI_POWER_MINUS_1dBm, WIFI_POWER_2dBm, WIFI_POWER_5dBm, WIFI_POWER_7dBm, WIFI_POWER_8_5dBm, WIFI_POWER_11dBm, WIFI_POWER_13dBm, WIFI_POWER_15dBm, WIFI_POWER_17dBm, WIFI_POWER_18_5dBm, WIFI_POWER_19dBm, WIFI_POWER_19_5dBm}; // 0~11
#endif

#include <DNSServer.h>
const byte DNS_PORT = 53;
#include <esp_wifi.h>
#include "BluetoothSerial.h"

//Libraries for LoRa
#include <SPI.h>
#include <LoRa.h>
#include <IRCClient.h>

// watchdog
#include <esp_task_wdt.h>
#define WDT_TIMEOUT 4 // in seconds

#include "esp32-hal-cpu.h"
#define CLKFREQ_HI 240 // megahertz when going as fast as possible
#define CLKFREQ 80 // megahertz when in full mode
#define CLKFREQ_LOW 20 // megahertz when in repeater mode; can't go any lower, annoyingly

#include "pinout.h" // LORA32 Pinout by default, change this file to change the pinout!


#ifdef MINDISPLAY
#define NODISPLAY
#include <Wire.h>
//#include "ACROBOTIC_SSD1306_minimal/ACROBOTIC_SSD1306_minimal.h" // pared down SS1306 driver that only has the 5x7 font
#include "ACROBOTIC_SSD1306_minimal/ACROBOTIC_SSD1306_minimal.cpp" // pared down SS1306 driver that only has the 5x7 font
#endif

#ifndef NODISPLAY
//Libraries for OLED Display
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#endif

// LORA32 setup variables
static bool lowpowerstart = false;
static uint64_t chipid; // chip id stuff = ESP.getEfuseMac();
static uint16_t uptwo; // chip id stuff = (uint16_t)(chipid >> 32);
static uint32_t dnfor; // chip id stuff  = (uint32_t)(chipid);


// Web constants for text align, used for website display
#define TEXT_ALIGN_STRING_A "center"
#define TEXT_ALIGN_STRING_B "justify"
String TEXT_ALIGN_STRING = TEXT_ALIGN_STRING_A;
static RTC_NOINIT_ATTR bool centertext = true;// if this, center, otherwise, justify


// Wireless UART Configuration
long LastReconnect = 0;
int currbatterylevel = 0;

#define UART_BAUD_RATE 9600
String ssid = SSIDROOT;
String tempstring = ""; // internal function use only

bool wifimode = true; // bluetooth or wifi

// serial on/off
boolean enablecomport = false; // set to false for a solderless fix for UART buffer crosstalk hardware issue

// Set web server port number to 80
WiFiServer server(80);
DNSServer dnsServer;
WiFiClient wificlient1;

// WEB SERVICE CONFIGURATION - What files do we want to serve?
#ifdef PROVIDE_APK
extern const long int btt_apk_size;
extern const unsigned char btt_apk[];
#endif
#ifdef PROVIDE_CAT_PICTURE
extern const long int cat_jpg_size;
extern const unsigned char cat_jpg[];
#endif
#ifdef PROVIDE_SOURCE_CODE
extern const long int src_zip_size;
extern const unsigned char src_zip[];
#endif

// BATTERY POWER CONFIGURATION
static RTC_NOINIT_ATTR int lastbatterylevel = 0; // try to save battery levels between runs
int batt_delta;
int battimeout = 0;
bool low_batt_announce = false;

// SERIAL and IP CONFIGURATION
int lploops = LPLOOP_BLINK - 10; // give me a blink early so i know we're alive
bool has_lora_been_initialized = false;
bool has_serial_been_initialized = false;
bool has_bluetooth_been_initialized = false;
bool has_wifi_been_initialized = false;
bool has_display_been_initialized = false;

// USER TAG CONFIGURATION
String hextag  = "XXXX"; // usually the last two IP octets; gives pseudonimity to sender
String lasttagimade = "XXXX"; // last one we made for use in checking

// Temp bytes for IP addresses
byte derpme = 0; // third number of ip address
byte spare_id_nibble = 0; // upper nibble; lower nibble always 0


//packet counter
int txcounter = 0;
int rxcounter = 0;
byte charcounter = 0;
boolean readytosend = false;
byte charcounte2 = 0;
boolean readytosen2 = false;

// display flags
boolean dodisplay = false;
boolean dodisplaybuf = false;
boolean displayexists = false;
bool displayenabled = true;

#ifdef REPEAT_UNTIL_ACK  
boolean got_lora_ack = false;
long last_repeat_time = millis();
String message_to_repeat = "";
#endif


#ifndef NODISPLAY
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RST);
#endif


long disptimeout = 0;
bool display_on_ping = false;


/**********************************************
   Watchdog block; creates and manages a task watchdog for esp32 to prevent unwanted power off
 **********************************************/
unsigned long pseudoseconds = 0;
unsigned long psm = 0;
long UTC_Seconds = 0;
void PetTheWatchdog() {
  if (millis() > psm)
  {
    esp_task_wdt_reset();
    // apparently the esp32 is smart enough to update tick size
    /*
      if (getCpuFrequencyMhz() < CLKFREQ)
        psm = psm+1000;
      else
        psm=psm+1000;
    */
    psm = psm + 999;
    pseudoseconds++;

    DoUTC(true);
  }
#ifdef GPS_SERIAL_1
  else
    DoUTC(false);
#endif
}

bool is_watchdog_on = false;
void Watchdog(bool onoff) // gets turned back on by the adc reading, assumtion is that we can survive for 5 seconds we hope
{
  if (onoff)
  {
    esp_task_wdt_init(WDT_TIMEOUT, true); //enable panic so ESP32 restarts
    esp_task_wdt_add(NULL); //add current thread to WDT watch
  }
  else
  {
    esp_task_wdt_delete(NULL);
    esp_task_wdt_deinit();
    esp_task_wdt_delete(NULL);
    esp_task_wdt_deinit();
  }
  is_watchdog_on = onoff;
  battimeout = millis() + ADC_INTERVAL; // wait to turn the watchdog back on please
}

/**
   END WATCHDOG BLOCK
*/


/*******************************************************************************
   TCP/IP Block
 *******************************************************************************/
IPAddress AP_IP(192, 168, 255, 1); // ip address of AP
IPAddress Client_IP(CLIENT_IP_ADDR);// this is the IP address we will be using.
IPAddress Client_Gateway(GATEWAY_IP_ADDR);// this is the IP address OF THE ROUTER
IPAddress Client_Subnet(GATEWAY_SUBNET);// usually 255,255,255,0 or 255,255,0,0
IPAddress lwc(255, 255, 255, 255); // ip address of current client
IPAddress IP(255, 255, 255, 255); // used to generate webpages
String ipstring;
String ipstring_c;
String ipstring_a;
String ipstring_b; // used in bluetooth mode
byte last_web_caller; // last web ip that said something
bool broadcast_twice = false;

// util method
String fourhex(int num1, int num2)
{
  int num = (num1 * 256) + num2;
  if (num < 0x10)
    return "000" + String(num, HEX);
  if (num < 0x100)
    return "00" + String(num, HEX);
  if (num < 0x1000)
    return "0" + String(num, HEX);
  if (num > 0xFFFF)
    return String(num % 65536, HEX);
  return String(num, HEX);
}

void BuildNicknameTags()
{
  // use the chip id to generate a (hopefully) unique identifier
  chipid = ESP.getEfuseMac();
  uptwo = (uint16_t)(chipid >> 32);
  dnfor = (uint32_t)(chipid);
  dnfor = uptwo + dnfor; // 6 byte unique-ish ID

#ifdef USE_BATTERY_NOISE_FOR_ID
  spare_id_nibble = ((currbatterylevel + dnfor) & 3840) >> 4; // only generated once. third nibble, fourth nibble is 0 since we are using it for something. so 00~F0. 3840 is F00
#else
  spare_id_nibble = (dnfor & 3840) >> 4;  // third nibble, fourth nibble is 0 since we are using it for something. so 00~F0. 3840 is F00
#endif


  derpme = dnfor & 255;
  if (derpme == 255) // allows using 192.168.x.1 for the AP which saves a lot of config headaches
    derpme = 0;
  AP_IP[2] = derpme;
  ipstring_a = AP_IP.toString();
  ipstring = ipstring_a;
  ssid = SSIDROOT + ipstring;
  TEXT_ALIGN_STRING = centertext ? TEXT_ALIGN_STRING_A : TEXT_ALIGN_STRING_B;
}
String status_string()
{
  return (fourhex(derpme, spare_id_nibble) + TAG_END_SYMBOL + "(" + String(currbatterylevel) + "/" + String(batt_delta) + ") " + (is_watchdog_on ? "`" : ",") + String(pseudoseconds) + (broadcast_twice ? "`" : ",") );
}
/*
   DUMMY METHODS FOR HTML ENCODE/DECODE - TODO: remove this comment as these methods got replaced?
  String decodeHtml(String text)
  {
  text.replace("&amp;", "&");
  text.replace("&#034;", "\"");
  text.replace("&#039;","'");
  text.replace("&lt;","<");
  text.replace("&gt;",">");
  return text;
  }
  String encodeHtml(String text)
  {
  text.replace("&", "&amp;");
  text.replace("\"", "&#034;");
  text.replace("'", "&#039;");
  text.replace("<", "&lt;");
  text.replace(">", "&gt;");
  return text;
  }
*/


/****************************************************************
   LoRa32 Radio block - LoRa, WiFi, and BT
*/


String LoRaData;

// Variable to store the HTTP request
String header;

#ifdef COMMUNITY_MEMORY_SIZE
String communitymemory[COMMUNITY_MEMORY_SIZE];
#endif
String string_rx[10];
long timestamp_rx[10];
String TimeAgoString(long secs) // keep it simple for display
{
  if (secs == pseudoseconds || secs < 0)
    return "?";
  if (secs < 60)
    return String(secs) + "s";
  if (secs < 3600)
    return String(secs / 60) + "m";
  if (secs < 86400)
    return String(secs / 3600) + "h";
  return String(secs / 86400) + "d";
}


long LastThingISentViaLora_3 = 0;
long LastThingISentViaLora_2 = 0;
long LastThingISentViaLora_1 = 0;
long LastThingISentViaLora_0 = 0;
long LTISVL_3_time = 0;
long LTISVL_2_time = 0;
long LTISVL_1_time = 0;
long LTISVL_0_time = 0;

String gotstring = "";


#ifdef USER_BUTTON_PIN
#else
#undef MODEFLIP_BUTTON
#endif

// if this exists, only show the display when we are pushing the button that way we know someone is actually looking at it (saves power)
#define user_button_display

#define MAXPKTSIZE  200 // lora packet is 255 bytes and we will need some for the header
#define MAXPKTSIZEP 201 // lora packet is 255 bytes and we will need some for the header
#define MAXPKTSIZEM 199 // lora packet is 255 bytes and we will need some for the header
char receivedChars[MAXPKTSIZE]; // an array to store the received data
char receivedChar2[MAXPKTSIZE]; // an array to store the received data
int rssi; // last packat received rssi

#ifdef BATT_ADC
static char RTC_NOINIT_ATTR byteme[10][MAXPKTSIZE]; // save last x sentences in here for posterity
#endif

void TryStoreSentences()
{
#ifdef BATT_ADC
  for (byte i = 0; i < 10; i++)
    string_rx[i].toCharArray(byteme[i], MAXPKTSIZE);
#endif
}
void TryRetrieveSentences()
{
#ifdef BATT_ADC
  for (byte i = 0; i < 10; i++)
    string_rx[i] = String(byteme[i]);
#endif
}

BluetoothSerial ESP_BT;

void decode_in_place(char *s) {
  char *d = s;

  while (*s) {
    switch (*s) {
      case '+': *d++ = ' '; s++; break; // turn + into space
      case 13: *d++ = ' '; s++; break; // turn CR into space
      case 10: *d++ = ' '; s++; break; // turn LF into space
      case '%':
        s++; if (!*s) break; // handle malformed input
        *d = hexValue(*s++) << 4;
        if (!*s) break; // handle malformed input
        *d |= hexValue(*s++);
        d ++;
        break;
      default:
        *d++ = *s++; break;
    }
  }

  *d = '\0'; // always add the terminator
}

inline int hexValue(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  return 0;
}

char* string2char(String command) {
  if (command.length() != 0) {
    char *p = const_cast<char*>(command.c_str());
    return p;
  }
}
// moves old strings out
void cyclestrings(String newone)
{
  newone.replace('\r', ' ');
  newone.replace('\n', ' ');//0xE2 0x80 0x8B
  newone.replace("<", "&lt;"); // prevent script injection; doing it here once is more efficient than doing it repeatedly.
  newone.replace("  ", " ");
  newone.trim();

#ifdef COMMUNITY_MEMORY_SIZE
  for (int i = COMMUNITY_MEMORY_SIZE; i > 1; i--)
  {
    communitymemory[i - 1] = communitymemory[i - 2];
  }
  communitymemory[0] = string_rx[9];
#endif
  for (int i = 10; i > 1; i--)
  {
    timestamp_rx[i - 1] = timestamp_rx[i - 2];
    string_rx[i - 1] = string_rx[i - 2];
  }

  timestamp_rx[0] = pseudoseconds;
  string_rx[0] = newone;
  dodisplay = true;
  DoDisplayIfItExists();
#ifdef user_button_display
  display_on_ping = false;
#else
  display_on_ping = true;
#endif
}

#define ANTISPAMTIME 3 // in (pseudo) seconds. for radio
#define ANTISPAM_TIME_SERIAL 1 // for serial

void TimeToForget()
{
  PetTheWatchdog();
  LastThingISentViaLora_3 = ((pseudoseconds - LTISVL_3_time) > ANTISPAMTIME) ? 0 : LastThingISentViaLora_3;
  LastThingISentViaLora_2 = ((pseudoseconds - LTISVL_2_time) > ANTISPAMTIME) ? 0 : LastThingISentViaLora_2;
  LastThingISentViaLora_1 = ((pseudoseconds - LTISVL_1_time) > ANTISPAMTIME) ? 0 : LastThingISentViaLora_1;
  LastThingISentViaLora_0 = ((pseudoseconds - LTISVL_0_time) > ANTISPAMTIME) ? 0 : LastThingISentViaLora_0;
}

unsigned long antispam_timestamp = 0;
void LoraSendAndUpdate(String whattosend)
{
  PetTheWatchdog();

  LTISVL_3_time = LTISVL_2_time;
  LTISVL_2_time = LTISVL_1_time;
  LTISVL_1_time = LTISVL_0_time;
  LTISVL_0_time = pseudoseconds;
  LastThingISentViaLora_3 = LastThingISentViaLora_2;
  LastThingISentViaLora_2 = LastThingISentViaLora_1;
  LastThingISentViaLora_1 = LastThingISentViaLora_0;
  LastThingISentViaLora_0 = LongChecksum(whattosend);
  LoRa.beginPacket();
  LoRa.print(whattosend);
  LoRa.endPacket();
  if (broadcast_twice)
  {
    delay(pseudoseconds % 23); // ironically the only place where we need a delay at this point
    LoRa.beginPacket();
    LoRa.print(whattosend);
    LoRa.endPacket();
  }
  #ifdef REPEAT_UNTIL_ACK  
  boolean got_lora_ack = false;
  last_repeat_time = millis();
  message_to_repeat = whattosend;  
  #endif
}


// important: this should be copy/pasted exactly between hardware types.
long LongChecksum(String str)
{
  if (str.length() < 2)
    return -1;
  long ret = 0;
  for (byte i = 1; i < str.length() - 1; i++) // skip the first and last characters to allow for a bit of extra noise
  {
    int c = str.charAt(i);
    if (c > 31 && c < 128) // ignore invalid characters and also crlfs
      ret = ret + ((c * i) & 16777215); // mildly weird, but it catches transpositions, so sending "east" and "tase" isn't the same. 16777215 is $00FFFFFF
  }
  return ret;
}

inline void Stop_LORA()
{
  if (has_lora_been_initialized)
  {
    SeeIfAnythingOnRadio();
    LoRa.end();
    SPI.end();
    has_lora_been_initialized = false;
  }
}


void ReadFromStream(Stream &st, char buf[], byte &cnt, bool &sendout, bool streamexists, int whichtag)
{
  if (streamexists)
  {
    while (st.available() > 0)
    {
      buf[cnt++] = st.read();
      UpdateCharCounterIfDisplayExists();
      if (cnt > MAXPKTSIZE)
      {
        dodisplay = true;
        sendout = true;
        return;
      }
      if ((cnt > 1 and buf[cnt - 1] == 13))
      {
        dodisplay = true;
        sendout = true;
      }


      // eat ascii 255s that that show up
      if ((buf[0]<6 or buf[0]>127) and cnt == 1)
        cnt = 0;

      if (buf[0] == ',' and buf[2] == ',' and (buf[1] == ',' or buf[1] == '.') and (buf[3] == 13 or buf[3] == 10)) // special: send status string and memory
      {
        cnt = 0;
        buf[0] = 0;
        buf[1] = 0;
        buf[2] = 0;
        sendout = false;
        dodisplay = false;
        hextag = fourhex(derpme, spare_id_nibble + whichtag);
        st.println(":SYS: TAG:" + hextag + " BAT:" + String(currbatterylevel) + " VER:" VERSIONSTRING " UPT" + (broadcast_twice ? ";" : ":") + String(pseudoseconds) + " MEM:"); // keep :SYS: TAG: same across hardware, or edit the bluetooth app to fit
        if (string_rx[0].length() > 0)
        {
          for (int i = 10; i > 0; i--)
          {
            PetTheWatchdog();
            tempstring = string_rx[i - 1];
            tempstring.replace(";lt", "<");

            if (tempstring.length() > 0)
              st.println(tempstring);
          }
        }
      }

      if (buf[0] == 10 or buf[0] == 13 or buf[1] == 10 or buf[1] == 13) // eliminate stray RFs in case we get a CRLF, and don't send empty packets
      {
        cnt = 0;
        buf[0] = 0;
        buf[1] = 0;
        sendout = false;
        dodisplay = false;
      }
    }
  }
}

// decide whether to actually deal with this packet or not
inline bool IsValidChar(char i) {
  return (i == 13 || i == 10 || (i > 31 && i < 128));
}
inline bool IsHex(char i) {
  return ((i > 64 && i < 71) || (i > 96 && i < 103) || (i > 47 && i < 58)); // AF, af, 09
}

bool FilterIncomingLoRa() {
  byte lenlen = LoRaData.length();

#ifdef REBROADCAST_DISASTER_RADIO_PACKETS // what it says on the tin: do we also want to display it?
  //Byte 0   Byte 1        Byte 2 - 5  Byte 6 - 9  Byte 10   Byte 11 - 14  Byte 15   Byte 16   Byte 17 - 255
  //ttl      totalLength   sender      receiver    sequence  source        hopCount  metric    datagram
  //https://github.com/sudomesh/disaster-radio/wiki/Protocol#packet-structure
  if (lenlen == LoRaData.charAt(1) and (LoRaData.charAt(0) > LoRaData.charAt(15)) and (lenlen > 16))
  {
    LoRaData.setCharAt(15, (LoRaData.charAt(15) + 1) % 256); // increment the hop count
    LoraSendAndUpdate(LoRaData); // send here, but don't cycle strings or output to serial(s)
#ifdef DISPLAY_DISASTER_RADIO_PACKETS // only do this if we are rebroadcasting them, nobody likes a stalker
    String drstring = "(DR)" + TAG_END_SYMBOL + fourhex(LoRaData.charAt(2), LoRaData.charAt(3)) + fourhex(LoRaData.charAt(4), LoRaData.charAt(5)) + TAG_END_SYMBOL + LoRaData.substring(16, 176);
    Serial.println(drstring);
    cyclestrings(drstring);
    if (has_bluetooth_been_initialized)
      ESP_BT.println(drstring);
#ifdef IRC_SERVER
    irc_broadcast( drstring);
#endif
#endif
    return false; // send here, but don't cycle strings or output to serial(s)
  }
#endif




#ifdef REQUIRE_TAG_FOR_REBROADCAST
  if (lenlen < 5) // too short
    return false;
#ifdef REQUIRE_TAG_FOR_REBROADCAST_STRICT
  if (IsHex(LoRaData.charAt(0)) == false || IsHex(LoRaData.charAt(1)) == false || IsHex(LoRaData.charAt(2)) == false || IsHex(LoRaData.charAt(3)) == false)
    return false;
#endif
  if (LoRaData.charAt(4) != TAG_END_SYMBOL) // not our format
    return false;
#else
  if (lenlen < 2) // too short
    return false;
#endif

  if (LoRaData.charAt(0) == hextag.charAt(0) && LoRaData.charAt(1) == hextag.charAt(1) && LoRaData.charAt(2) == hextag.charAt(2))
    return false; // stop broadcast storms
  if (LoRaData.startsWith(lasttagimade)) // failed rebroadcast attempt, so filter out
    return false;

#ifndef GPS_SERIAL_1 // if we already have our own gps, use it. otherwise do this
  if (lenlen > 14 and LoRaData.charAt(3) == '0' and LoRaData.charAt(5) == 'U' and LoRaData.charAt(6) == 'T' and LoRaData.charAt(7) == 'C' and LoRaData.charAt(8) == ':') // Do a bunch of checks to make sure we're getting a good packet
  {
    if (LoRaData.charAt(9) == '1' or LoRaData.charAt(9) == '2')
    {
      if (LoRaData.charAt(14) > 47 && LoRaData.charAt(14) < 58)
      {
        UTC_Seconds = (LoRaData.charAt(9)  - '0') * 100000 + // display fix type
                      (LoRaData.charAt(10) - '0') * 10000 +
                      (LoRaData.charAt(11) - '0') * 1000 +
                      (LoRaData.charAt(12) - '0') * 100 +
                      (LoRaData.charAt(13) - '0') * 10 +
                      (LoRaData.charAt(14) - '0') * 1;

#ifdef DO_NOT_LOG_SYSTEM_PACKETS
        LoraSendAndUpdate(LoRaData); // send here, but don't cycle strings or output to serial(s)
        return false; // send here, but don't cycle strings or output to serial(s)
#endif
      }
    }
  }
#endif

  long chk = LongChecksum(LoRaData);
  TimeToForget(); // erase lastthing... after a fixed time; prevents broadcast storms
  if (chk == LastThingISentViaLora_0 or chk == LastThingISentViaLora_1 or chk == LastThingISentViaLora_2 or chk == LastThingISentViaLora_3)
    return false;
  byte i = 0;
  byte testbyte = 0;
  for (i = 0; i < lenlen; i++)
  {
    if (IsValidChar(LoRaData.charAt(i)) == false)
      testbyte++;
  }
  if (testbyte > (i / BAD_CHARACTERS_MAX_DIVIDER))
  {
    return false;
  }
  // looks like we're good!
  return true;
}


String UTC_String(long secs)
{
  if (secs < 100000)
    return "N/A";
  //  String st = (secs > 199999) ? "A" : "V";
  String st;
  secs = secs % 100000;

  int h, m, s;

  s = secs % 60;

  secs = (secs - s) / 60;
  m = secs % 60;

  secs = (secs - m) / 60;
  h = secs;

  st = st + String(h) + ":" + String(m) + ":" + String(s);
  return st;
}

#ifdef SEND_TWICE
int RSSI_0 = RSSI_TRE_HI; //start neutral
int RSSI_1 = RSSI_TRE_HI; //start neutral
int RSSI_2 = RSSI_TRE_HI; //start neutral
#endif

void SeeIfAnythingOnRadio() {
  //see if there's anything on the radio, and if there is, be ready to send it
  int packetSize = LoRa.parsePacket();
  if (packetSize) {
    //received a packet

#ifdef SEND_TWICE
    RSSI_2 = RSSI_1;
    RSSI_1 = RSSI_0;
    RSSI_0 = rssi;
#endif
    rssi = LoRa.packetRssi();
    //put the filter here so that it also evaluates garbage packets, in the hope of reducing said garbage later. the logic is that if i'm away from someone, they are away from me, and they may benefit from erxtra loudness.
#ifdef SEND_TWICE
    if (RSSI_2 < RSSI_TRE_LO or RSSI_1 < RSSI_TRE_LO or RSSI_0 < RSSI_TRE_LO or rssi < RSSI_TRE_LO)
    {
      broadcast_twice = true;
    }
    else if (RSSI_2 > RSSI_TRE_HI and RSSI_1 > RSSI_TRE_HI and RSSI_0 > RSSI_TRE_HI and rssi > RSSI_TRE_HI)
    {
      broadcast_twice = false;
    }
#endif



    //read packet
    while (LoRa.available()) {
      LoRaData = LoRa.readString();
      LoRaData.trim();
      if (FilterIncomingLoRa())//(LoRaData.length() > 1) and (LoRaData.substring(1).equals(LastThingISentViaLora_0.substring(1)) == false))
      {
#ifdef SHOW_RSSI
        if (rssi < -99)
          tempstring = String(rssi) + ":" + LoRaData;
        else
          tempstring = " " + String(rssi) + ":" + LoRaData;
        Serial.println(tempstring);
        cyclestrings(tempstring);
        if (has_bluetooth_been_initialized)
          ESP_BT.println(tempstring);
        tempstring = "";
#else
        Serial.println(LoRaData);
        cyclestrings(LoRaData);
        if (has_bluetooth_been_initialized)
          ESP_BT.println(LoRaData);
#endif
        if (LoRaData.startsWith(":SYS:") == false) // avoid spamming
          LoraSendAndUpdate(LoRaData);

#ifdef IRC_SERVER
        irc_broadcast( LoRaData);
#endif
      }
    }
    dodisplay = true;
  }
}

#ifdef GPS_SERIAL_1
char gpsnom[18];
unsigned long next_utc_to_send = 0;
void DoUTC(bool changed_pseudosecond) // harvests UTC time since that's all we care about. that means keep the last 18 characters.
{
  if (has_serial_been_initialized == false)
    return;

  if (Serial1.available())
  {
    for (byte i = 18; i > 1; i--)
    {
      gpsnom[i - 1] = gpsnom[i - 2];
    }
    gpsnom[0] = Serial1.read();

    //GxRMC or GxGGA string has UTC date right in front, so eat it up
    if (gpsnom[17] == '$' and (gpsnom[14] == 'R' or gpsnom[14] == 'G') and (gpsnom[13] == 'M' or gpsnom[13] == 'G') and (gpsnom[12] == 'C' or gpsnom[12] == 'A') and (gpsnom[0] == 'A' or gpsnom[4] == 'A' or gpsnom[0] == 'V' or gpsnom[4] == 'V'))
    {
      UTC_Seconds =    ((gpsnom[0] == 'A' or gpsnom[4] == 'A') ? 200000 : 100000) + // easier to parse if we know how many leading zeros we have. 2 = A , 1 = V
                       (gpsnom[10] - '0') * 36000
                       + (gpsnom[9] - '0') * 3600
                       + (gpsnom[8] - '0') * 600
                       + (gpsnom[7] - '0') * 60
                       + (gpsnom[6] - '0') * 10
                       + (gpsnom[5] - '0') * 1; // this is actually seconds from start of day

      if (pseudoseconds > next_utc_to_send)
      {
        String utcstring = fourhex(derpme, spare_id_nibble + 0) + TAG_END_SYMBOL + "UTC:" + String(UTC_Seconds);
        LoraSendAndUpdate(utcstring);
        //cyclestrings(utcstring); // do not spam yourself
#ifdef IRC_SERVER
        //irc_broadcast( utcstring); // do not spam IRC
#endif
        next_utc_to_send = pseudoseconds + (GPS_SERIAL_1 - 1); // one hour should do it
      }
    }
  }
}
#else
#ifdef WIFI_IS_CLIENT
void DoUTC(bool changed_pseudosecond)// use NTP here maybe? for now, harvests UTC time since that's all we care about
{
  // keep up until the next UTC message
  if (changed_pseudosecond)
  {
    UTC_Seconds++;
    if (UTC_Seconds > 286399) // number of seconds in a day (A fix)
      UTC_Seconds = 200000;
    else if (UTC_Seconds > 186399 and UTC_Seconds < 200000) // number of seconds in a day (V fix)
      UTC_Seconds = 100000;
    else if (UTC_Seconds < 99000) // invalid, so leave it alone
      UTC_Seconds = 0;
  }
}
#else
void DoUTC(bool changed_pseudosecond)// harvests UTC time since that's all we care about
{
  // keep up until the next UTC message
  if (changed_pseudosecond)
  {
    UTC_Seconds++;
    if (UTC_Seconds > 286399) // number of seconds in a day (A fix)
      UTC_Seconds = 200000;
    else if (UTC_Seconds > 186399 and UTC_Seconds < 200000) // number of seconds in a day (V fix)
      UTC_Seconds = 100000;
    else if (UTC_Seconds < 99000) // invalid, so leave it alone
      UTC_Seconds = 0;
  }
}
#endif
#endif

unsigned long antispam_timestamp2 = 0;
unsigned long antispam_timestamp1 = 0;
void SendSerialIfReady()
{
  // do actual sending; stay in send mode for as little as possible; this should be followed by the receive function
  if (readytosend and (pseudoseconds > antispam_timestamp1))
  {
    if (enablecomport == false)
    {
      if (String(receivedChars).startsWith(enablecomstring))
      {
#ifdef REPEATER_ONLY

#else
#ifdef DEBUG_OPTION_PAGE
        if (String(receivedChars).startsWith(enablecomstring "#"))
        {
          Serial.println(":SYS:Entering power state FULL");
          HighPowerSetup(true);
        }
        else if (String(receivedChars).startsWith(enablecomstring "@"))
        {
          Serial.println(F(":SYS:Entering power state REPEATER"));
          LowPowerLoop();
        }
        else if (String(receivedChars).startsWith(enablecomstring "!"))
        {
          Serial.println(":SYS:Entering power state SLEEP");
          SleepLowBatt();
        }
        else
#endif
#endif
        {
          Serial.println(":SYS:Serial port TX enabled");
          enablecomport = true;
        }

      }
      else
      {
        Serial.println(":SYS:Enable port TX first: " enablecomstring);
      }
      for (int i = 0; i < MAXPKTSIZE; i++)
      {
        receivedChars[i] = 0;
      }
      charcounter = 0;
      readytosend = false;
      dodisplaybuf = true;
      return;
    }
    //Send LoRa packet to receiver
    //  Serial.print("TX:");
    //  Serial.println(receivedChars);
    hextag = fourhex(derpme, spare_id_nibble + 15);
    lasttagimade = hextag;
    String serstr = String(receivedChars);
    charcounter = 0;
#ifdef GPS_SERIAL_1
    if (serstr.startsWith("UTC:"))
      serstr = fourhex(derpme, spare_id_nibble + 0) + TAG_END_SYMBOL + "UTC:" + String(UTC_Seconds);
    else
#endif
      serstr = hextag + TAG_END_SYMBOL + serstr;
    int highbytenums = serstr.length();
    for (int i = 0; i < MAXPKTSIZE; i++)
    {
      if (receivedChars[i] > 127)
        highbytenums = highbytenums + 50;
      receivedChars[i] = 0;
    }

    if (highbytenums > MAXPKTSIZE)
    {
      Serial.println(":SYS:Serial port noise detected, turning it off.");
      enablecomport = false;
      charcounter = 0;
      readytosend = false;
      dodisplaybuf = true;
      return;
    }

    if (LongChecksum(serstr) != LastThingISentViaLora_0 && (serstr.length() > 5))
    {
      if (has_bluetooth_been_initialized)
        ESP_BT.println(serstr);
      LoraSendAndUpdate(serstr);
      cyclestrings(serstr);

#ifdef IRC_SERVER
      irc_broadcast( serstr);
#endif

      antispam_timestamp1 = pseudoseconds + ANTISPAM_TIME_SERIAL;
      dodisplay = true;
      readytosend = false;
    }
  }

  if (readytosen2 and (pseudoseconds > antispam_timestamp2))
  {
    //Send LoRa packet to receiver
    hextag = fourhex(derpme, spare_id_nibble + 14);
    lasttagimade = hextag;
    String serstr = String(receivedChar2);
    charcounte2 = 0;
#ifdef GPS_SERIAL_1
    if (serstr.startsWith("UTC:"))
      serstr = fourhex(derpme, spare_id_nibble + 0) + TAG_END_SYMBOL + "UTC:" + String(UTC_Seconds);
    else
#endif
      serstr = hextag + TAG_END_SYMBOL + serstr;
    for (int i = 0; i < MAXPKTSIZE; i++)
    {
      receivedChar2[i] = 0;
    }
    if (LongChecksum(serstr) != LastThingISentViaLora_0 && (serstr.length() > 5))
    {
      LoraSendAndUpdate(serstr);
      antispam_timestamp2 = pseudoseconds + ANTISPAM_TIME_SERIAL;
      Serial.println(serstr);
      dodisplay = true;
      readytosen2 = false;
      cyclestrings(serstr);
#ifdef IRC_SERVER
      irc_broadcast( serstr);
#endif
    }
  }
}


#ifndef NO_BINARY_SUPPORT
// the binary file must be imported as a header using bin2c because I don't want a filesystem on this build, it's too much overhead rn
void SendBinaryFile(String contenttype, const unsigned char file[], const long int filesize)
{
  // this obviously should be its own function since we are using it more than once.
  wificlient1.print("HTTP/1.1 200 OK\r\n"
                    "Content-Description: File Transfer\r\n"
                    "Content-type:");
  wificlient1.print(contenttype);
  wificlient1.print("\r\nConnection: close\r\n"
                    "Content-Transfer-Encoding: binary\r\n"
                    "Content-Length: ");
  wificlient1.println(filesize);
  wificlient1.println();
  char ch;
  byte stuff[32]; // 32 bytes is a surprisingly good compromise between speed and memory overhead
  byte j = 0;
  int i;
  for (i = 0; i < filesize; i++)
  {
    stuff[j] = (byte)file[i];
    if (++j == 32)
    {
      wificlient1.write(stuff, 32);
      j = 0;
    }
    if (i == (filesize - 1))
    {
      wificlient1.write(stuff, j);
    }

    // attempt slow operation while the data gets sent; other wifi clients will just have to wait and that's all there is to it
    if (i % 64 == 0)
    {
      PetTheWatchdog();

      ReadFromStream(Serial, receivedChars, charcounter, readytosend, has_serial_been_initialized, 15);
#ifdef BT_ENABLE_FOR_AP
      ReadFromStream(ESP_BT, receivedChar2, charcounte2, readytosen2, has_bluetooth_been_initialized, 14);
#endif
    }
    if (i % 512 == 0)
      SeeIfAnythingOnRadio();
    if (i % 512 == 128)
      DoDisplayIfItExists();
    if (i % 512 == 256)
      UpdateCharCounterIfDisplayExists();
    if (i % 512 == 384)
      SendSerialIfReady();
  }
  wificlient1.flush();
  wificlient1.println();
}
#endif


void Start_LORA(bool trydisplay)
{
  if (has_lora_been_initialized)
    return;

  Stop_LORA();
  SPI.begin(SCK, MISO, MOSI, SS);
  LoRa.setPins(SS, RST, DIO0);

  LoRa.setSpreadingFactor(9); // default 7
  LoRa.setSignalBandwidth(62.5E3); //default 125E3
  //  LoRa.crc(); // we would rather get a damaged packet than no packet at all; maybe some of it will be readable.

  //  bool changeclock = (getCpuFrequencyMhz() < CLKFREQ);

  //  if (changeclock)
  //    setCpuFrequencyMhz(CLKFREQ);

  has_lora_been_initialized = LoRa.begin(BAND);
  if (has_lora_been_initialized == false)
    has_lora_been_initialized = LoRa.begin(BAND);

  //  if (changeclock)
  //    setCpuFrequencyMhz(CLKFREQ_LOW);

  if (has_lora_been_initialized == false)
  {
    LoRa.end();
    SPI.end();
    tempstring = ":SYS:Starting LoRa failed! Trying again after sleep.";
    if (has_serial_been_initialized)
    {
      Serial.println(tempstring);
      Serial.flush();
    }
#ifndef NODISPLAY
    if (has_display_been_initialized and trydisplay and displayexists and displayenabled)
    {
      display.setCursor(0, 10);
      display.println(tempstring);
      display.display();
    }
#endif
    esp_deep_sleep_start();

  }
  else
  {
    LoRa.setTxPower(19, 1 ); // PABOOST; for RFO use 14,0
  }
}

void Start_UART()
{
  Serial.begin(UART_BAUD_RATE);
#ifdef GPS_SERIAL_1
  Serial1.begin(9600, SERIAL_8N1, 34, 12);
#endif
  has_serial_been_initialized = true;
}
void Stop_UART()
{
  Serial.flush();
  Serial.end();
#ifdef GPS_SERIAL_1
  Serial1.flush();
  Serial1.end();
#endif
  has_serial_been_initialized = false;
}

void Start_BT()
{
  if (!btStarted()) {
    btStart();
  }
  if (ipstring_b.length() < 2)
    ipstring_b = String(SSIDROOT) + String("BT") + String(derpme);
  ESP_BT.begin(ipstring_b); //Name of your Bluetooth Signal
  has_bluetooth_been_initialized = true;
}

void Stop_BT()
{
  ESP_BT.end();
  btStop(); // we aren't using BT for this one
  has_bluetooth_been_initialized = false;
}

// repeater-only mode for when we are low on juice
void LowPowerSetup()
{
  Watchdog(false);
  Stop_BT();
  dnsServer.stop();
  server.stop();
  WiFi.mode(WIFI_OFF);

#ifndef NODISPLAY
  if (has_display_been_initialized)
  {
    displayonoff(true);
    display.clearDisplay();
    display.display();
    displayonoff(false);
  }
#endif
  dodisplay = false;

  Stop_LORA();

  Stop_UART();

  setCpuFrequencyMhz(CLKFREQ_LOW); //Set CPU clock to 10MHz fo example

  PetTheWatchdog();

  Start_UART();

  Start_LORA(false);


  tempstring = ":SYS:LOWPWR " + String(derpme) + ":" + status_string();
  Serial.println(tempstring);

  if (low_batt_announce)
  {
    LoraSendAndUpdate(tempstring);
    cyclestrings(tempstring);
#ifdef IRC_SERVER
    irc_broadcast( tempstring);
#endif
  }

  ReadBatteryADC(true);
}


void DoBasicSteps()
{
  PetTheWatchdog();
  ReadBatteryADC(false);

  if (has_lora_been_initialized)
  {
    SeeIfAnythingOnRadio();
         
    #ifdef REPEAT_UNTIL_ACK
    if(LoRaData.length() >= 1) // if no packet, don't change ack state, just broadcast again.
    {
      got_lora_ack = (LoRaData.equals(message_to_repeat));
    }
    if(!got_lora_ack)
    {       
      if((millis() - last_repeat_time) > REPEAT_DELAY and message_to_repeat.length() > 1)
      {
        LoraSendAndUpdate(message_to_repeat);
      }
    }    
    #endif
  }


  ReadFromStream(Serial, receivedChars, charcounter, readytosend, has_serial_been_initialized, 15);

  ReadFromStream(ESP_BT, receivedChar2, charcounte2, readytosen2, has_bluetooth_been_initialized, 14);

  if ((has_serial_been_initialized || has_bluetooth_been_initialized) && has_lora_been_initialized)
    SendSerialIfReady();
}

void DoRepeaterSteps()
{

  if (has_serial_been_initialized == false)
    Start_UART();
  if (has_lora_been_initialized == false)
    Start_LORA(false);
  if (has_display_been_initialized)
    ResetDisplayViaPin();


  //  Serial.println(""+String(has_serial_been_initialized)+String(has_lora_been_initialized)+String(has_display_been_initialized));
  DoBasicSteps();

#ifdef BATT_ADC
  if (currbatterylevel < (BATT_TOO_LOW_FOR_ANYTHING))
    SleepLowBatt();
#endif

#ifdef USER_BUTTON_PIN
  if (digitalRead(USER_BUTTON_PIN) == false)
    if ((lploops + lploops) < LPLOOP_BLINK) // blink for me if we push the button, to denote aliveness
      if (digitalRead(USER_BUTTON_PIN) == false)
      {
        lploops = LPLOOP_BLINK - 10;
      }
#endif

  BlinkMeWhen();

}
/**
   END LORA32 RADIO BLOCK
*/



/********************************************
   DISPLAY BLOCK: USED FOR HANDLING OLED
*/
void ResetDisplayViaPin()
{
#ifndef NODISPLAY
  //  Wire.end();
  pinMode(OLED_RST, OUTPUT);
  digitalWrite(OLED_RST, LOW);
  for (byte i = 0; i < 20; i++)
    DoBasicSteps();
  digitalWrite(OLED_RST, HIGH);
  has_display_been_initialized = false;
  Wire.begin(OLED_SDA, OLED_SCL);
#endif
}
void led(boolean onoff)
{
#ifdef USER_LED_PIN
  digitalWrite(USER_LED_PIN, onoff);
#endif
}

void displayonoff(bool onoff)
{
#ifdef NODISPLAY
  displayenabled = false;
#else
  if (onoff)
  {
    if (has_display_been_initialized == false)
      InitDisplayTryAll();
    display.ssd1306_command(SSD1306_DISPLAYON);
    displayenabled = true;
  }
  else
  {
    if (has_display_been_initialized)
    {
      display.clearDisplay();
      display.display();
      display.ssd1306_command(SSD1306_DISPLAYOFF);
    }
    displayenabled = false;
  }
#endif
}

#ifdef WIFI_IS_HYBRID
int displayswitch = 0; // display switch if we are in hybrid mode
#endif

void DoDisplayIfItExists()
{
#ifndef NODISPLAY
#ifdef user_button_display
  if (digitalRead(USER_BUTTON_PIN) == false or display_on_ping == true) // actually button pushed
#else
  if (display_on_ping == true)
#endif
  {
    if (has_display_been_initialized == false) // try without hard reset first
      InitDisplayTryAll();
    dodisplay = true;
    displayonoff(true);
    disptimeout = millis() + DISPLAY_INTERVAL;
    display_on_ping = false;
  }
  if (millis() > (disptimeout))
  {
    displayonoff(false);
    dodisplaybuf = false;
    dodisplay = false;
  }

  // either update the entire display, or just the buffer indicator (faster)
  if (dodisplay) // update entire display
  {

    if (displayexists and displayenabled)
    {

      display.clearDisplay();
      display.setCursor(81, 0);
      display.print(currbatterylevel);
      display.setCursor(110, 0);
      display.print(charcounter);
      display.setCursor(0, 0);
      if (wifimode)
      {
#ifdef WIFI_IS_CLIENT // getting noise here for some reason
#ifdef WIFI_IS_HYBRID
        display.println((++displayswitch % 2) ? ipstring_c : ipstring_a);
#else
        display.println(ipstring_c);
#endif
#else
        display.println(ipstring_a);
#endif
      }
      else
      {
        display.println(ipstring_b);
      }
      // print the last 3 lines we got, or at least the first 42 characters of each since it's what will fit.
      for (int i = 2; i > -1; i--)
      {
        display.setCursor(0, 45 - (i * 18));
        tempstring = string_rx[i];
        tempstring.replace("&lt;", "<");
        display.println(tempstring.substring(0, 42));
      }
      display.display();
    }
    dodisplaybuf = false;
    dodisplay = false;
  }
#endif
}

void UpdateCharCounterIfDisplayExists()
{
#ifndef NODISPLAY
  if (has_display_been_initialized == false)
    InitDisplayTryAll();

  if ((dodisplay == false) and displayexists and displayenabled and dodisplaybuf)
  {
    display.fillRect(110, 0, 45, 11, BLACK); // upperleftx, upperlefty, width, height, color
    display.setCursor(110, 0);
    display.print(charcounter);
    display.display();
    dodisplaybuf = false;
  }
#endif
}




int countme = 0;


bool InitDisplay(bool hardreset)
{
#ifndef NODISPLAY
  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3c, hardreset, false)) { // Address 0x3C for 128x32
    if (has_serial_been_initialized)
      Serial.println(":SYS:SSD1306 init fail");
    displayexists = false;
    has_display_been_initialized = false;
    return false;
  }
  else
  {
    display.setTextColor(WHITE);
    display.setTextSize(1);
    display.clearDisplay();
    //    Serial.println(":SYS:SSD1306 init OK");
    display.display();
    displayexists = true;
    dodisplay = true;
    has_display_been_initialized = true;
    return true;
  }
#endif
  has_display_been_initialized = false;
  return false;
}

void InitDisplayTryAll()
{
#ifndef NODISPLAY
  if (has_display_been_initialized == false)
    has_display_been_initialized = InitDisplay(false);
  if (has_display_been_initialized == false)
    has_display_been_initialized = InitDisplay(true);
  if (has_display_been_initialized == false)
  {
    ResetDisplayViaPin();
    has_display_been_initialized = InitDisplay(false);
  }
  if (has_display_been_initialized == false)
    has_display_been_initialized = InitDisplay(true);
#endif
}

void BlinkMeWhen()
{
  if (++lploops > LPLOOP_BLINK)
  {
    #ifdef MINDISPLAY
    if (pseudoseconds>3)
    {
      oled.clearDisplay();
      oled.sendCommand(SSD1306_Display_Off_Cmd);
    }
    #endif
    led(true);
    DoBasicSteps();
    lploops = 0;
    led(false);
  }
}


/**
   END DISPLAY BLOCK
*/


/********************************************
   POWER MANAGEMENT BLOCK: USED FOR HANDLING BATTERY AND SLEEP
*/
#ifdef BATT_ADC
void ReadBatteryADC(bool force)
{
  if (force || (millis() > battimeout))
  {
    Watchdog(true);
    if (currbatterylevel > 0)
      lastbatterylevel = currbatterylevel;
    currbatterylevel = (analogRead(BATT_ADC) + analogRead(BATT_ADC) + analogRead(BATT_ADC) + analogRead(BATT_ADC)) / 4;
    if (lastbatterylevel == 0)
      lastbatterylevel = currbatterylevel;
    batt_delta = currbatterylevel - lastbatterylevel;
    if (batt_delta > 100 or batt_delta < -100)
      batt_delta = 0; // unrealistic, discard.
    battimeout = millis() + ADC_INTERVAL; // should be less than that since these instructions take time, but meh
  }
}
#else
void  ReadBatteryADC(bool force)
{
  lastbatterylevel = FULL_BATT;
  currbatterylevel = FULL_BATT;
  batt_delta = 0;
}
#endif

void SleepLowBatt()
{
#ifdef BATT_ADC
  tempstring = ":SYS:SLEEP " + String(derpme) + ":" + status_string();
  Watchdog(false);
  led(true);
  if (low_batt_announce)
  {
    LoraSendAndUpdate(tempstring);
    cyclestrings(tempstring);
#ifdef IRC_SERVER
    irc_broadcast( tempstring);
#endif
  }
  lastbatterylevel = currbatterylevel;
  lowpowerstart = true;
  displayonoff(false);
  led(false);

  displayonoff(false);

  //display.end();
  // turn off external devices
  Serial.println(tempstring);
  Serial.flush();
  Stop_LORA();
  Stop_BT();
  WiFi.mode(WIFI_OFF);
  server.stop();
  dnsServer.stop();
  TryStoreSentences();
  esp_deep_sleep_start();
#endif
}

/**
   END POWER MANAGEMENT BLOCK
*/
/**
   OPERATIONS BLOCK - this is where logic for pylon goes
*/

void LowPowerLoop()
{
  LowPowerSetup();
  Watchdog(true);
  while (currbatterylevel < (BATT_HIGH_ENOUGH_FOR_FULL_POWER + BATT_HYSTERESIS_POWER))
  {
    DoRepeaterSteps();
  }
  // continue flow
}

void DoBtSteps()
{
  PetTheWatchdog();


#ifdef BATT_ADC
  ReadBatteryADC(false);
  if (currbatterylevel < (BATT_TOO_LOW_FOR_ANYTHING))
    SleepLowBatt();
  if (currbatterylevel < (BATT_HIGH_ENOUGH_FOR_FULL_POWER - BATT_HYSTERESIS_POWER))
  {
    LowPowerLoop();
    HighPowerSetup(true);
  }
#endif
  SeeIfAnythingOnRadio();
  ReadFromStream(Serial, receivedChars, charcounter, readytosend, has_serial_been_initialized, 15);
  ReadFromStream(ESP_BT, receivedChar2, charcounte2, readytosen2, has_bluetooth_been_initialized, 14);
  DoDisplayIfItExists();
  UpdateCharCounterIfDisplayExists();
  SendSerialIfReady();
  BlinkMeWhen();
}

#ifdef MODEFLIP
unsigned long modefliptime;
#endif

void DoWifiSteps()
{

  PetTheWatchdog();

#ifdef BATT_ADC
  ReadBatteryADC(false);
  if (currbatterylevel < (BATT_TOO_LOW_FOR_ANYTHING))
    SleepLowBatt();
  if (currbatterylevel < (BATT_HIGH_ENOUGH_FOR_FULL_POWER - BATT_HYSTERESIS_POWER))
  {
    LowPowerLoop();
    HighPowerSetup(true);
  }
#endif

#ifdef MODEFLIP // switch between full AP and repeater according to a timer, but only if there are no wifi clients.
  if ((MODEFLIP < 60000) && (millis() > (modefliptime + MODEFLIP)))
  {
    if (WiFi.softAPgetStationNum() > 0)
    {
      modefliptime = millis(); // reset it
      Serial.println(":SYS:Modeflip skipped. Clients: " + String(WiFi.softAPgetStationNum()));
    }
    else if (currbatterylevel > MODEFLIP_BATTERY_FULL)
    {
      modefliptime = millis(); // reset it
      //      Serial.println(":SYS:Modeflip skipped. Battery: " + String(currbatterylevel)); // no need to announce either since we're clearly good on resources
    }
#ifdef MODEFLIP_BUTTON
    else if (digitalRead(USER_BUTTON_PIN) == false)
    {
      modefliptime = millis(); // reset it
      Serial.println(":SYS:Modeflip skipped. Button pressed. Battery: " + String(currbatterylevel));
    }
#endif
    else
    {
      bool derpp = true;
      LowPowerSetup();
      Watchdog(true);
      while (millis() < (modefliptime + 60000) && derpp)
      {
        DoRepeaterSteps();
#ifdef MODEFLIP_BUTTON
#ifdef USER_BUTTON_PIN
        if (digitalRead(USER_BUTTON_PIN) == false)
        {
          Serial.println(":SYS:Modeflip aborted. Button pressed. Battery: " + String(currbatterylevel));
          led(true);
          BasicWhileDelay(500); // give me a moment to remove my finger
          derpp = false;
          led(false);
          BasicWhileDelay(500); // give me a moment to remove my finger
        }
#endif
#endif

      }
      HighPowerSetup(true);
      modefliptime = millis();
      Watchdog(true);
    }
  }
#endif

  SeeIfAnythingOnRadio();
  ReadFromStream(Serial, receivedChars, charcounter, readytosend, has_serial_been_initialized, 15);
#ifdef BT_ENABLE_FOR_AP
  ReadFromStream(ESP_BT, receivedChar2, charcounte2, readytosen2, has_bluetooth_been_initialized, 14);
#endif
#ifdef WIFI_IS_CLIENT
#ifdef user_button_display
  if (digitalRead(USER_BUTTON_PIN) == false) // actually button pushed
    if (digitalRead(USER_BUTTON_PIN) == false) // actually button pushed
      if (digitalRead(USER_BUTTON_PIN) == false) // actually button pushed
      {
        ConnectToUpstreamWifi(100); // force the issue; no ping check
      }
#endif
#endif
  DoDisplayIfItExists();
  UpdateCharCounterIfDisplayExists();
  SendSerialIfReady();

  BlinkMeWhen();
}

/**
   SETUP METHOD - Called first by the ESP32.
*/
void setup() {
  setCpuFrequencyMhz(CLKFREQ_LOW);



#ifdef EXT_PWR_PIN
  pinMode(EXT_PWR_PIN, OUTPUT);
  digitalWrite(EXT_PWR_PIN, LOW);
#endif
#ifdef USER_BUTTON_PIN
  pinMode(USER_BUTTON_PIN, INPUT);
#endif
#ifdef USER_LED_PIN
  pinMode(USER_LED_PIN, OUTPUT);
#endif

  led(false);


#ifdef BATT_ADC

  pinMode(BATT_ADC, INPUT);

  esp_sleep_enable_timer_wakeup(SLEEP_TIME * uS_TO_S_FACTOR);

  // determine cause of reset
  esp_reset_reason_t reason = esp_reset_reason();

  // get reg_b if reset not from deep sleep
  if ((reason == ESP_RST_DEEPSLEEP))
  {
    TryRetrieveSentences();
  }

  lowpowerstart = false;

  ReadBatteryADC(true); // important: only one before check

  if (currbatterylevel < (BATT_TOO_LOW_FOR_ANYTHING + BATT_HYSTERESIS_POWER))
  {
    SleepLowBatt();
  }

  if (currbatterylevel < (BATT_HIGH_ENOUGH_FOR_FULL_POWER - BATT_HYSTERESIS_POWER))
  {
    lowpowerstart = true;
  }

  // if battery is low but not critical, switch to repeater-only mode

#else
  currbatterylevel = 1500;
  lowpowerstart = false;
#endif


#ifdef WIFI_POWER_LEVEL
  WiFi.setTxPower(wifilevels[WIFI_POWER_LEVEL]);
#endif

  BuildNicknameTags();


  if (lowpowerstart)
  {
    LowPowerLoop();
  }

  HighPowerSetup(true);


}

bool currentlyserving = false;

unsigned int pausetag;
void BasicWhileDelay(int pausetime)
{
  pausetag = millis();
  while (millis() < (pausetag + pausetime))
  {
    DoBasicSteps(); // DoWifiSteps();
  }
}

#ifdef WIFI_IS_CLIENT // if defined, wifi will attach to an existing AP and act as a web gateway. it may be useful to have a way to get OUT of this mode, but that's for another day
#ifdef IRC_SERVER
int failed_irc_connections = 0;
#endif
bool CurrentlyConnecting = false;
void ConnectToUpstreamWifi(int pausetime)
{
  if (server.hasClient() || CurrentlyConnecting || wificlient1.connected() || currentlyserving)
  {
    led(true);
    BasicWhileDelay(1);
    led(false);
    return;
  }
  CurrentlyConnecting = true;
  led(true);
  Watchdog(false);
  //server.stop();
  WiFi.disconnect();
  WiFi.mode(WIFI_OFF);
  BasicWhileDelay(pausetime);

#ifdef WIFI_IS_HYBRID
  WiFi.mode(WIFI_MODE_APSTA);
  BasicWhileDelay(pausetime);
#ifndef DHCP
  WiFi.config(Client_IP, Client_Gateway, Client_Subnet, Client_Gateway, Client_Gateway);
#endif
  BasicWhileDelay(pausetime);
  WiFi.softAPConfig(AP_IP, AP_IP, IPAddress(255, 255, 255, 0));
  BasicWhileDelay(pausetime);
  if (String(WIFI_UPSTREAM_PWD).length() == 0)
    WiFi.begin(WIFI_UPSTREAM_AP);
  else
    WiFi.begin(WIFI_UPSTREAM_AP, WIFI_UPSTREAM_PWD);
  BasicWhileDelay(pausetime);
  WiFi.softAP(string2char(ssid), NULL, (1 + (derpme % 12)), false, 8); // the derpme thing is to set a channel
#else
  WiFi.mode(WIFI_MODE_STA);
  BasicWhileDelay(pausetime);
#ifndef DHCP
  WiFi.config(Client_IP, Client_Gateway, Client_Subnet, Client_Gateway, Client_Gateway);
#endif
  BasicWhileDelay(pausetime);
  WiFi.begin(WIFI_UPSTREAM_AP, WIFI_UPSTREAM_PWD);
#endif
  BasicWhileDelay(pausetime);
  long i = 0;
  led(false);
  while (WiFi.status() != WL_CONNECTED)
  {
    if (i++ > WIFI_RESET_CYCLES)
      ESP.restart();
    led(true);
    BasicWhileDelay(1);
    led(false);
    BasicWhileDelay(pausetime);
  }
#ifdef DHCP
  Client_IP = WiFi.localIP();
#endif

  LastReconnect = millis();
  ipstring = ipstring_a;//AP_IP.toString();//"Gateway";//Client_IP.toString()+"G";
  dodisplay = true;
  BasicWhileDelay(pausetime);
  server.begin();
  CurrentlyConnecting = false;
#ifdef IRC_SERVER
  failed_irc_connections = 0;
#endif
}
#endif



#ifdef MINDISPLAY
void DoMinDisplay()
{
  dodisplaybuf = false;
  dodisplay = false;
#ifdef USER_BUTTON_PIN
if (digitalRead(USER_BUTTON_PIN)==true)
  return;
#endif
  oled.sendCommand(SSD1306_Display_On_Cmd);
  oled.clearDisplay();              // Clear screen
  oled.setTextXY(0,0);              // Set cursor position, start of line 0
  oled.putString("CellSol" VERSIONSTRING "(" PYLONMODEL ":" PYLONTYPE);
  oled.setTextXY(1,0);              // Set cursor position, start of line 1
  oled.putString(wifimode?ipstring_a:"Bluetooth");
  #ifdef WIFI_IS_HYBRID
  oled.putString(wifimode?ipstring_c:"");
  #endif
  oled.setTextXY(2,0);
  oled.putString(string_rx[2].substring(0,24));
  oled.setTextXY(3,0);
  oled.putString(string_rx[2].substring(25,49));
  oled.setTextXY(4,0);
  oled.putString(string_rx[1].substring(0,24));
  oled.setTextXY(5,0);
  oled.putString(string_rx[1].substring(25,49));
  oled.setTextXY(6,0);
  oled.putString(string_rx[0].substring(0,24));
  oled.setTextXY(7,0);
  oled.putString(string_rx[0].substring(25,49));
}
#endif


void HighPowerSetup(bool echo)
{
  Watchdog(false);
  setCpuFrequencyMhz(CLKFREQ); //Set CPU clock to 80MHz fo example
  led(true);
  ReadBatteryADC(true);
  lowpowerstart = false;

  Start_UART();

  wifimode = true; // adjust later

  //    esp_wifi_set_protocol( WIFI_IF_AP, WIFI_PROTOCOL_LR ); //LR only works between esp32s sadly
  WiFi.mode(WIFI_AP); // WIFI_AP_STA
  //  esp_wifi_set_protocol( WIFI_IF_AP, WIFI_PROTOCOL_11B ); // LR only works between esp32s sadly
  WiFi.softAP(string2char(ssid), NULL, (1 + (derpme % 12)), false, 8); // the derpme thing is to set a channel

  low_batt_announce = POWER_STATE_CHANGE_ANNOUNCE;

  if (enablecomstring == "")
    enablecomport = true; // for tbeam etc.


  //  Stop_LORA();

  Start_LORA(false);

  //reset OLED display via software
  //initialize OLED
  DoBasicSteps();
  ResetDisplayViaPin();

  //  InitDisplay(true);
  //  displayonoff(true);

  // Connect to Wi-Fi network with SSID and password

  WiFi.softAPConfig(AP_IP, AP_IP, IPAddress(255, 255, 255, 0));

  dnsServer.start(DNS_PORT, "*", AP_IP);
  DoBasicSteps();

#ifdef WIFI_IS_CLIENT
#else
  server.setNoDelay(true);
  wificlient1.setTimeout(2);
  server.setTimeout(2);
#endif

#ifdef USER_BUTTON_PIN // true = wifi, false = bluetooth
  int j = 0;
  for (int i = 0; i < 50; i++)
  {
    BasicWhileDelay(1);
    if (digitalRead(USER_BUTTON_PIN))
      j++; // cheapo debouncing
  }
  wifimode = (j > 24);

#endif
  led(false);

#ifdef WIFI_IS_CLIENT
  wifimode = true;
#endif

  if (wifimode == false)
  {
    WiFi.mode(WIFI_OFF);
    server.stop();
    dnsServer.stop();
    ipstring = String(SSIDROOT) + String("BT") + String(derpme);
    esp_wifi_stop();
    Start_BT();
    dodisplay = true;
  }
  else
  {
#ifdef BT_ENABLE_FOR_AP
    Start_BT();
#else
    Stop_BT();
#endif

#ifdef WIFI_IS_CLIENT
    ConnectToUpstreamWifi(100);
#endif

  }
  tempstring = ":SYS:(" PYLONMODEL ";" PYLONTYPE " " + String(derpme) + ":" + status_string();
  if (low_batt_announce)
  {
    LoraSendAndUpdate(tempstring);
    cyclestrings(tempstring);
#ifdef IRC_SERVER
    irc_broadcast( tempstring);
#endif
  }
  if (echo)
    Serial.println(tempstring);

  // reset these
  AP_IP = IPAddress(192, 168, derpme, 1);
  Client_IP = IPAddress(CLIENT_IP_ADDR);
#ifdef DHCP
  Client_IP = WiFi.localIP();
#else
  Client_IP = IPAddress(CLIENT_IP_ADDR);
#endif
  IP = AP_IP;
  ipstring = IP.toString();
  ipstring_a = AP_IP.toString();
  ipstring_c = Client_IP.toString();

  server.stop();
  server.begin();


  //Serial.println(":SYS:IP " + ipstring_a + " " + ipstring_c + " " + ipstring + " " + lwc.toString());

  if (DISPLAY_INTERVAL > 10000)
  {
    display_on_ping = true;
  }

#ifdef TUTORIALSTRINGS
  if (string_rx[2].length() < 3)
  {
    if (wifimode == false)
    {
      //                                   012345678901234567890
      //              012345678901234567890
      string_rx[2] = ":SYS: Bluetooth mode on. To switch to wifi";
      string_rx[1] = "simply reset the unit You can download a  ";
      string_rx[0] = "BT chat app from the wifi page.";
    }
    else
    {

#ifdef WIFI_IS_CLIENT
      //                                   012345678901234567890
      //              012345678901234567890
#ifdef WIFI_IS_HYBRID
      string_rx[2] = ":SYS: AP IP address: " + AP_IP.toString();
#else
      string_rx[2] = ":SYS: This unit talks to upstream wifi.";
#endif
#ifdef DHCP
      string_rx[1] = "(D)Client IP address:" + Client_IP.toString();
#else
      string_rx[1] = "Client IP address:   " + Client_IP.toString();
#endif
#ifdef IRC_SERVER
      string_rx[0] = "(I)Upstream router:  " + Client_Gateway.toString();
#else
      string_rx[0] = "Upstream router:     " + Client_Gateway.toString();
#endif
#else
#ifdef BT_ENABLE_FOR_AP
      //                                   012345678901234567890
      //              012345678901234567890
      string_rx[2] = ":SYS: This unit can  also be talked to via";
      string_rx[1] = "Bluetooth. You can   download the app via";
      string_rx[0] = "wifi, then reconnect using it.";
#else
#ifdef MODEFLIP
      //                                   012345678901234567890
      //              012345678901234567890
      string_rx[2] = ":SYS: This unit will switch between full";
      string_rx[1] = "and repeater/serial  once a minute. If you";
      string_rx[0] = "can't connect, try   for another minute.";
#else

      //                                   012345678901234567890
      //              012345678901234567890
      string_rx[2] = ":SYS: To switch to   Bluetooth, reset the";
      string_rx[1] = "unit and immediately press and hold PROG";
      string_rx[0] = "after letting the RST button go.";
#endif
#endif
#endif
    }
  }
#endif



#ifdef MINDISPLAY
  pinMode(OLED_RST, OUTPUT);
  digitalWrite(OLED_RST, LOW);
  delay(20);
  digitalWrite(OLED_RST, HIGH);
  Wire.begin(OLED_SDA,OLED_SCL);  
  oled.init();                      // Initialze SSD1306 OLED display
  DoMinDisplay();
#endif
}

/****************************************************************
   WEBSITE SERVING CODE.
   This includes a full HTTP server that runs as a serial UART
   It is coded and coupled explicitly for performance
   K. Borri
 ****************************************************************/

void send_html_header(int redir = -1) // 0: redirect to root. positive: refresh every x seconds
{
  PetTheWatchdog();
  wificlient1.print("<!DOCTYPE html>"
                    "<title>CellSol WiFi Pylon " VERSIONSTRING "</title>"
                    "<meta http-equiv=\"Content-type\" content=\"text/html;charset=UTF-8\">");
  if (redir == 0)
    wificlient1.print("<meta http-equiv=\"refresh\" content=\"0;URL='/'\">");
  else if (redir > -1 && redir < 99)
    wificlient1.print("<meta http-equiv=\"refresh\" content=\"" + String(redir) + "\">");
  /*
    wificlient1.println("<meta purpose=\"Deus Nolens Exitus\" name=\"viewport\" content=\"width=device-width, initial-scale=1\"><link rel=\"icon\" href=\"data:,\">"
                   "<style>html { font-family: Helvetica; display: inline-block; margin: 0px auto; text-align: " + TEXT_ALIGN_STRING + ";}.button { background-color: #4CAF50; border: none; color: white; padding: 0px 0px; text-decoration: none; font-size: 20px; margin: 0px; cursor: pointer;}.button2 {background-color: #555555;}</style>"
                   "</head>");
  */
  wificlient1.print("<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\"><link rel=\"icon\" href=\"data:;base64,UklGRnIAAABXRUJQVlA4TGUAAAAvD8ADEB7HACRJ0kiO4rCszULMPQSJUomTEoGRb4EAodZ/AUTaNlM3/2RSjt//CZAWsHXZP7f+sbbkwv86BriQHgGS5DHnzyUJIEl4nEMAcYd7wpeRPIc5tDzTdtc2saf7SfdfAAA\">"

                    "<style>html{color-scheme:dark light;font-family:Helvetica,sans-serif;display:inline-block;margin:0 auto;text-align:");
  wificlient1.print(TEXT_ALIGN_STRING);
  wificlient1.println("}</style>"); /* old css seems unused(?) .button {border: none; color: white; padding: 0px 0px; text-decoration: none; font-size: 1.2em; margin: 0px; cursor: pointer;}.button2 {}*/
}
bool client_on_lan = true;
String hoststring = "/";

void send_string_by_itself (String st)
{
  wificlient1.println("HTTP/1.1 200 OK");
  wificlient1.println("Content-type:text/plain");
  wificlient1.println("Connection: close");
  wificlient1.println();
  //  wificlient1.println(encodeHtml(st));
  wificlient1.println(st);
  wificlient1.println();
}
void send_redirect_to_main(bool doip)
{
  PetTheWatchdog();
  wificlient1.println("HTTP/1.1 302 Found");
  if (doip)
  {
    if (client_on_lan)
      wificlient1.println("Location: http://" + ipstring); //ipstring
    else
      wificlient1.println("Location: http://" + hoststring); //ipstring
  }
  else
    wificlient1.println("Location: /");// http://" + ipstring + "/");
  wificlient1.println("Connection: close");
  wificlient1.println();
}

#ifdef TX_IFRAME
void send_form_iframe()
{
  send_ok_response();
  send_html_header();
  wificlient1.println("<small><small>TX&gt;</small></small> <input id=\"msgfield\" type=\"text\" maxlength=\"160\" name=\"input1\"><button onClick=sendMsg() value=\"Send\">Send</button><br>");
}
#endif
void send_ok_response()
{
  PetTheWatchdog();
  wificlient1.println("HTTP/1.1 200 OK");
  wificlient1.println("Content-type:text/html");
  wificlient1.println("Connection: close");
  wificlient1.println();
}

#ifdef SERVE_FAQ_PAGE
void send_faq_page()
{
  send_ok_response();
  send_html_header();
  wificlient1.println( "<style>html{text-align:start}</style>"
                       "<h1>CellSol WiFi Pylon " VERSIONSTRING " Help</h1>"
                       "<p>The full help/howto is available, when the internet works, at <a href=\"http://f3.to/cellsol/\">http://f3.to/cellsol/</a></p><p>"
                       "<h2>About the Chat</h2>"
                       "<p>By using the chat in the main page, you will be able to communicate with people who are in range of the CellSol network, or people who are using CellSol gateways."
                       "<p>Each message is sent out to neighboring pylons, which can be a few kilometers apart, and rebroadcast."
                       "<p>You can consider this system akin to a single IRC/Discord/Twitch chat channel, except it will work when the internet at large will not."
                       "<p>Any phone with a Bluetooth chat app (Android APK can be downloaded from this very pylon, iPhone or others will have to use their own) can also use the Bluetooth pylons or pocket nodes. They require very little power to operate."
                       "<p>Please note that CellSol does not store your location or identity data, but also please note that there is no encryption for these messages (to make it easier for other systems to interoperate with). This is intended to be used during or after natural disasters, so that should really not be a concern."
                       "<h3>Identifying Users</h3>"
                       "<p>The tag in front of your message is a pseudonymous identifier: it is used to tell people apart. It is four hex digits. (Sorry, no nicknames). Example: 0abc."
                       "<h2>About the Network</h2>"
                       "<p>CellSol is free to use and does not depend on any infrastructure, each pylon is self-contained: just deploy a few of them in an area and you are good to go."
                       "<p>This is a lot like existing LoRa mesh chat systems, except that it's intended to leave repeaters in place."
                       "<p>The mesh topology prioritizes redundancy over speed or cleanliness, so you may occasionally get a garbled message: we try to display those in case their meaning can be understood despite the garbling."
                       "<p>The best use for a standalone repeater is somewhere between areas with traffic, on a road for example. The best use for a Bluetooth pylon is in someone's pocket or backpack, connected to their phone."
                       "<p>The best use for a WiFi pylon is somewhere that has a stable internet connection (satellite, etc.) or generally somewhere where people go (a waypoint, base camp, etc.)"
                       "<h2>About This Pylon</h2>"
#ifdef GPS_SERIAL_1
                       "<p>It has integrated GPS or timebase, and will send a time pulse every 1 hour to other nodes." // we should make the gps fix available...
#endif

#ifdef DISPLAY_DISASTER_RADIO_PACKETS // only do this if we are rebroadcasting them, nobody likes a stalker
                       "<p>It will rebroadcast and display Disaster Radio packets."
#else
#ifdef REBROADCAST_DISASTER_RADIO_PACKETS // what it says on the tin: do we also want to display it?
                       "<p>It will rebroadcast Disaster Radio packets."
#endif
#endif


#ifdef REQUIRE_TAG_FOR_REBROADCAST_STRICT
#else
#ifdef REQUIRE_TAG_FOR_REBROADCAST
                       "<p>It will honor other mesh network systems by repeating their packets too, as long as they start with a xxxx: tag in front."
#else
                       "<p>It will honor other mesh network systems by repeating their packets too!"
#endif
#endif
                       "<p>If you are on a Bluetooth or serial CellSol pylon, typing ,,, on a line by itself will dump the pylon's status and last received strings (in case your phone loses them). Our app does this automatically on reconnect. This lets you use any terminal app."
#ifdef MODEFLIP
                       "<p>This pylon will turn on its WiFi once a minute to check for clients, and be a repeater the rest of the time, in order to save power."
#else
#ifdef BT_ENABLE_FOR_AP
                       "<p>This pylon can be accessed both from WiFi and Bluetooth. Please note that this drains the battery fairly quickly."
#else
                       "<p>This pylon is in WiFi mode right now. You can switch to Bluetooth mode by pushing the PROG button right after a reset and keeping it pushed until the screen says Bluetooth."
#endif
#endif
                       "<p>The CellSol pylons (any version of them) will work on any 3.6 or 3.7v batteries, or any USB power banks, or anything that delivers 3.5 to 6 volts, really."
                       "<h2> About the Project</h2>"
#ifdef PROVIDE_SOURCE_CODE
                       "<p>The project is open source and open schematic. You can get a copy of the source right from here! <a href=\"/src.zip\"> Source code </a>."
#else
                       "<p>The project is open source and open schematic. You can download everything from <a href=\"http://f3.to/cellsol/\">http://f3.to/cellsol/</a> or from a pylon that carries it (This one doesn't have enough memory to)."
#endif
                       "<p><a href=\"/\">Go back</a>");
  PetTheWatchdog();
  wificlient1.println();
}
#endif

void sendstrings()
{
  for (byte i = 10; i > 0; i--)
  {
    PetTheWatchdog();
    wificlient1.println("<small><small>RX(" + TimeAgoString(pseudoseconds - timestamp_rx[i - 1]) + "):</small></small>" + (string_rx[i - 1]) + "<br>");
    //    wificlient1.println("<small><small>RX(" + TimeAgoString(pseudoseconds - timestamp_rx[i - 1]) + "):</small></small>" + encodeHtml(string_rx[i - 1]) + "<br>");
  }
}
bool LoginPageRequested(String url)
{
  if (url.startsWith("/redirect"))
    return true;
  if (url.startsWith("/connecttest.txt"))
    return true;
  if (url.startsWith("/ncsi.txt"))
    return true;
  if (url.startsWith("/hotspot.txt"))
    return true;
  if (url.startsWith("/success.txt"))
    return true;
  if (url.startsWith("/generate_204"))
    return true;
  if (url.startsWith("/hotspot-detect.html"))
    return true;

  return false;

}

void ServeWebPagesAsNecessary()
{
  dnsServer.processNextRequest();

  // Do wifi things here
  wificlient1 = server.available();   // Listen for incoming clients

#ifdef WIFI_IS_CLIENT
  if (wificlient1 == false)
  {
    if (millis() > (LastReconnect + RECONNECT_EVERY))
    {
      ConnectToUpstreamWifi(100);
    }
  }
#endif
  if (wificlient1) {
    //  Serial.print("2");
    currentlyserving = true;
    //      Serial.println(String(client)); // says 1
#ifdef WIFI_IS_CLIENT
    LastReconnect = millis();
#endif
    unsigned long connect_timeout = millis() + 3000;
    while (wificlient1.connected()) {            // loop while the client's connected
      // does not get this far outside lan for some reason
      if (wificlient1.available() == false)
      {
        //  Serial.print("3");
        DoBasicSteps();
        dnsServer.processNextRequest();
        if (millis() > connect_timeout)
        {
          wificlient1.stop();
          break;
        }

      }
      char c, c1, c2;
      if (wificlient1.available()) // if there's bytes to read from the client,
      {
        //  Serial.print("4");
        PetTheWatchdog();
        c2 = c1;
        c1 = c;
        c = wificlient1.read();

        if (header.length() < (MAXPKTSIZEP + 30)) // prevent buffer overflows, and keeps strings a reasonable size.
          header += c;

        //        Serial.print(c1);

        // detect two newlines
        if ((c == 10 and c1 == 10) or (c == 13 and c1 == 13) or (c == 10 and c1 == 13 and c2 == 10)) {              // if the byte is a newline character
          PetTheWatchdog();

          // if the current line is blank, you got two newline characters in a row.
          // that's the end of the client HTTP request, so send a response:

          //Serial.print(header);

          long derp = header.indexOf("Host: ");
          hoststring = header.substring(derp + 6);
          hoststring = hoststring.substring(0, hoststring.indexOf('\n') - 1) + "/";

          String whattoget = header.substring(4, header.indexOf(" HTTP/"));

          //Serial.print(hoststring);

          lwc = wificlient1.remoteIP();
          if (lwc[2] == AP_IP[2] && lwc[1] == AP_IP[1] && lwc[0] == AP_IP[0])
          {
            IP = AP_IP;
            client_on_lan = true;
          }
          else
          {
            IP = Client_IP;
            client_on_lan = false;
          }
          //              Serial.println(":SYS:IP " + ipstring_a + " " + ipstring_c + " " + ipstring + " " + lwc.toString());

          ipstring = IP.toString();

          LastReconnect = millis(); // we're talking, so obviously there's no need to reconnect!

          //Serial.print(hextag);
          //Serial.print(String(lwc));
          //Serial.println("Header:" + header);



          // naive, and cheap, method for serving multiple pages. if someone better at HTML than me can do autorefresh of the chat contents, that'd be awesome.
          if (whattoget.equals("/favicon.ico"))
          {
            wificlient1.println("HTTP/1.1 204 No Content\r\n");
            //                wificlient1.println();// we're not actually sending anything
          }
#ifdef SERVE_FAQ_PAGE
          else if (whattoget.equals("/faq.html")) // can  be done for other emergency files here
          {
            send_faq_page();
          }
#endif
          else if (whattoget.startsWith("/last") and (whattoget.endsWith(".txt")))
          {
            int i = whattoget.charAt(5) - '0';
            if (i > -1 and i < 10)
              send_string_by_itself(string_rx[i]);
            else
              send_string_by_itself("");
          }
#ifdef COMMUNITY_MEMORY_SIZE
          else if (whattoget.equals("/mem.html")) // can  be done for other emergency files here
          {
            //Serial.println("Regular page (default)");
            send_ok_response();

            // Display the HTML web page
            // General purpose html header

            send_html_header();
            wificlient1.println( "CellSol WiFi Pylon " VERSIONSTRING " at ");
            wificlient1.print(ipstring_a);
            wificlient1.println("<div style=\"max-width:100%;\"><p><small>");
            for (int i = COMMUNITY_MEMORY_SIZE - 1; i > -1; i--)
            {
              PetTheWatchdog();
              wificlient1.print("<br><small><small>RX");
              //              wificlient1.print(i + 9);
              wificlient1.print(":</small></small>");
              //              wificlient1.println(encodeHtml(communitymemory[i]));
              wificlient1.println(communitymemory[i]);
            }
            wificlient1.print("<br>");
            sendstrings();
            wificlient1.println("</small></p><a href=\"http://" + hoststring + "\">Return to main</a></div>");
            wificlient1.println();
          }
#endif
#ifdef PROVIDE_CAT_PICTURE
          else if (whattoget.equals("/cat.jpg")) // can  be done for other emergency files here
          {
            SendBinaryFile("image/jpeg", cat_jpg, cat_jpg_size);
          }
#endif
#ifdef PROVIDE_SOURCE_CODE
          else if (whattoget.equals("/src.zip")) // can  be done for other emergency files here
          {
            SendBinaryFile("application/octet-stream", src_zip, src_zip_size);
          }
#endif
#ifdef PROVIDE_APK
          else if (whattoget.equals("/btt.apk")) // can  be done for other emergency files here
          {
            SendBinaryFile("application/octet-stream", btt_apk, btt_apk_size);
          }
#endif
          else if (whattoget.equals("/chat.html")) // chat iframe
          {
            send_ok_response();
            send_html_header(REFRESH_CHAT_EVERY);
            if (UTC_Seconds)
              wificlient1.println("<small><small><small>UTC Time Update:" + UTC_String(UTC_Seconds) + "<br></small></small>");
            else
              wificlient1.print("<small>");
            sendstrings();
            wificlient1.println("</small>");
          }
#ifdef TX_IFRAME
          else if (whattoget.equals("/answerform.html")) // answer form iframe
          {
            send_form_iframe();
          }
#endif
          else if (whattoget.startsWith("/get"))
          {
            // get header value for input1 which should be message
            gotstring = header.substring(header.indexOf("input1="));
            gotstring = gotstring.substring(0, gotstring.indexOf("HTTP/1.1"));
            gotstring = gotstring.substring(7, gotstring.indexOf("&refresh="));//HTTP/1.1")); //input1= is 7 characters
            if (gotstring.length() > (MAXPKTSIZE + 5)) // prevent intentional spam on arrival. the other input methods already do this
            {
              gotstring = "(>maxsize)" + gotstring.substring(0, 20) + "..."; // give a preview of what was sent, in case it's important and got thru even if it should not have
            }
            if (gotstring.length() > MAXPKTSIZEM) // prevent accidental spam // do not flood the lora
            {
              gotstring = gotstring.substring(0, MAXPKTSIZEM);
            }
            if (gotstring.length() > 1) // don't send empty strings or strings with just
            {
              char tempme[MAXPKTSIZE]; // slightly more efficient
              gotstring.toCharArray(tempme, MAXPKTSIZE);
              decode_in_place(tempme);
              gotstring = String(tempme);
              gotstring.trim();
              String hextag_temp;
              last_web_caller = wificlient1.remoteIP()[3];
#ifdef WIFI_IS_CLIENT
              lwc = wificlient1.remoteIP();
              hextag_temp = fourhex(lwc[0] ^ lwc[3], lwc[1] ^ lwc[2]); // should be unique enough, but mix it up so that we don't accidentally reveal someone's ip
#else
              if (last_web_caller < 16)
                last_web_caller = last_web_caller + spare_id_nibble; // do SOMETHING with the third digit
              hextag_temp = fourhex(derpme, last_web_caller);
#endif
              if (hextag_temp.charAt(3) == ('0'))
                hextag_temp.setCharAt(3, '1'); // only actual nodes are allowed to end in 0
              lasttagimade = hextag_temp;

#ifdef GPS_SERIAL_1
              if (gotstring.startsWith("UTC:"))
                gotstring = fourhex(derpme, spare_id_nibble + 0) + TAG_END_SYMBOL + "UTC:" + String(UTC_Seconds);
#endif
              gotstring = hextag_temp + TAG_END_SYMBOL + gotstring;

              if (string_rx[0].equals(gotstring) == false) // prevents sending it out twice if it's the last thing that went out
              {
                Serial.println(gotstring);
#ifdef BT_ENABLE_FOR_AP
                ESP_BT.println(gotstring);
#endif


                //Send LoRa packet to receiver. Will need to make sure we don't send duplicates.

                LoraSendAndUpdate(gotstring);
                cyclestrings(gotstring);
#ifdef IRC_SERVER
                irc_broadcast( gotstring);
#endif
              }

              gotstring = "";

            }
#ifdef TX_IFRAME
            send_form_iframe();
#else
            send_redirect_to_main(true);
#endif
          }
          else if (whattoget.equals("/refresh")) // exception is needed to make sure that the refresh button works properly
          {
            send_redirect_to_main(true);
          }
#ifdef DEBUG_OPTIONS_PAGE
          else if (whattoget.startsWith("/option!")) // semi hidden option stuff! Yay! Any more that we need? I don't want to make it possible to turn this off or switch modes remotely because that's easy to abuse.
          {
            char option = whattoget.charAt(8);
            if (option == 'C') // center text
            {
              centertext = true;
              TEXT_ALIGN_STRING = TEXT_ALIGN_STRING_A;
            }
            if (option == 'J') // justify text
            {
              centertext = false;
              TEXT_ALIGN_STRING = TEXT_ALIGN_STRING_B;
            }
            if (option == 'S') // turn serial port off (in case of spam or errors coming from it)(turn back on with the enable string)
            {
              enablecomport = false;
            }

            if (option == 'T') // reset pseudoseconds timer
            {
              pseudoseconds = 0;
            }
#ifdef IRC_SERVER
            if (option == 'I') // reset failed irc connections / try to connect to irc again
            {
              failed_irc_connections = 0;
              pongs_joins = 0;
              has_irc_been_initialized = false;
            }
#endif
            if (option == 'D') // force display on
            {
              display_on_ping = true;
            }
            if (option == 'd') // force display off
            {
              display_on_ping = false;
            }
            if (option == 'B') // force sending (broadcasting) twice on
            {
              broadcast_twice = true;
            }
            if (option == 'b') // force sending (broadcasting) twice off
            {
              broadcast_twice = false;
            }
            send_ok_response();
            send_html_header(0);
            wificlient1.print("CellSol WiFi Pylon " VERSIONSTRING " Option: <b>");
            wificlient1.print(option);
            wificlient1.println("</b><br>Redirecting to main");
            wificlient1.println();
          }
#endif
          else if (whattoget.equals("/") or whattoget.startsWith("/index.html"))
          {
            //Serial.println("Regular page (default)");
            send_ok_response();
            // Display the HTML web page
            // General purpose html header

            send_html_header();
            wificlient1.println("<script>"
                                "window.addEventListener(\"load\", function(){"
                                "var input = document.getElementById('msgfield');"
                                "input.addEventListener(\"keyup\", function(event) {"
                                "if (event.keyCode === 13) {"
                                "event.preventDefault();"
                                "document.getElementById(\"msgBtn\").click();}});});"
                                "function resizeIframe(obj) {"
                                "    obj.style.height = obj.contentWindow.document.documentElement.scrollHeight + 'px';"
                                //"    obj.style.width = obj.contentWindow.document.documentElement.scrollWidth + 'px';"
                                "  }\r\n</script>"

                                "<script>function sendMsg(){\r\n"
                                "getSend('\\get',document.getElementById('msgfield').value); document.getElementById('msgfield').value=\"\";\r\n"
                                "}\r\n"
                                "function getSend(url, input1=null) {\r\n" // we default undefined so that we don't have to explicitly specify it; it just gets passed to the next function
                                "var xhttp = new XMLHttpRequest();\r\n"
                                "xhttp.open('GET', url+\"?input1=\" + input1, true); xhttp.send();" // we don't care about the response, we are just dumb sending.
                                "}"
                                "</script>"
                                "CellSol WiFi Pylon " VERSIONSTRING " at ");
            wificlient1.print(ipstring_a);
            wificlient1.println("<br>"
#ifdef COMMUNITY_MEMORY_SIZE
                                "<a href=\"http://" + hoststring + "mem.html\">Older messages</a> "
#endif
#ifdef PROVIDE_CAT_PICTURE
                                "<a href=\"http://" + hoststring + "cat.jpg\">Cat picture</a> "
#endif
#ifdef SERVE_FAQ_PAGE
                                "<a href=\"http://" + hoststring + "faq.html\">FAQ page</a> "
#endif
                                "<br>"
                                "<div style=\"max-width:100%;\">"
                                "<iframe id=\"chatin\" src=\"/chat.html\" frameborder=\"0\" scrolling=\"no\" onload=\"resizeIframe(this);\"style=\"width:100%;\" /></iframe>"
#ifdef TX_IFRAME
                                "<iframe id=\"chatout\" src=\"/answerform.html\" frameborder=\"0\" scrolling=\"no\" style=\"width:100%; height:3em;\" /></iframe></div>"
#else
                                "<small><small>TX&gt;</small></small> <input id=\"msgfield\" type=\"text\" maxlength=\"160\" name=\"input1\"><button id=\"msgBtn\" onClick=sendMsg() value=\"Send\">Send</button><br>"
#endif
                               );
            /*
              "<iframe id=\"chatin\" src=\"/chat.html\" frameborder=\"0\" scrolling=\"no\" onload=\"resizeIframe(this);\"style=\"width:100%;\" />");
              // in case iframe doesn't work
              sendstrings();
              wificlient1.println("</iframe>"
              "<br>"
              "<iframe id=\"chatout\" src=\"/answerform.html\" frameborder=\"0\" scrolling=\"no\" style=\"width:100%; height:5em;\" />"
              // in case iframe doesn't work, it displays this
              "<form action=\"/get\"> Your message: <input type=\"text\" maxlength=\"50\" name=\"input1\"><input type=\"submit\" value=\"Submit\"><form>"
              "</iframe>");
            */
            wificlient1.println("<button value=\"Refresh this page (in case of errors, etc.)\" onClick=getSend('/refresh')>Refresh this page (in case of errors, etc.)</button>");
            wificlient1.print  ("<br><small>CellSol is a serverless relay chat between LoRa pylons. It is intended for enabling communication in case of cell phone network disruption.<br>"
                                "Bluetooth terminal APK download (you may have to enter URL in browser manually): "
#ifdef PROVIDE_APK
                                "<a href=\"http://" + hoststring + "btt.apk\" target=\"_blank\">http://" + hoststring + "btt.apk</a> , <a href=\"http://f3.to/btt.apk\" target=\"_blank\">http://f3.to/btt.apk</a><br>To help this project grow, you must construct additional pylons. <br>Sysinfo:(" PYLONMODEL ";"  PYLONTYPE " ");
#else
                                "<a href=\"http://f3.to/btt.apk\" target=\"_blank\">http://f3.to/btt.apk</a><br>To help this project grow, you must construct additional pylons. <br>Sysinfo:(" PYLONMODEL ";"  PYLONTYPE " ");
#endif
            wificlient1.print(status_string());
#ifdef PROVIDE_SOURCE_CODE
            wificlient1.print("The source code for all platforms is available <a href=\"http://" + hoststring + "src.zip\" target=\"_blank\">here.</a>");
#endif

            if (pseudoseconds % 2)
              wificlient1.println("<small> Deus Nolens Exitus");
            else
              wificlient1.println("<small> Vigilo Confido");


            // The HTTP response ends with another blank line
            wificlient1.println();
          }
          else // we were asked a page that we don't have. redirect to main
          {
            if (LoginPageRequested(whattoget))
            {
              send_redirect_to_main(true);
            }
            else
            {
              send_ok_response();
              send_html_header(0);
              wificlient1.println("CellSol WiFi Pylon " VERSIONSTRING " (" PYLONMODEL ";" PYLONTYPE" does not hold " + whattoget + "<br>Redirecting to main");
              wificlient1.println();
            }
          }
          header = "";
          break;
        }
      }
    }
    // Clear the header variable
    header = "";
    wificlient1.stop();
    DoBasicSteps();
    currentlyserving = false;
  }
}


#ifdef IRC_SERVER
String IRC_NICK;
String IRC_CHAN;
String hextag_irc;
int prefixlength = -1;
byte pongs_joins = 0;

WiFiClient wificlient2;
bool has_irc_been_initialized = false;
IRCClient ircclient(IRC_SERVER, IRC_PORT, wificlient2);

void DoIRCStuff()
{
  if (failed_irc_connections > IRC_FAILED_TRE)
    return;

  if (has_irc_been_initialized == false)
  {
    hextag_irc = fourhex(derpme, spare_id_nibble + 0);
    IRC_NICK = IRC_NICK_ROOT + hextag_irc;
#ifdef IRC_CHAN_HEXTAG
    IRC_CHAN = String("#") + IRC_CHAN_ROOT + hextag_irc;
#else
    IRC_CHAN = String("#") + IRC_CHAN_ROOT;
#endif
#ifdef FWD_PREFIX
    prefixlength = String(FWD_PREFIX).length();
#endif
    ircclient.setCallback(irc_callback);
    ircclient.setSentCallback(debugSentCallback);
    has_irc_been_initialized = true;
  }
  if (!ircclient.connected())
  {
    Serial.println(":SYS:IRC_CONN");
    // Attempt to connect
    if (ircclient.connect(IRC_NICK, IRC_NICK)) //IRC_NICKNAME, IRC_USER)) {
    {
      Serial.println(":SYS:IRC_CONN_OK");
      failed_irc_connections = 0;
      pongs_joins = 0;
    }
    else
    {
      Serial.println(":SYS:IRC_CONN_FAIL");
      // Wait 1 second before retrying
      BasicWhileDelay(100); // needs smart delay later
      failed_irc_connections++;
    }
    return;
  }

  ircclient.loop();

}
void irc_broadcast (String s)
{
  if (has_irc_been_initialized)
    if (ircclient.connected())
      ircclient.sendMessage(IRC_CHAN, s);
}

void irc_callback(IRCMessage ircMessage)
{
  PetTheWatchdog();
  // PRIVMSG ignoring CTCP messages
  if (ircMessage.command == "PRIVMSG" && ircMessage.text[0] != '\001')
  {
#ifdef FWD_PREFIX // forward if there's a valid prefix, or if there's another pylon that is using correct syntax.
    bool validforward = ircMessage.text.startsWith(FWD_PREFIX);
    bool isotherpylon = false;
#ifdef ALLOW_PYLON_FORWARD
    if (validforward == false)
      //      isotherpylon = ircMessage.nick.startsWith(IRC_NICK_ROOT) and (IsHex(ircMessage.nick.charAt(ircMessage.nick.length() - 1))) and (IsHex(ircMessage.nick.charAt(ircMessage.nick.length() - 2))) and (IsHex(ircMessage.nick.charAt(ircMessage.nick.length() - 3))) and (IsHex(ircMessage.nick.charAt(ircMessage.nick.length() - 4)));
      isotherpylon = ircMessage.nick.startsWith(IRC_NICK_ROOT) and (ircMessage.nick.charAt(ircMessage.nick.length()) == '0') and (IsHex(ircMessage.nick.charAt(ircMessage.nick.length() - 2))) and (IsHex(ircMessage.nick.charAt(ircMessage.nick.length() - 3))) and (IsHex(ircMessage.nick.charAt(ircMessage.nick.length() - 4)));
#endif
    validforward = validforward or isotherpylon;
    if (validforward)
    {
      String gotstring;
      if (isotherpylon)
      {
        //        Serial.println("ADDITIONAL PYLONS");
        gotstring = hextag_irc + TAG_END_SYMBOL + ircMessage.nick.substring(ircMessage.nick.length() - 4) + ">" + ircMessage.text.substring(0, MAXPKTSIZEM);
      }
      else
      {
        gotstring = hextag_irc + TAG_END_SYMBOL + ircMessage.nick.substring(0, 6) + ">" + ircMessage.text.substring(prefixlength, MAXPKTSIZEM);
      }
      gotstring = gotstring.substring(0, MAXPKTSIZEP);
#else
    {
      String gotstring = hextag_irc + TAG_END_SYMBOL + ircMessage.nick.substring(0, 6) + ">" + ircMessage.text;
      gotstring = gotstring.substring(0, MAXPKTSIZEP);
#endif
      Serial.println(gotstring);

#ifdef BT_ENABLE_FOR_AP
      if (has_bluetooth_been_initialized)
        ESP_BT.println(gotstring);
#endif

      LoraSendAndUpdate(gotstring);
      cyclestrings(gotstring);
    }
    return;
  }
  //Serial.print("irc_callback");
  if (ircMessage.original.endsWith("already in use."))
  {
    PetTheWatchdog();
    IRC_NICK = IRC_NICK + "_";
    if (IRC_NICK.endsWith("___"))
      IRC_NICK.replace("___", "");
    ircclient.sendRaw("NICK " + IRC_NICK);
    //ircclient.sendRaw("USER " + IRC_USER + " 8 * :Arduino IRC Client");
    //Serial.println(IRC_NICK);
  }
  // Serial.println(ircMessage.original);
}

void debugSentCallback(String data)
{
  PetTheWatchdog();
  if (data.startsWith("SENT: PONG") and (++pongs_joins < 3)) // if we are getting pings, we are in good shape as far as the irc server is concerned, so join the channel and start doing work.
  {
    ircclient.sendRaw("JOIN " + IRC_CHAN); // if already joined, rejoin, not a problem
    ircclient.sendRaw("TOPIC " + IRC_CHAN + " :" + IRC_TOPIC); // if already set, not a problem

  }
  //Serial.print("debugSentCallback");
  //Serial.println(data);
}

#endif


void loop() {

#ifdef MINDISPLAY
DoMinDisplay();
#endif

  if (wifimode)
  {
    DoWifiSteps();
    ServeWebPagesAsNecessary();
#ifdef IRC_SERVER
    DoIRCStuff();
#endif
  }
  else
  {
    DoBtSteps();
  }
}
