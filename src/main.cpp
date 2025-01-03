#include "Arduino.h"
#include <WiFi.h>

#include "WebServer.h"
#include <ESPmDNS.h>

#include <list>
#include <LittleFS.h>

#include "Audio.h"
#include "AudioTools.h"
#include "AudioTools/AudioLibs/AudioSourceLittleFS.h"
#include "AudioTools/AudioCodecs/CodecMP3Helix.h"

#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_sleep.h"
#include "esp32/rom/rtc.h"
#include "esp32/rom/ets_sys.h"
#include "soc/uart_reg.h"
#include "soc/timer_group_reg.h"
#include "hulp_arduino.h"
#include "driver/rtc_io.h"

//#define myDEBUG
#include "MyDebug.h"

#include "configuration.h"

extern void initFS();
extern void setupFS();
extern bool buildList();
extern const String formatBytes(size_t const& bytes);

//#define TST

// File List
using namespace std;
using records = tuple<String, String, int>;
list<records> dirList;
size_t usedBytes;
size_t totalBytes;
size_t freeBytes;

// Audio Output
const char *startFilePath = "/";
const char *ext = "mp3";
AudioSourceLittleFS source(startFilePath, ext);
I2SStream i2s;
MP3DecoderHelix decoder;
AudioPlayer player(source, i2s, decoder);
char fileFilter[256];

RTC_DATA_ATTR int counter=0;

int dly=0;
bool isConnected;
unsigned long currentMillis;

// Webserver
WebServer server(80);

// ULP Program, executed during deep sleep
void prepare_ulp()
{
    enum
    {
        LBL_START,
        LBL_NO_MOVEMENT,
        LBL_FINISH_UP,
    };

    const ulp_insn_t program[] = {
        M_LABEL(LBL_START),
        I_DELAY(65535),
        I_MOVI(R0, 0),
        I_GPIO_READ(BEWEGUNG),    // READ GPIO PIN in order to detect movement
        M_BL(LBL_NO_MOVEMENT, 1),

    M_LABEL(LBL_FINISH_UP),
        // Movement detected, wakeup CPU
        I_WAKE(),
        I_DELAY(65535),

    M_LABEL(LBL_NO_MOVEMENT),
        I_HALT()
    };

    ESP_ERROR_CHECK(hulp_ulp_load(program, sizeof(program), 1000*1000, 0)); // 1 Sec
    ESP_ERROR_CHECK(hulp_ulp_run(0));
}

#ifdef TST
void printMetaData(MetaDataType type, const char *str, int len)
{
    Serial.print("==> ");
    Serial.print(toStr(type));
    Serial.print(": ");
    Serial.println(str);
}
#endif

// Check Volume, Map Analog Read 0-4095 (0-3.3V) to 0-1.0
float getVolume() {
    float vol = map(analogRead(VOLUME),0,4095,0,1000);
    vol /= 1000;
    return vol==0.0?0.2:vol;
}

// Check Battery, Map Analog Read 0-4095 (0-3.3V) to 0-3.3
float getBatterie() {
    float bat = map(analogRead(BATTERIE),0,4095,0,3300);
    DEBUG_PRINTF("getBatterie=%f\n",bat);
    bat /= 1000;
#ifdef TST
    return 3.3;
#endif
    return bat==0.0?3.3:bat;
}

// Returns list of files in LittleFS of ESP32
String getFilelist() {
  DEBUG_PRINTF("getFileList 1 %ld\n",millis());
  if(!buildList())
    return "";
  DEBUG_PRINTLN("got file list");
  String result="<br>Verwendet:" + formatBytes(usedBytes) + "<br>";
  result += "<br>Gesamt: " + formatBytes(totalBytes) + "<br>";
  result += "<br>Frei: " + formatBytes(freeBytes) + "<br>";
  DEBUG_PRINTLN("add files to website");
  for (auto& t : dirList) {
    result += "<br>Ordner: " +  get<0>(t) + " Name: " + get<1>(t) + " Größe: " + formatBytes(get<2>(t)) + "<br>";
  }
   DEBUG_PRINTF("getFileList 2 %ld\n",millis());
  return result;
}



// Callback-Funktion für WiFi-Events
void WiFiEvent(WiFiEvent_t event) {
   DEBUG_PRINTF("[WiFiEvent] Event: %d\n", event);
  switch (event) {
    case ARDUINO_EVENT_WIFI_AP_STACONNECTED:
      DEBUG_PRINTLN("Ein Client hat sich verbunden!");
      DEBUG_PRINTF("Aktuelle Anzahl der Clients: %d\n", WiFi.softAPgetStationNum());
      isConnected=true;
      break;
    case ARDUINO_EVENT_WIFI_AP_STAIPASSIGNED:
      DEBUG_PRINTLN("Ein Client hat sich identifiziert.");
      DEBUG_PRINTF("Aktuelle Anzahl der Clients: %d\n", WiFi.softAPgetStationNum());
      break;
    case ARDUINO_EVENT_WIFI_AP_STADISCONNECTED:
      DEBUG_PRINTLN("Ein Client hat sich getrennt!");
      DEBUG_PRINTF("Aktuelle Anzahl der Clients: %d\n", WiFi.softAPgetStationNum());
      isConnected=false;
      break;
    case ARDUINO_EVENT_WIFI_AP_STOP:
      DEBUG_PRINTLN("Accesspoint beendet!");
      break;
    default:
      DEBUG_PRINTF("[WiFiEvent] Event: %d\n", event);
      break;
  }
}

void doSetFileFilter() {
  DEBUG_PRINTF("doSetFileFilter size=%d\n",dirList.size());
  // Select sound file
  fileFilter[0]='\0';
  if(counter<dirList.size()) {
    std::list<records>::iterator it = dirList.begin();
    advance(it, counter);
    strcpy(fileFilter,(get<1>(*it)).c_str());
    DEBUG_PRINTF("counter=%d, fileFilter=%s\n",counter,fileFilter);
    if(++counter>=dirList.size()-2)
      counter=0;
  }
  source.setFileFilter(fileFilter);
}

void doPlaySound() {
    DEBUG_PRINTLN("doPlaySound");
    // setup output
    auto cfg = i2s.defaultConfig(TX_MODE);
    cfg.pin_bck = BCLK;
    cfg.pin_data = DIN;
    cfg.pin_ws = LRC;
    cfg.channels = 1;
    i2s.begin(cfg);

    // setup player
    //player.setMetadataCallback(printMetaData);
    //AudioLogger::instance().begin(Serial, AudioLogger::Info);

    player.setVolume(getVolume());

    DEBUG_PRINTLN("Player begin");

    // Play sound
    player.begin();
    while (player.isActive()) { 
        player.setVolume(getVolume());
        player.copy(); 
    };
    player.end();

    DEBUG_PRINTF("Player end, dly=%d\n", dly);

    delay(dly);

    DEBUG_PRINTLN("Prepare_ulp");

    rtc_gpio_init((gpio_num_t)BEWEGUNG);
    rtc_gpio_set_direction((gpio_num_t)4, (rtc_gpio_mode_t)0);
    prepare_ulp();
    esp_sleep_enable_ulp_wakeup();
    hulp_peripherals_on();
    DEBUG_PRINTLN("starting deep sleep");

    esp_deep_sleep_start();
}

// Initial Setup
void setup()
{
  delay(500);
  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0); //disable brownout detector   

  Serial.begin(115200);

  delay(500);

  DEBUG_PRINTLN("\nStarting...\n\n");
  
  initFS();
  buildList();

  // Analog PINs for Volume and Battery setting
  pinMode(VOLUME,INPUT);
  pinMode(BATTERIE,INPUT);

  if(getBatterie() < 3.0) { // if battery less than 3V play Sound "Battery almost empty"
    strcpy(fileFilter,"_Battery*");
    dly=11000;
  } else {
    doSetFileFilter();
    dly=0;
  }
  bool wakeup = hulp_is_deep_sleep_wakeup() || hulp_is_ulp_wakeup();

  DEBUG_PRINTF("wakeup=%d, counter=%d, dly=%d, fileFilter=%s\n",wakeup,counter,dly,fileFilter);

  if (!wakeup) {  // if initial start (not wakeup) open Access Point
    DEBUG_PRINTLN("Not wakeup");
    counter=0;
    isConnected=false;
    WiFi.mode(WIFI_AP);
    WiFi.softAP(APNAME);
    WiFi.onEvent(WiFiEvent);
    //dnsServer.start(53, "*", WiFi.softAPIP());

    delay(500);

    DEBUG_PRINTF("status=%d\n",WiFi.status());

    setupFS(); 

    DEBUG_PRINTLN("setupFS finished");
    DEBUG_FLUSH();
    // Start the server
    server.begin();
    if (!MDNS.begin(APNAME)) { // APNAME ist der Hostname
        Serial.println("mDNS konnte nicht gestartet werden");
        return;
    }
    Serial.println("mDNS gestartet. Zugang über: http://klangbox.local");

    // Optionale mDNS-Dienste registrieren (z. B. HTTP)
    MDNS.addService("http", "tcp", 80);

    DEBUG_PRINTLN("HTTP server gestarted");
    currentMillis=millis();

  } else // wakeup called
  {
    DEBUG_PRINTLN("Woken up!");
    doPlaySound();
  }
}


void loop()
{
  // DEBUG_PRINTLN("loop");
  unsigned long ms = millis() - currentMillis;
  if(!isConnected && ms >= LOOPCNTMAX) {
    server.stop();
    WiFi.disconnect();    // Disconnect WiFi
    WiFi.mode(WIFI_OFF);  // turnoff WiFi
    DEBUG_PRINTLN("HTTP server stopped, wifi stopped, deep sleep starting");
    doSetFileFilter();
    doPlaySound();
  }
  server.handleClient();
}