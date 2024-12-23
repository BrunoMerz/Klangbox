#include <Arduino.h>

//#define myDEBUG
#include "MyDebug.h"

#include "configuration.h"
#include "html_content.h"

#include <WiFi.h>
#include "WebServer.h"

#include <list>
#include <FS.h>
#include <LittleFS.h>
#include "Languages.h"

void initFS();
void handleFSExplorer();
void handleFSExplorerCSS();
void handleContent(const uint8_t * image, size_t size, const char * mime_type);
void sendResponse();
void handleUpload();
bool handleFile(String &&path);
void formatFS();
const String formatBytes(size_t const& bytes);

extern String processor(const String& var);

using namespace std;
using records = tuple<String, String, int>;

extern uint16_t codeline;
extern String codetab;
extern unsigned long currentMillis;
extern bool isConnected;
extern list<records> dirList;
extern size_t usedBytes;
extern size_t totalBytes;
extern size_t freeBytes;
extern WebServer server;

extern float getBatterie();
extern float getVolume();
extern String getFilelist();


String generateHTML(const char *index_html) {
  String html(index_html);
  html.replace("%VERSION%", "2.0");
  html.replace("%BATTERIE%", String(getBatterie()));
  html.replace("%VOLUME%", String(getVolume()));
  html.replace("%FILES%", getFilelist());
  return html;
}

void setupFS() {
  server.on("/fs", handleFSExplorer);
  server.on(F("/fsstyle.css"), handleFSExplorerCSS);

  server.on(F("/sanduhr"), []() {handleContent(p_bluespinner,sizeof(p_bluespinner),IMAGE_GIF);});

  server.on(F("/format"), formatFS);
  server.on(F("/upload"), HTTP_POST, sendResponse, handleUpload);

  server.on("/", []() {
       server.send(200, TEXT_HTML, generateHTML(index_html));
  });

  server.on("/start", HTTP_GET, []() {
      isConnected=false;
      currentMillis=millis()+LOOPCNTMAX;
      server.send(200, "text/ascii", "Klangbox gestartet");
  });

  server.onNotFound([]() {
  #if defined(WITH_ALEXA)
    if (!alexa->espalexa.handleAlexaApiCall(server.uri(),server.arg(0))) {
  #endif
    if (!handleFile(server.urlDecode(server.uri())))
      server.send(404, TEXT_PLAIN, F("FileNotFound"));
  #if defined(WITH_ALEXA)
    }
  #endif
  });

}


void initFS() {
  if ( !LittleFS.begin() ) 
  {
    DEBUG_PRINTLN(F("LittleFS Mount fehlgeschlagen"));
    if ( !LittleFS.format() )
    {
      DEBUG_PRINTLN(F("Formatierung nicht möglich"));
    }
    else
    {
      DEBUG_PRINTLN(F("Formatierung erfolgreich"));
      if ( !LittleFS.begin() )
      {
        DEBUG_PRINTLN(F("LittleFS Mount trotzdem fehlgeschlagen"));
      }
      else 
      {
        DEBUG_PRINTLN(F("LittleFS Dateisystems erfolgreich gemounted!")); 
      }
    }
  }  
  else
  {
    DEBUG_PRINTLN(F("LittleFS erfolgreich gemounted!"));
  }
}

void sortList() {
  DEBUG_PRINTLN("vor sort");
  dirList.sort([](const records & f, const records & l) { // Ordner sortieren
    if (get<0>(f)[0] != 0x00 || get<0>(l)[0] != 0x00) {
      for (uint8_t i = 0; i < 31; i++) {
          if (tolower(get<0>(f)[i]) < tolower(get<0>(l)[i])) return true;
          else if (tolower(get<0>(f)[i]) > tolower(get<0>(l)[i])) return false;
      }
    }
    return false;
  });
  DEBUG_PRINTLN("nach sort");
}


bool buildList() {
  if(dirList.size())
    return true;

  DEBUG_PRINTLN("Read Filesystem");
  usedBytes = LittleFS.usedBytes();
  totalBytes = LittleFS.totalBytes();
  freeBytes = totalBytes - usedBytes;
  File root = LittleFS.open("/");
  File dir = root.openNextFile();
  
  while(dir) {
    if(dir.isDirectory()) {
      
      uint8_t ran {0};
      DEBUG_PRINTF("buildList <%s>\n",dir.name());
      String fn="/"+String(dir.name());
      File root2 = LittleFS.open(fn);
      File fold = root2.openNextFile();
      while (fold)  {
        ran++;
        dirList.emplace_back(fn, fold.name(), fold.size());
        fold = root2.openNextFile();
      }
      if (!ran) dirList.emplace_back(fn, "", 0);
    }
    else {
      dirList.emplace_back("", dir.name(), dir.size());
    }
    dir = root.openNextFile();
  }
  
  sortList();
  return true;
}

bool handleList() {   
  DEBUG_PRINTF("handleList 1 %ld\n",millis());
  if(!buildList())
    return false;

  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  uint8_t jsonoutteil = 0;
  bool jsonoutfirst = true;
  String temp = F("[");
  for (auto& t : dirList) {
#if defined(ESP8266)
    if ( ESP.getMaxFreeBlockSize() < 1000) delay(10);
#endif
    if (temp != "[") temp += ',';
    temp += F("{\"folder\":\"");
    temp += get<0>(t);
    temp += F("\",\"name\":\"");
    temp += get<1>(t);
    temp += F("\",\"size\":\"");
    temp += formatBytes(get<2>(t));
    temp += F("\"}");
    jsonoutteil++;
    if ( jsonoutteil > 2 ) 
    {
      jsonoutteil = 0;
      if ( jsonoutfirst )
      {
        jsonoutfirst = false;
        server.send(200, F("application/json"), temp);
      }
      else
      {
        server.sendContent(temp);
      }
      DEBUG_PRINTLN(temp);
      temp = "";
      delay(0);
    } 
  }
  if(dirList.size())
    temp += F(",");
  temp += F("{\"usedBytes\":\"");
  temp += formatBytes(usedBytes);                      // Berechnet den verwendeten Speicherplatz
  temp += F("\",\"totalBytes\":\"");
  temp += formatBytes(totalBytes);                     // Zeigt die Größe des Speichers
  temp += F("\",\"freeBytes\":\""); 
  temp += formatBytes(freeBytes);             // Berechnet den freien Speicherplatz
  temp += F("\"}]");   

  server.sendContent(temp);
  server.sendContent("");

  DEBUG_PRINTLN(temp);

  temp = "";
  DEBUG_PRINTF("handleList 2 %ld\n",millis());
  return true;
}

void deleteDirectory(const char* path) {
  DEBUG_PRINTF("deleteDirectory <%s>\n",path);
  if (LittleFS.exists(path)) {
    File dir = LittleFS.open(path);
    
    if (dir.isDirectory()) {
      File file = dir.openNextFile();
      
      // Solange es Dateien gibt, iteriere durch sie
      while (file) {
        String filePath = String(path) + "/" + file.name();
        
        if (file.isDirectory()) {
          // Wenn es ein Unterverzeichnis ist, rekursiv löschen
          deleteDirectory(filePath.c_str());
        } else {
          // Wenn es eine Datei ist, löschen
          file.close();
          LittleFS.remove(filePath.c_str());
          DEBUG_PRINT("Datei gelöscht: ");
          DEBUG_PRINTLN(filePath);
        }
        
        file = dir.openNextFile();  // Nächste Datei
      }
      
      // Verzeichnis löschen
      LittleFS.rmdir(path);
      DEBUG_PRINT("Verzeichnis gelöscht: ");
      DEBUG_PRINTLN(path);
    }
  } else {
    DEBUG_PRINT("Verzeichnis existiert nicht: ");
    DEBUG_PRINTLN(path);
  }
}

void deleteFileOrDir(const String &path) {
  DEBUG_PRINTF("deleteFileOrDir <%s>\n",path.c_str());
  if (!LittleFS.remove(path))
    deleteDirectory(path.c_str());
  dirList.clear();
}

bool handleFile(String &&path) {
  DEBUG_PRINT (F("LittleFS handleFile: "));
  DEBUG_PRINTLN(path);
  if (path.endsWith("/")) path += F("index.html");
  if (path.endsWith(F("favicon.ico"))) path=F("/_favicon.ico");
  if ( LittleFS.exists(path) )
  {
    File f = LittleFS.open(path, "r");
    if(!f)
    {
      DEBUG_PRINT(F("Fehler beim lesen des Files: "));
      DEBUG_PRINTLN(path);
      return false;
    }
    else
    {
      String mime_type;
      if(path.endsWith(F(".htm")) || path.endsWith(F(".html")))
          mime_type = F("text/html");
      else if(path.endsWith(F(".jpg")) || path.endsWith(F(".jpeg")))
          mime_type = F("image/jpeg");
      else if(path.endsWith(".png"))
        mime_type = F("image/png");
      else if(path.endsWith(".ico"))
        mime_type = F("image/x-icon");
      else if(path.endsWith(".bmp"))
        mime_type = F("image/bmp");
      else if(path.endsWith(".gif"))
        mime_type = F("image/gif");
      else if(path.endsWith(".css"))
        mime_type = F("text/css");
      else if(path.endsWith(".pdf"))
        mime_type = F("application/pdf");
      else if(path.endsWith(".txt"))
        mime_type = F("text/plain");
      else if(path.endsWith(".json"))
        mime_type = F("application/json");
      else if(path.endsWith(".mp3"))
        mime_type = F("audio/mpeg");
      else if(path.endsWith(".js"))
        mime_type = F("text/javascript");
      else
        mime_type = F("application/octet-stream");

      DEBUG_PRINTLN(mime_type);

      server.streamFile(f, mime_type);
      f.close(); 
      return true;
    }
  }
  else
  {
    return false;
  }
}

// Dateien ins Filesystem schreiben
void handleUpload() { 
  static File fsUploadFile;
  HTTPUpload& upload = server.upload();
  if (upload.status == UPLOAD_FILE_START) {
    if (upload.filename.length() > 31) {  // Dateinamen kürzen
      upload.filename = upload.filename.substring(upload.filename.length() - 31, upload.filename.length());
    }
    fsUploadFile = LittleFS.open(server.arg(0) + "/" + server.urlDecode(upload.filename), "w");
  } else if (upload.status == UPLOAD_FILE_WRITE) {
     fsUploadFile.write(upload.buf, upload.currentSize);
  } else if (upload.status == UPLOAD_FILE_END) {
    fsUploadFile.close();
    dirList.clear();
  }

}

void formatFS() {                                                                      // Formatiert das Filesystem
  LittleFS.format();
  initFS();
  dirList.clear();
  server.sendHeader("Location", "/");
  server.send(303, "message/http");
}

void sendResponse() {
  server.sendHeader("Location", "fs");
  server.send(303, "message/http");
}

const String formatBytes(size_t const& bytes) {                                        // lesbare Anzeige der Speichergrößen
  return bytes < 1024 ? static_cast<String>(bytes) + " Byte" : bytes < 1048576 ? static_cast<String>(bytes / 1024.0) + "KB" : static_cast<String>(bytes / 1048576.0) + "MB";
}

// #####################################################################################################################

void handleFSExplorer() {

  String message;
  if (server.hasArg("new")) 
  {
    String folderName {server.arg("new")};
    for (auto& c : {34, 37, 38, 47, 58, 59, 92}) for (auto& e : folderName) if (e == c) e = 95;    // Ersetzen der nicht erlaubten Zeichen
    folderName = "/" + folderName;
    DEBUG_PRINTF("mkdir: %s\n",folderName);
    if(!LittleFS.mkdir(folderName)) {
      DEBUG_PRINTLN("mkdir error");
    } 
    dirList.clear();
    sendResponse();
  }
  else if (server.hasArg("sort")) 
  {
    bool x=handleList();
  }
  else if (server.hasArg("delete")) 
  {
    deleteFileOrDir(server.arg("delete"));
    sendResponse();
  }
  else
  {
    server.setContentLength(CONTENT_LENGTH_UNKNOWN);
    message =  F("<!DOCTYPE HTML>");
    message += F("<html lang=\"");
    message += LANG_HTMLLANG;
    message += F("\">\n");
    message += F("<head>");
    message += F("<meta charset=\"UTF-8\">");
    message += F("<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">");
    message += F("<link rel=\"stylesheet\" href=\"fsstyle.css\">");
    message += F("<title>");
    message += LANG_EXPLORER;
    message += F("</title>\n");
    message += F("<script>");
    message += F("const LANG_FORMATCONF = \"" LANG_FORMATCONF "\";\n");
    message += F("const LANG_NOSPACE = \"" LANG_NOSPACE "\";\n");
    message += F("const LANG_FILESIZE= \"" LANG_FILESIZE "\";\n");
    message += F("const LANG_SURE= \"" LANG_SURE "\";\n");
    message += F("const LANG_FSUSED= \"" LANG_FSUSED "\";\n");
    message += F("const LANG_FSTOTAL= \"" LANG_FSTOTAL "\";\n");
    message += F("const LANG_FSFREE= \"" LANG_FSFREE "\";\n");
    
    server.send(200, F(TEXT_HTML), message);
    delay(0);
    server.sendContent_P(fshtml1);
    delay(0);
    message = F("<h2>" LANG_EXPLORER "</h2>");
    server.sendContent(message);
    server.sendContent_P(fshtml2);
    delay(0);
    message = F("<input name=\"new\" placeholder=\"" LANG_FOLDER "\"");    
    //message += F(" pattern=\"[^\x22/%&\\:;]{1,31}\" title=\"");
    message += F(" title=\"");
    message += F(LANG_CHAR_NOT_ALLOW);
    message += F("\" required=\"\">");
    message += F("<button>Create</button>\n");
    message += F("</form>");
    message += F("<main></main>\n");
    message += F("<form action=\"/format\" method=\"POST\">\n");            
    message += F("<button class=\"buttons\" title=\"" LANG_BACK "\" id=\"back\" type=\"button\" onclick=\"window.location.href=\'\\/\'\">&#128281; " LANG_BACK "</button>\n");
    message += F("<button class=\"buttons\" title=\"Format LittleFS\" id=\"btn\" type=\"submit\" value=\"Format LittleFS\">&#10060;Format LittleFS</button>\n");
    message += F("</form>\n");
    message += F("</body>\n");
    message += F("</html>\n");
    server.sendContent(message);
    message = "";
    delay(0);
    server.sendContent("");
    delay(0);
  }

}

void handleFSExplorerCSS() {

  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  delay(0);
  server.send_P(200, TEXT_CSS, fsstyle1);
  delay(0);
  server.sendContent_P(fsstyle2);
  delay(0);
  server.sendContent("");  
  delay(0);

}


void handleContent(const uint8_t * image, size_t size, const char * mime_type) {
  uint8_t buffer[512];
  size_t buffer_size = sizeof(buffer);
  size_t sent_size = 0;

  server.setContentLength(size);
  server.send(200, mime_type, "");

  while (sent_size < size) {
    size_t chunk_size = min(buffer_size, size - sent_size);
    memcpy_P(buffer, image + sent_size, chunk_size);
    server.client().write(buffer, chunk_size);
    sent_size += chunk_size;
    delay(0);

    //DEBUG_PRINTF("sendContent: %i byte : %i byte of %i byte\n", chunk_size, sent_size,size );

  }
  server.sendContent("");
  delay(0);
}
