/*
 * Lotus ABS ECU Tool
 * v1.0 (c) 2021 by CyberNet - original author
 * v1.1 (c) 2026 Updates and Bug Fixes by Rstone72
 *
 * >> Init Wifi, Main Loop, Handle Websockets <<
 *
 * v1.1: K-line on the hardware UART, debug log page (/log.html, /log.txt, /status)
 *       stall watchdog: if loop() stops running for 30 s the module restarts by itself and
 *       shows the log lines from before the stall; valves/pump switched on from the valve page
 *       are switched off again when that page is closed
 */

#include <Arduino.h>
#include <FS.h>     
#include <TaskScheduler.h>
#include <stdint.h>
#include <LittleFS.h>
#include <ElegantOTA.h>
#include <Ticker.h>

#include "web.h"
#include "kwp.h"
#include "mqtt.h"
#include "log.h"
#include "ebc430.h"

#define WIFI_AP_SSID  "LOTUS-ABS"
#define WIFI_AP_PASS  ""
const byte DNS_PORT = 53;
IPAddress apIP(192, 168, 1, 1);
DNSServer dnsServer;

//#define WIFI_DEBUG 0   // enable this and set WIFI_JOIN_SSID and WIFI_JOIN_PW to join an AP (handy for debugging)
#ifdef WIFI_DEBUG
 #define WIFI_JOIN_SSID "cloud9"
 #define WIFI_JOIN_PW ""
#endif

#define TX 1
#define RX 3
#define LED_OK  16
KWP2K ABS(TX, RX);


ESP8266WebServer server(80);
WebSocketsServer ws_server = WebSocketsServer(81);

Scheduler Taskrunner;
void OKLEDTaskCallback();
Task OKLEDTask(1000, -1, &OKLEDTaskCallback);

bool state_ok_led = true;

// ---- stall watchdog ----
// loop() must come round regularly. A wait that never ends but keeps calling yield() is not caught by
// the ESP8266's own watchdogs: Wi-Fi stays up but the web pages are dead until power-off.
#define STALL_LIMIT_MS  30000
#define STALL_MAGIC     0x4C414253          // "LABS"
struct StallRecord { uint32_t magic; uint32_t uptime_s; char tail[368]; };   // kept in RTC memory over the restart
#define STALL_RTC_OFFSET 32                 // 4-byte blocks: skip the first 128 bytes, used by OTA (eboot)
static volatile uint32_t loop_alive_ms = 0;
static Ticker stall_ticker;

static void stall_check()
{
  if (millis() - loop_alive_ms > STALL_LIMIT_MS)
  {
    static StallRecord rec;
    rec.magic = STALL_MAGIC;
    rec.uptime_s = millis()/1000;
    log_tail(rec.tail, sizeof(rec.tail));
    ESP.rtcUserMemoryWrite(STALL_RTC_OFFSET, (uint32_t *)&rec, sizeof(rec));
    ESP.reset();
  }
}

static void stall_report_previous()
{
  static StallRecord rec;
  if (!ESP.rtcUserMemoryRead(STALL_RTC_OFFSET, (uint32_t *)&rec, sizeof(rec)) || rec.magic != STALL_MAGIC) return;
  rec.tail[sizeof(rec.tail)-1] = 0;
  log_line("!! previous run froze after %lus and was restarted by the stall watchdog. Its last log lines:", (unsigned long)rec.uptime_s);
  const char *t = strchr(rec.tail, '\n');               // start at a whole line
  log_msg("%s", t ? t+1 : rec.tail);
  log_line("!! end of the log from before the restart");
  rec.magic = 0;
  ESP.rtcUserMemoryWrite(STALL_RTC_OFFSET, (uint32_t *)&rec, sizeof(rec));
}

// ---- valves / pump switched on from the valve page ----
#define IO_PUMP_MOTOR   0x02
#define IO_FIRST_VALVE  0x11
#define IO_LAST_VALVE   0x42
static uint8_t io_on[12];
static uint8_t io_on_n = 0;

// only the IO ids the valve page uses: lamp (read), pump, relay, the 8 valves, all-valves (read)
static bool io_id_known(int id, int ctrl_param)
{
  if (ctrl_param != 0x01 && ctrl_param != 0x07) return false;
  bool valve = (id >= IO_FIRST_VALVE && id <= IO_LAST_VALVE && (id & 0x0F) >= 1 && (id & 0x0F) <= 2);
  if (ctrl_param == 0x07) return (id == IO_PUMP_MOTOR || id == 0x03 || valve);
  return (id == 0x01 || id == IO_PUMP_MOTOR || id == 0x03 || id == 0x49 || valve);
}

static void io_note(uint8_t id, uint8_t ctrl_param, uint8_t ctrl_state)
{
  if (ctrl_param != 0x07) return;                                         // only writes
  if (!(id == IO_PUMP_MOTOR || (id >= IO_FIRST_VALVE && id <= IO_LAST_VALVE))) return;   // relay is left alone
  uint8_t i;
  for (i=0; i<io_on_n && io_on[i]!=id; i++);
  if (ctrl_state && i == io_on_n && io_on_n < sizeof(io_on)) io_on[io_on_n++] = id;
  if (!ctrl_state && i < io_on_n) io_on[i] = io_on[--io_on_n];
}

// a page was closed: anything it left switched on goes off
static void io_all_off()
{
  for (uint8_t i=0; i<io_on_n; i++)
  {
    log_line("WS: page closed, switching %02X off", io_on[i]);
    ABS.InputOutputControlByLocalId(io_on[i], 0x07, 0);
  }
  io_on_n = 0;
}

// uint8_t ecu_addr_glob = 0x1;

void webSocketEvent(uint8_t num, WStype_t type, uint8_t * payload, size_t length) 
{
    DynamicJsonDocument json(256);
    switch(type) {
        case WStype_DISCONNECTED:
#ifdef DBG_WEBSOCKETS                    
            log_msg("webSocketEvent() [%u] Disconnected!\n", num);
#endif            
            log_line("WS: page disconnected (#%u, %d still open)", num, ws_server.connectedClients());
            io_all_off();
            break;
        case WStype_CONNECTED:
            {
                IPAddress ip = ws_server.remoteIP(num);
                log_line("WS: page connected (#%u, %d open, free heap %u)", num, ws_server.connectedClients(), ESP.getFreeHeap());
#ifdef DBG_WEBSOCKETS                            
                log_msg("webSocketEvent() [%u] Connected from %d.%d.%d.%d url: %s\n", num, ip[0], ip[1], ip[2], ip[3], payload);
#endif                
      //          ws_server.sendTXT(num, "Connected");
            }
            break;
        case WStype_TEXT:        
#ifdef DBG_WEBSOCKETS                    
            log_msg("webSocketEvent() [%u] get Text: %s\n", num, payload);
#endif            
            if (deserializeJson(json, payload, length))
            {
                log_line("WS: bad request ignored");
                break;
            }
#ifdef DBG_WEBSOCKETS            
            log_msg("\t Calling SId: %02x", json["sid"].as<int>());     
#endif            
            switch (json["sid"].as<int>())          
            {
                case KWP_SID_STARTCOMMUNICATION:                  ABS.StartCommunication(); break;
                case KWP_SID_READECUIDENTIFICATION:               ABS.ReadECUIdentification(json["option"].as<int>()); break;
                case KWP_SID_READDIAGNOSTICTROUBLECODESBYSTATUS:  ABS.ReadDiagnosticTroubleCodesByStatus(); break;
                case KWP_SID_CLEARDIAGNOSTICINFORMATIONSERVICE:   ABS.ClearDiagnosticInformationService(); break;
                case KWP_SID_READDATABYLOCALID:                   ABS.ReadDataByLocalId(json["record"].as<int>(), json["dtc"].as<int>()); break;
                case KWP_SID_INPUTOUTPUTCONTROLBYLOCALID:         if (!io_id_known(json["id"].as<int>(), json["ctrl_param"].as<int>())) { log_line("WS: IO id %02X / param %02X refused", json["id"].as<int>(), json["ctrl_param"].as<int>()); break; }
                                                                  io_note(json["id"].as<int>(), json["ctrl_param"].as<int>(), json["ctrl_state"].as<int>());
                                                                  ABS.InputOutputControlByLocalId(json["id"].as<int>(), json["ctrl_param"].as<int>(), json["ctrl_state"].as<int>()); break;
                default:
#ifdef DBG_WEBSOCKETS                            
                          log_msg("\n unknown SiD, no function defined ?");
#endif                           
                          break;
            }
            break;        
    }

}

void setup() {

  // Taskrunner
  Taskrunner.init();
  Taskrunner.addTask(OKLEDTask);
  
  // Setup K-line (hardware UART0, GPIO1/GPIO3)
  ABS.begin();
  log_line("LotusABS firmware v%s (build %s), reset reason: %s", FW_VERSION, __DATE__, ESP.getResetReason().c_str());
  stall_report_previous();
  
  // SETUP WiFI (Normal AP Mode)
#ifndef WIFI_DEBUG   
  WiFi.mode(WIFI_AP);
  WiFi.softAPConfig(apIP, apIP, IPAddress(255, 255, 255, 0));
  boolean state = WiFi.softAP(WIFI_AP_SSID,WIFI_AP_PASS);
  while (!state) {
    log_line("Creating access point failed");
    // don't continue - WD will kill us
    while (true);
  }
  dnsServer.start(DNS_PORT, "*", apIP);  
#else
  // (Debug join AP Mode)
  WiFi.mode(WIFI_STA);  
  WiFi.begin("CLOUD9", "deepspace48");  
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.printf("\n===================================\nLotusABS IP address is: %d.%d.%d.%d\n", WiFi.localIP()[0], WiFi.localIP()[1], WiFi.localIP()[2], WiFi.localIP()[3]);  
#endif

  mqtt_setup();
  log_msg("\n===================================\nLotusABS IP address is: %d.%d.%d.%d\n", WiFi.localIP()[0], WiFi.localIP()[1], WiFi.localIP()[2], WiFi.localIP()[3]);
  
  // SETUP WWW
  if (!LittleFS.begin())
  {
    log_line("unable to init LittleFS");
  }
  server.serveStatic("/", LittleFS, "/www/index.html");
  server.serveStatic("/valves.html", LittleFS, "/www/valves.html");
  server.serveStatic("/dtc.html", LittleFS, "/www/dtc.html");
  server.serveStatic("/log.html", LittleFS, "/www/log.html");
  server.serveStatic("/disclaimer.html", LittleFS, "/www/disclaimer.html");

  // debug log: text written after position ?from=N, first line = new position
  server.on("/log.txt", HTTP_GET, []() {
      uint32_t from = 0;
      if (server.hasArg("from")) from = strtoul(server.arg("from").c_str(), NULL, 10);
      String body = String(log_total()) + "\n" + log_read_since(from);
      server.sendHeader("Cache-Control", "no-store");
      server.send(200, "text/plain", body);
  });

  // module and K-line status for the log page
  server.on("/status", HTTP_GET, []() {
      DynamicJsonDocument doc(512);
      doc["version"] = FW_VERSION;
      doc["build"] = __DATE__;
      doc["uptime_s"] = millis()/1000;
      doc["free_heap"] = ESP.getFreeHeap();
      doc["heap_frag"] = ESP.getHeapFragmentation();
      doc["reset_reason"] = ESP.getResetReason();
      doc["ws_clients"] = ws_server.connectedClients();
      doc["tx"] = kwp_stats.tx;
      doc["rx_ok"] = kwp_stats.rx_ok;
      doc["checksum_errors"] = kwp_stats.cs_err;
      doc["timeouts"] = kwp_stats.timeouts;
      doc["echo_errors"] = kwp_stats.echo_err;
      doc["resyncs"] = kwp_stats.resync;
      doc["retries"] = kwp_stats.retries;
      doc["fast_inits"] = kwp_stats.inits;
      doc["dropped"] = kwp_stats.dropped;
      String out;
      serializeJson(doc, out);
      server.sendHeader("Cache-Control", "no-store");
      server.send(200, "application/json", out);
  });
  server.serveStatic("/js", LittleFS, "/www/js");
  server.serveStatic("/img", LittleFS, "/www/img");
  server.serveStatic("/css", LittleFS, "/www/css");

  server.on("/ReadECUIdentification", HTTP_GET, []() {            
      uint8_t option = EBC_OPT_ALL;
      if (server.hasArg("option"))
      {
          option = server.arg("option").toInt();                                       
         
      }  
      ABS.ReadECUIdentification(option);          
      server.send(200, "application/json", "{ \"status\" : true }");
  });


  // AsyncElegantOTA.begin(&server);
  ElegantOTA.begin(&server); 
  Update.onProgress([](size_t done, size_t total) { loop_alive_ms = millis(); });   // a long OTA upload is not a stall
  server.begin();

  // register ABS ECU callbacks
  ABS.setTimeoutCallback(&TimeoutCB);
  ABS.setCallback(KWP_SID_ERR, &ErrorMessageCB);
  ABS.setCallback(KWP_SID_STARTCOMMUNICATION, &StartCommunicationCB);
  ABS.setCallback(KWP_SID_READECUIDENTIFICATION, &ReadECUIdentificationCB);
  ABS.setCallback(KWP_SID_READDATABYLOCALID, &ReadDataByLocalIdCB);
  ABS.setCallback(KWP_SID_READDIAGNOSTICTROUBLECODESBYSTATUS, &ReadDiagnosticTroubleCodeByStatusCB);
  ABS.setCallback(KWP_SID_CLEARDIAGNOSTICINFORMATIONSERVICE, &ClearDiagnosticInformationServiceCB);
  ABS.setCallback(KWP_SID_INPUTOUTPUTCONTROLBYLOCALID, &InputOutputControlByLocalIdCB);

  // SETUP Pins
  pinMode(LED_OK, OUTPUT);  
  
  // enable Tasks
  OKLEDTask.enable();  

  // enable webSockets
    ws_server.begin();
    ws_server.onEvent(webSocketEvent);
    // ping every page every 15 s; a page that does not answer twice (phone asleep, out of range, tab
    // killed) is dropped, so its slot is free again (the module takes 5 pages at most)
    ws_server.enableHeartbeat(15000, 3000, 2);

  loop_alive_ms = millis();
  stall_ticker.attach_ms(1000, stall_check);
}

// not that useful, but everybody likes blinkenlights ;)
// might use that for something else one day
void OKLEDTaskCallback()
{  
  digitalWrite(LED_OK, state_ok_led);
  state_ok_led = !state_ok_led;
}

void loop() {
  loop_alive_ms = millis();

  mqtt_loop();
  ABS.setKeepAlive(ws_server.connectedClients() > 0);
  ABS.process(); 
  Taskrunner.execute();
  server.handleClient();
  ws_server.loop();
#ifndef WIFI_DEBUG    
  dnsServer.processNextRequest();
#endif
}

