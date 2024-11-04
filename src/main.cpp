#include "Arduino.h"
#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <DNSServer.h>

#include <ElegantOTA.h>
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

//#define TST

// GPIO PINs
#define BEWEGUNG GPIO_NUM_4
#define ULP_WAKEUP_INTERVAL_MS 100
#define VOLUME 34
#define BATTERIE 35

// Audio Output
const char *startFilePath = "/";
const char *ext = "mp3";
AudioSourceLittleFS source(startFilePath, ext);
I2SStream i2s;
MP3DecoderHelix decoder;
AudioPlayer player(source, i2s, decoder);
char fileFilter[10];

// OTA Variables
bool otaEnd;
bool otaSuccess=false;
bool otaStarted;

int dly=0;
RTC_DATA_ATTR int counter;

// Webserver
DNSServer dnsServer;
AsyncWebServer server(80);

// WEB Page
const char index_html[] PROGMEM = R"rawliteral(
<!DOCTYPE HTML><html>
<head>
  <title>Zwitscherbox</title>
  <meta name="viewport" content="width=device-width, initial-scale=1", charset="UTF-8">
  <link rel="icon" href="data:,">
  <style>
  html {
    font-family: Arial, Helvetica, sans-serif;
    text-align: center;
  }
  h1 {
    font-size: 1.8rem;
    color: white;
  }
  h2{
    font-size: 1.5rem;
    font-weight: bold;
    color: #143642;
  }
  .topnav {
    overflow: hidden;
    background-color: #143642;
  }
  body {
    margin: 0;
  }
  .content {
    padding: 30px;
    max-width: 600px;
    margin: 0 auto;
  }
  .card {
    background-color: #F8F7F9;;
    box-shadow: 2px 2px 12px 1px rgba(140,140,140,.5);
    padding-top:10px;
    padding-bottom:20px;
  }
  .button {
    padding: 15px 50px;
    font-size: 24px;
    text-align: center;
    outline: none;
    color: #fff;
    background-color: #0f8b8d;
    border: none;
    border-radius: 5px;
    -webkit-touch-callout: none;
    -webkit-user-select: none;
    -khtml-user-select: none;
    -moz-user-select: none;
    -ms-user-select: none;
    user-select: none;
    -webkit-tap-highlight-color: rgba(0,0,0,0);
   }
   /*.button:hover {background-color: #0f8b8d}*/
   .button:active {
     background-color: #0f8b8d;
     box-shadow: 2 2px #CDCDCD;
     transform: translateY(2px);
   }
   .info {
     font-size: 1rem;
     color:#8c8c8c;
     font-weight: bold;
     text-align: left;
   }
  </style>
<title>Zwitscherbox</title>
<meta name="viewport" content="width=device-width, initial-scale=1">
<link rel="icon" href="data:,">
</head>
<body>
  <div class="topnav">
    <h1>Zwitscherbox</h1>
  </div>
  <div class="content">
    <div class="card">
      <h2>Info</h2>
      <p class="info">Version: <span id="version">%VERSION%</span></p>
      <p class="info">Batterie: <span id="batterie">%BATTERIE%</span></p>
      <p class="info">Lautstärke: <span id="lautstärke">%VOLUME%</span></p>
      <h2>Dateisystem</h2>
      <p class="info"><span id="files">%FILES%</span></p>
      <p <input class="button" onclick="parent.location='/update'" />Aktualisieren</p>
      <p <input class="button" onclick="parent.location='/start'" />Starten</p>
    </div>
  </div>
</body>
</html>)rawliteral";

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
    return vol;
}

// Check Battery, Map Analog Read 0-4095 (0-3.3V) to 0-3.3
float getBatterie() {
    float bat = map(analogRead(BATTERIE),0,4095,0,3300);
    bat /= 1000;
#ifdef TST
    return 3.3;
#endif
    return bat;
}

// Returns list of files in LittleFS of ESP32
String getFilelist() {
  LittleFS.begin();
  String result="<br>Verwendet:" + String(LittleFS.usedBytes()) + "<br>";
  result += "<br>Total: " + String(LittleFS.totalBytes()) + "<br>";
  File root = LittleFS.open("/");
  if (!root)
  {
    Serial.println("Failed to open directory");
    return "";
  }

  if (!root.isDirectory())
  {
    Serial.println("Not a directory");
    return "";
  }

  File file = root.openNextFile();
  while (file)
  {
    if (file.isDirectory())
    {
      Serial.println("DIR : ");
      Serial.println(file.name());
      result += "<br>Ordner: " + String(file.name()) + "<br>";
    }
    else
    {
      Serial.println("FILE: ");
      Serial.println(file.name());
      result += "<br>Datei: " + String(file.name()) + "<br>";
    }
    
    file = root.openNextFile();
  }
  return result;
}

// Function gets called due to fill variables in WEB page
String processor(const String& var){
  Serial.println(var);
  if(var == "VERSION"){
    return "1.7";
  }
  if(var == "BATTERIE"){
    return String(getBatterie());
  }
  if(var == "VOLUME"){
    return String(getVolume());
  }
  if(var == "FILES") {
    return getFilelist();
  }
  return String();
}

// Initial Setup
void setup()
{
  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0); //disable brownout detector   

#ifdef TST
  Serial.begin(115200);
  while (!Serial)
    delay(100);
  Serial.println("\nStarting...\n\n");
  
  if(0) {
    pinMode(BEWEGUNG,INPUT);
    while(true) {
      Serial.printf("BEWEGUNG=%d\n",digitalRead(BEWEGUNG));
      delay(300);
    }
  }
#endif
  // Analog PINs for Volume and Battery setting
  pinMode(VOLUME,INPUT);
  pinMode(BATTERIE,INPUT);

  if(getBatterie() < 3.0) { // if battery less than 3V play Sound "Battery almost empty"
    strcpy(fileFilter,"B*");
    dly=11000;
  } else {
    // Select between to different sound files
    if(counter) {
      strcpy(fileFilter,"Z01*");
      counter = 0;
    } else {
      strcpy(fileFilter,"Z02*");
      counter = 1;
    }
    source.setFileFilter(fileFilter);
    dly=0;
  }
  bool wakeup = hulp_is_deep_sleep_wakeup() || hulp_is_ulp_wakeup();
#ifdef TST
  Serial.printf("wakeup=%d, counter=%d, dly=%d, fileFilter=%s\n",wakeup,counter,dly,fileFilter);
#endif
  if (!wakeup) {  // if initial start (not wakeup) open Access Point
    counter=0;
    WiFi.mode(WIFI_AP);
    WiFi.softAP("Klangbox");
    dnsServer.start(53, "*", WiFi.softAPIP());

    delay(500);
#ifdef TST
    Serial.printf("status=%d\n",WiFi.status());
#endif
    server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
        otaStarted = true;
        request->send(200, "text/html", index_html, processor);
    });
    server.on("/start", HTTP_GET, [](AsyncWebServerRequest *request) {
      otaStarted = false;
      otaEnd = true;
      request->send(200, "text/ascii", "Zwitscherbox gestartet");
    });
      
    ElegantOTA.onStart([]() {
      // Handle OTA updates
#ifdef TST
    Serial.println("OTA update process started.");
#endif
      otaEnd = false;
      otaSuccess = false;
      otaStarted = true;
    });

    ElegantOTA.onEnd([](bool success) {
#ifdef TST
    Serial.println("OTA update process ended.");
#endif
      otaEnd = true;
      otaSuccess = success;
      otaStarted = false;
    });

    ElegantOTA.begin(&server);    // Start ElegantOTA
      
    // Start the server
    server.begin();
#ifdef TST
    Serial.println("HTTP server gestarted");
#endif
    // Keep Access point open for 30 seconds or longer if OTA is in progress
    int i=0;
    while(otaStarted || (i++ < 3000 && !otaEnd)) {
      dnsServer.processNextRequest();
      delay(10);
    }
#ifdef TST
    Serial.printf("i=%d, otaEnd=%d, otaSuccess=%d\n",i, otaEnd, otaSuccess);
#endif
    if(otaSuccess) {
#ifdef TST
      Serial.println("rebooting");
#endif
      ESP.restart();
    }
    server.end();
    WiFi.disconnect();    // Disconnect WiFi
    WiFi.mode(WIFI_OFF);  // turnoff WiFi
#ifdef TST
    Serial.println("HTTP server stopped, wifi stopped, deep sleep starting");
#endif
  } else
  {
#ifdef TST
    Serial.printf("Woken up!\n");
#endif
    // setup output
    auto cfg = i2s.defaultConfig(TX_MODE);
    cfg.pin_bck = 27;
    cfg.pin_data = 25;
    cfg.pin_ws = 26;
    cfg.channels = 1;
    i2s.begin(cfg);

    // setup player
#ifdef TST
    player.setMetadataCallback(printMetaData);
    AudioLogger::instance().begin(Serial, AudioLogger::Info);
#endif
    player.setVolume(getVolume());
#ifdef TST
    Serial.println("Player begin");
#endif
    // Play sound
    player.begin();
    while (player.isActive()) { 
        player.setVolume(getVolume());
        player.copy(); 
    };
    player.end();
#ifdef TST
    Serial.printf("Player end, dly=%d\n", dly);
#endif
    delay(dly);
  }

#ifdef TST
  Serial.printf("Prepare_ulp\n");
#endif
  rtc_gpio_init((gpio_num_t)4);
  rtc_gpio_set_direction((gpio_num_t)4, (rtc_gpio_mode_t)0);
  prepare_ulp();
  esp_sleep_enable_ulp_wakeup();
  hulp_peripherals_on();
#ifdef TST
  Serial.printf("starting deep sleep\n");
#endif
  esp_deep_sleep_start();
  return;
}

void loop()
{
  // nothing todo in loop
}