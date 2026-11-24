// LotusABS full-stack harness: the real firmware (main.cpp, kwp.cpp, ebc430.cpp, log.cpp) built for a PC,
// talking to a simulated EBC430 over a simulated K-line, with the web server / websocket side bridged
// to bridge.py over stdin/stdout so a real browser can use the real web pages.
#include "Arduino.h"
#include "ESP8266WebServer.h"
#include "WebSocketsServer.h"
#include "ElegantOTA.h"
#include "Ticker.h"
#include "LittleFS.h"
#include "ESP8266WiFi.h"
#include <chrono>
#include <thread>
#include <mutex>
#include <deque>
#include <vector>
#include <map>
#include <set>
#include <string>
#include <iostream>
#include <sstream>
#include <atomic>
#include <unistd.h>

void setup(); void loop();
void mqtt_loop() {} void mqtt_log(const char *, const char *) {} bool mqtt_setup() { return false; }

const String emptyString;
HardwareSerial Serial; EspClass ESP; LittleFSClass LittleFS; WiFiClass WiFi; ElegantOtaClass ElegantOTA; UpdaterClass Update;

static std::mutex out_mx;
static void out(const std::string &s) { std::lock_guard<std::mutex> g(out_mx); fputs(s.c_str(), stdout); fputc('\n', stdout); fflush(stdout); }
static std::string hexenc(const std::string &s) { static const char *h = "0123456789abcdef"; std::string o; for (unsigned char c : s) { o += h[c >> 4]; o += h[c & 15]; } return o; }
static std::string hexdec(const std::string &s) { std::string o; for (size_t i = 0; i + 1 < s.size(); i += 2) o += (char)strtol(s.substr(i, 2).c_str(), 0, 16); return o; }

// ---------------- time ----------------
static auto T0 = std::chrono::steady_clock::now();
uint32_t millis() { return (uint32_t)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - T0).count(); }
static std::atomic<uint32_t> yields_since_loop{0};
void ecu_tick();
void delay(uint32_t ms) { uint32_t t = millis(); while (millis() - t < ms) { ecu_tick(); std::this_thread::sleep_for(std::chrono::microseconds(200)); } }
static std::atomic<bool> hang{false};
void yield() { ecu_tick(); std::this_thread::sleep_for(std::chrono::microseconds(50)); }

// ---------------- ESP ----------------
uint32_t EspClass::getFreeHeap() { return 40000; }
static std::string reset_reason = "External System";
String EspClass::getResetReason() { return String(reset_reason.c_str()); }
static uint8_t rtc[512]; static std::string rtc_file;
bool EspClass::rtcUserMemoryRead(uint32_t off, uint32_t *d, size_t n) { if (off * 4 + n > 512) return false; memcpy(d, rtc + off * 4, n); return true; }
bool EspClass::rtcUserMemoryWrite(uint32_t off, uint32_t *d, size_t n) { if (off * 4 + n > 512) return false; memcpy(rtc + off * 4, d, n); return true; }
void EspClass::reset() { FILE *f = fopen(rtc_file.c_str(), "wb"); if (f) { fwrite(rtc, 1, 512, f); fclose(f); } out("RESET watchdog"); _exit(42); }
void EspClass::restart() { out("RESET restart"); _exit(43); }

// ---------------- Ticker (runs in its own thread, like the SDK timer) ----------------
void Ticker::attach_ms(uint32_t ms, void (*cb)()) { std::thread([ms, cb] { while (true) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); cb(); } }).detach(); }

// ---------------- bridge input ----------------
static std::mutex in_mx; static std::deque<std::string> inq;
static void reader() { std::string l; while (std::getline(std::cin, l)) { std::lock_guard<std::mutex> g(in_mx); inq.push_back(l); } _exit(0); }

struct WsEv { int num; int type; std::string payload; };
static std::deque<WsEv> wsq; static std::set<int> ws_clients;
struct HttpReq { int id; std::string path; std::map<std::string, std::string> args; };
static std::deque<HttpReq> httpq;

// ---------------- K-line + ECU model ----------------
struct RxB { uint32_t at; uint8_t b; };
static std::deque<RxB> rxq; static bool port_open = true; static bool echo_ok = true;
static int tx_level = 1; static uint32_t tx_low_since = 0, last_wakeup = 0;
void pinMode(int, int) {}
void digitalWrite(int pin, int v) {
  if (pin != 1) return;
  if (v == LOW && tx_level == 1) tx_low_since = millis();
  if (v == HIGH && tx_level == 0) { uint32_t w = millis() - tx_low_since; if (w >= 20 && w <= 35) last_wakeup = millis(); }
  tx_level = v;
}
void bus_port_open(bool o) { port_open = o; }
int bus_available() { ecu_tick(); return port_open && !rxq.empty() && rxq.front().at <= millis(); }
int bus_read() { if (!bus_available()) return -1; uint8_t b = rxq.front().b; rxq.pop_front(); return b; }

static int valve_bit(uint8_t id) { switch (id) { case 0x11: return 7; case 0x12: return 6; case 0x21: return 5; case 0x22: return 4; case 0x31: return 3; case 0x32: return 2; case 0x41: return 1; case 0x42: return 0; } return -1; }

struct ECU {
  bool on = true, session = false; uint32_t last_req = 0;
  uint8_t valves = 0, pump = 0, relay = 1;
  int corrupt_next = 0, noise_before = 0, pending_next = 0, drop_next = 0, refuse_next = 0; bool pending_forever = false;
  uint32_t babble_until = 0, last_babble = 0; int delay_ms = 30;
  std::vector<uint8_t> in; uint32_t last_in = 0;
  std::vector<std::vector<uint8_t>> dtcs = { { 0x40, 0x35, 0x48 } };
  int requests = 0, writes = 0, pump_on_events = 0, io_while_no_session = 0;
  uint32_t valves_on_since = 0, max_valve_on_ms = 0, pump_on_since = 0, max_pump_on_ms = 0;
  std::vector<std::string> seen;
  void track() {
    uint32_t now = millis();
    if (valves && !valves_on_since) valves_on_since = now; if (!valves) valves_on_since = 0;
    if (valves_on_since && now - valves_on_since > max_valve_on_ms) max_valve_on_ms = now - valves_on_since;
    if (pump && !pump_on_since) pump_on_since = now; if (!pump) pump_on_since = 0;
    if (pump_on_since && now - pump_on_since > max_pump_on_ms) max_pump_on_ms = now - pump_on_since;
  }
  void reply(std::vector<uint8_t> p, uint32_t at) {
    std::vector<uint8_t> f = { (uint8_t)(0x80 | (p.size() <= 63 ? p.size() : 0)), 0xF1, 0x2B };
    if (p.size() > 63) f.push_back((uint8_t)p.size());       // long reply: separate length byte
    f.insert(f.end(), p.begin(), p.end());
    uint8_t cs = 0; for (auto b : f) cs += b; f.push_back(cs);
    if (corrupt_next) { corrupt_next--; f.back() ^= 0x5A; }
    if (noise_before) { noise_before--; rxq.push_back({ at, 0x13 }); at += 2; }
    for (auto b : f) { rxq.push_back({ at, b }); at += 1; }
  }
  void frame(std::vector<uint8_t> f) {
    if (!on || f.size() < 5) return;
    uint8_t len = f[0] & 0x3F; if (f.size() != (size_t)len + 4u) return;
    uint8_t cs = 0; for (size_t i = 0; i + 1 < f.size(); i++) cs += f[i]; if (cs != f.back()) return;
    uint8_t sid = f[3]; requests++;
    { char b[80]; std::string s; for (size_t i = 3; i + 1 < f.size(); i++) { snprintf(b, 80, "%02X", f[i]); s += b; } seen.push_back(s); if (seen.size() > 400) seen.erase(seen.begin()); }
    uint32_t now = millis();
    if (session && now - last_req > 5000) { session = false; valves = 0; pump = 0; }     // P3max: session ends, ECU takes the valves back
    uint32_t at = now + delay_ms;
    if (drop_next) { drop_next--; return; }
    if (sid == 0x81) {
      if (!session && (now - last_wakeup > 300)) return;
      session = true; last_req = now; reply({ 0xC1, 0xEF, 0x8F }, at); return;
    }
    if (!session) { if (sid == 0x30) io_while_no_session++; return; }
    last_req = now;
    if (refuse_next) { refuse_next--; reply({ 0x7F, sid, 0x22 }, at); return; }
    if (pending_forever) { for (int i = 0; i < 40; i++) reply({ 0x7F, sid, 0x78 }, at + i * 2000); return; }
    if (pending_next) { pending_next--; reply({ 0x7F, sid, 0x78 }, at); at += 300; }
    std::vector<uint8_t> r;
    switch (sid) {
      case 0x1A: { r = { 0x5A, f[4] }; const char *id = "A120J6000F_EBC430LCFWDQ04K2K03/02/25"; for (int i = 0; id[i]; i++) r.push_back(id[i]); r.push_back(0); r.push_back(0); r.push_back(0); break; }
      case 0x21: { uint8_t rec = f[4]; if (rec == 0x30) r = { 0x61, rec, 0x8C }; else r = { 0x61, rec, 0x00, 0x00 }; break; }
      case 0x18: { r = { 0x58, (uint8_t)dtcs.size() }; for (auto &d : dtcs) r.insert(r.end(), d.begin(), d.end()); break; }
      case 0x14: dtcs.clear(); r = { 0x54, 0xFF, 0x00 }; break;
      case 0x30: {
        uint8_t id = f[4], par = f[5];
        if (par == 0x07) {
          writes++; uint8_t st = f[6];
          if (id == 0x02) { if (st && !pump) pump_on_events++; pump = st ? 1 : 0; }
          else if (id == 0x03) relay = st ? 1 : 0;
          else { int b = valve_bit(id); if (b >= 0) { if (st) valves |= (1 << b); else valves &= ~(1 << b); } }
          track();
          r = { 0x70, id, 0x07 };
        } else {
          uint8_t v = (id == 0x49) ? valves : (id == 0x02) ? pump : (id == 0x03) ? relay : 0;
          r = { 0x70, id, 0x01, v };
        }
        break; }
      case 0x3E: r = { 0x7E }; break;
      default: r = { 0x7F, sid, 0x11 };
    }
    reply(r, at);
  }
  void tick() {
    uint32_t now = millis();
    if (babble_until > now && now != last_babble) { last_babble = now; rxq.push_back({ now, 0x00 }); }
    if (!in.empty() && now - last_in > 5) { frame(in); in.clear(); }
    if (session && now - last_req > 5000) { session = false; valves = 0; pump = 0; }
    track();
  }
} ecu;
void ecu_tick() { ecu.tick(); }
void bus_write(const uint8_t *d, size_t n) {
  uint32_t now = millis();
  for (size_t i = 0; i < n; i++) {
    if (echo_ok) rxq.push_back({ now + (uint32_t)i, d[i] });
    if (!ecu.in.empty() && now - ecu.last_in > 5) { ecu.frame(ecu.in); ecu.in.clear(); }
    ecu.in.push_back(d[i]); ecu.last_in = now;
  }
}

// ---------------- web server / websocket bridge ----------------
void ESP8266WebServer::handleClient() {
  while (!httpq.empty()) {
    HttpReq r = httpq.front(); httpq.pop_front();
    cur_id = r.id; args = r.args;
    auto it = handlers.find(r.path);
    if (it != handlers.end()) it->second(); else send(404, "text/plain", String("not found"));
    cur_id = -1;
  }
}
void ESP8266WebServer::send(int code, const char *ctype, const String &body) { out("HTTP " + std::to_string(cur_id) + " " + std::to_string(code) + " " + ctype + " " + hexenc(body.std())); }
void WebSocketsServer::loop() {
  while (hang) yield();                                       // simulated stall (test of the stall watchdog)
  while (!wsq.empty()) {
    WsEv e = wsq.front(); wsq.pop_front();
    if (e.type == WStype_CONNECTED) ws_clients.insert(e.num);
    if (e.type == WStype_DISCONNECTED) ws_clients.erase(e.num);
    std::vector<uint8_t> buf(e.payload.begin(), e.payload.end()); buf.push_back(0);
    if (_cb) _cb(e.num, (WStype_t)e.type, buf.data(), e.payload.size());
  }
}
int WebSocketsServer::connectedClients(bool) { return ws_clients.size(); }
bool WebSocketsServer::broadcastTXT(String &p) { out("WS -1 " + hexenc(p.std())); return true; }
bool WebSocketsServer::sendTXT(uint8_t num, String &p) { out("WS " + std::to_string(num) + " " + hexenc(p.std())); return true; }

static std::string ecu_state() {
  std::ostringstream o;
  o << "{\"on\":" << ecu.on << ",\"session\":" << ecu.session << ",\"valves\":" << (int)ecu.valves << ",\"pump\":" << (int)ecu.pump << ",\"relay\":" << (int)ecu.relay
    << ",\"dtcs\":" << ecu.dtcs.size() << ",\"requests\":" << ecu.requests << ",\"writes\":" << ecu.writes << ",\"pump_on_events\":" << ecu.pump_on_events
    << ",\"max_valve_on_ms\":" << ecu.max_valve_on_ms << ",\"max_pump_on_ms\":" << ecu.max_pump_on_ms << ",\"ws_clients\":" << ws_clients.size() << ",\"seen\":[";
  size_t start = ecu.seen.size() > 60 ? ecu.seen.size() - 60 : 0;
  for (size_t i = start; i < ecu.seen.size(); i++) o << (i > start ? "," : "") << "\"" << ecu.seen[i] << "\"";
  o << "]}";
  return o.str();
}

static void control(const std::string &l) {
  std::istringstream is(l); std::string cmd; is >> cmd;
  if (cmd == "WSCONN") { int n; is >> n; wsq.push_back({ n, WStype_CONNECTED, "/" }); }
  else if (cmd == "WSDISC") { int n; is >> n; wsq.push_back({ n, WStype_DISCONNECTED, "" }); }
  else if (cmd == "WSTEXT") { int n; std::string h; is >> n >> h; wsq.push_back({ n, WStype_TEXT, hexdec(h) }); }
  else if (cmd == "HTTP") {
    int id; std::string path; is >> id >> path; HttpReq r; r.id = id;
    size_t q = path.find('?'); r.path = path.substr(0, q);
    if (q != std::string::npos) { std::string a = path.substr(q + 1); std::istringstream as(a); std::string kv; while (std::getline(as, kv, '&')) { size_t e = kv.find('='); if (e != std::string::npos) r.args[kv.substr(0, e)] = kv.substr(e + 1); } }
    httpq.push_back(r);
  }
  else if (cmd == "ECU") {
    std::string what; long v = 0; is >> what; is >> v;
    if (what == "on") { ecu.on = v; if (!v) { ecu.session = false; ecu.valves = 0; ecu.pump = 0; } }
    else if (what == "corrupt") ecu.corrupt_next = v;
    else if (what == "noise") ecu.noise_before = v;
    else if (what == "pending") ecu.pending_next = v;
    else if (what == "pendforever") { if (ecu.pending_forever && !v) rxq.clear(); ecu.pending_forever = v; }
    else if (what == "babble") ecu.babble_until = millis() + v;
    else if (what == "drop") ecu.drop_next = v;
    else if (what == "refuse") ecu.refuse_next = v;
    else if (what == "delay") ecu.delay_ms = v;
    else if (what == "echo") echo_ok = v;
    else if (what == "dtcs") { ecu.dtcs.clear(); std::string h; while (is >> h) { uint32_t x = strtoul(h.c_str(), 0, 16); ecu.dtcs.push_back({ (uint8_t)(x >> 16), (uint8_t)(x >> 8), (uint8_t)x }); } if (v) {} }
    else if (what == "resetstats") { ecu.max_valve_on_ms = ecu.max_pump_on_ms = 0; ecu.pump_on_events = 0; ecu.writes = 0; ecu.seen.clear(); ecu.io_while_no_session = 0; }
    out("STATE " + ecu_state());
  }
  else if (cmd == "STATE") out("STATE " + ecu_state());
  else if (cmd == "HANG") hang = true;
}

int main(int argc, char **argv) {
  rtc_file = argc > 1 ? argv[1] : "/tmp/rtc.bin";
  FILE *f = fopen(rtc_file.c_str(), "rb"); if (f) { if (fread(rtc, 1, 512, f) == 512) reset_reason = "Software/System restart"; fclose(f); remove(rtc_file.c_str()); }
  std::thread(reader).detach();
  setup();
  out("READY");
  while (true) {
    { std::lock_guard<std::mutex> g(in_mx); while (!inq.empty()) { control(inq.front()); inq.pop_front(); } }
    loop();
    ecu_tick();
    std::this_thread::sleep_for(std::chrono::microseconds(300));
  }
}
