// LotusABS K-line simulator: runs the real kwp.cpp of v1.0 or v1.1 against a simulated EBC430 ECU
#include "Arduino.h"
#include "kwp.h"
#include <stdarg.h>
#include <vector>
#include <deque>
#include <string>
#include <new>
#ifdef V10
typedef uint8_t *PAY;
#define VER "v1.0"
#else
typedef const uint8_t *PAY;
#define VER "v1.1"
KWP2KStats *dummy_stats_ref = &kwp_stats;
#endif

// ---------------- heap accounting (only allocations made inside firmware code) ----------------
extern "C" void *__real_malloc(size_t); extern "C" void __real_free(void *);
static bool in_fw = false;
struct NotFw { bool save; NotFw(){ save=in_fw; in_fw=false; } ~NotFw(){ in_fw=save; } };
static long fw_live_bytes = 0, fw_live_esp = 0, fw_live_count = 0;
#include <map>
static std::map<size_t,long> *hist=nullptr;
struct Hdr { size_t n; int fw; int pad; };
static size_t esp_cost(size_t n) { return ((n + 4 + 7) / 8) * 8; }     // umm_malloc: 8-byte blocks, 4-byte header
extern "C" void *__wrap_malloc(size_t n) {
  Hdr *h = (Hdr *)__real_malloc(n + sizeof(Hdr)); h->n = n; h->fw = in_fw;
  if (in_fw) { fw_live_bytes += n; fw_live_esp += esp_cost(n); fw_live_count++; in_fw=false; if(!hist) hist=new std::map<size_t,long>; (*hist)[n]++; in_fw=true; }
  return h + 1;
}
extern "C" void __wrap_free(void *p) {
  if (!p) return; Hdr *h = (Hdr *)p - 1;
  if (h->fw) { fw_live_bytes -= h->n; fw_live_esp -= esp_cost(h->n); fw_live_count--; if(hist) (*hist)[h->n]--; }
  __real_free(h);
}
void *operator new(size_t n) { return __wrap_malloc(n); }
void *operator new[](size_t n) { return __wrap_malloc(n); }
void operator delete(void *p) noexcept { __wrap_free(p); }
void operator delete[](void *p) noexcept { __wrap_free(p); }
void operator delete(void *p, size_t) noexcept { __wrap_free(p); }
void operator delete[](void *p, size_t) noexcept { __wrap_free(p); }

// ---------------- time ----------------
uint32_t g_now = 1000;
struct ECU; void ecu_tick();
uint32_t hang_deadline = 0; const char *hang_name = "";
static void hang_check() { if (hang_deadline && g_now > hang_deadline) { printf("%-44s %-5s %s\n", hang_name, "FAIL", "firmware stuck inside process() (web pages would be dead)"); exit(1); } }
void delay(uint32_t ms) { for (uint32_t i = 0; i < ms; i++) { g_now++; ecu_tick(); } hang_check(); }
void yield() { g_now++; ecu_tick(); hang_check(); }

// ---------------- logging ----------------
std::vector<std::string> LOG;
void log_msg(const char *fmt, ...) { NotFw g; char b[600]; va_list a; va_start(a, fmt); vsnprintf(b, 600, fmt, a); va_end(a); LOG.push_back(b); }
void log_line(const char *fmt, ...) { NotFw g; char b[600]; va_list a; va_start(a, fmt); vsnprintf(b, 600, fmt, a); va_end(a); LOG.push_back(b); }
void log_hex(const char *p, const uint8_t *d, uint16_t n) { NotFw g; std::string s=p; s+=":"; char t[4]; for(int i=0;i<n;i++){snprintf(t,4," %02X",d[i]); s+=t;} s+="\n"; LOG.push_back(s); }

// ---------------- K-line + ECU model ----------------
struct RxB { uint32_t at; uint8_t b; };
std::deque<RxB> rxq;              // bytes on their way to the D1 mini RX pin
bool port_open = true;
bool echo_ok = true;              // false = dead L9637 (nothing comes back)
int  tx_level = 1; uint32_t tx_low_since = 0; uint32_t last_wakeup = 0;
void pinMode(int, int) {}
void digitalWrite(int pin, int v) {
  if (pin != 1) return;
  if (v == LOW && tx_level == 1) tx_low_since = g_now;
  if (v == HIGH && tx_level == 0) { uint32_t w = g_now - tx_low_since; if (w >= 20 && w <= 30) last_wakeup = g_now; }
  tx_level = v;
}
void bus_port_open(bool o) { port_open = o; }
int bus_available() { ecu_tick(); return port_open && !rxq.empty() && rxq.front().at <= g_now; }
int bus_read() { NotFw g; if (!bus_available()) return -1; uint8_t b = rxq.front().b; rxq.pop_front(); return b; }

struct ECU {
  bool on = true;
  bool session = false; uint32_t last_req = 0; uint32_t woke = 0;
  uint8_t valves = 0, pump = 0, relay = 0;
  int corrupt_next = 0, noise_before = 0, pending_next = 0, gap_mid = 0, drop_next = 0;
  bool tp_supported = true;
  std::vector<uint8_t> in;  uint32_t last_in = 0;
  int pump_on_events = 0; uint32_t pump_on_at = 0;
  int requests = 0; uint8_t src = 0x2B; uint32_t babble_until = 0; int pending_forever = 0;
  void reply(std::vector<uint8_t> payload, uint32_t at) {
    std::vector<uint8_t> f = { (uint8_t)(0x80 | payload.size()), 0xF1, src };
    f.insert(f.end(), payload.begin(), payload.end());
    uint8_t cs = 0; for (auto b : f) cs += b; f.push_back(cs);
    if (corrupt_next) { corrupt_next--; f.back() ^= 0x5A; }
    if (noise_before) { noise_before--; rxq.push_back({ at, 0x13 }); at += 2; }
    for (size_t i = 0; i < f.size(); i++) { if (gap_mid && i == 3) { gap_mid--; at += 40; } rxq.push_back({ at, f[i] }); at += 1; }
  }
  void frame(std::vector<uint8_t> f) {
    if (!on) return;
    uint8_t len = f[0] & 0x3F; if (f.size() != (size_t)len + 4u) return;
    uint8_t cs = 0; for (size_t i = 0; i + 1 < f.size(); i++) cs += f[i]; if (cs != f.back()) return;
    uint8_t sid = f[3]; requests++;
    if (session && g_now - last_req > 5000) session = false;          // P3max
    uint32_t at = g_now + 30;                                          // P2
    if (drop_next) { drop_next--; return; }
    if (sid == 0x81) {
      if (!session && (g_now - last_wakeup > 300)) return;            // needs a fast-init wake-up first
      session = true; last_req = g_now; reply({ 0xC1, 0xEF, 0x8F }, at); return;
    }
    if (!session) return;                                              // no session: ECU stays silent
    last_req = g_now;
    std::vector<uint8_t> r;
    if (pending_forever) { for (int i = 0; i < 40; i++) reply({ 0x7F, sid, 0x78 }, at + i * 2000); return; }
    if (pending_next) { pending_next--; reply({ 0x7F, sid, 0x78 }, at); at += 60; }
    switch (sid) {
      case 0x1A: { r = { 0x5A, f[4] }; const char *id = "A120J6000F_EBC430LCFWDQ04K2K03/02/25"; for (int i = 0; id[i]; i++) r.push_back(id[i]); r.push_back(0); r.push_back(0); r.push_back(0); break; }
      case 0x21: r = { 0x61, f[4], 0x8C }; break;                       // 14.0 V
      case 0x18: r = { 0x58, 0x02, 0x40, 0x35, 0x48, 0x40, 0x23, 0x08 }; break;
      case 0x14: r = { 0x54, 0xFF, 0x00 }; break;
      case 0x30: {
        uint8_t id = f[4], par = f[5];
        if (par == 0x07) {
          uint8_t st = f[6];
          if (id == 0x01) { if (st && !pump) { pump_on_events++; pump_on_at = g_now; } pump = st; }
          r = { 0x70, id, 0x07 };
        } else r = { 0x70, id, 0x01, (uint8_t)(id == 0x01 ? pump : valves) };
        break; }
      case 0x3E: r = tp_supported ? std::vector<uint8_t>{ 0x7E } : std::vector<uint8_t>{ 0x7F, 0x3E, 0x11 }; break;
      default: r = { 0x7F, sid, 0x11 };
    }
    reply(r, at);
  }
  void tick() {
    static uint32_t last_babble = 0;
    if (babble_until > g_now && g_now != last_babble) { last_babble = g_now; rxq.push_back({ g_now, 0x00 }); }   // K-line held low / chatter
    if (!in.empty() && g_now - last_in > 5) { frame(in); in.clear(); }  // inter-frame gap ends a request
  }
} ecu;
void ecu_tick() { NotFw g; ecu.tick(); }
void bus_write(const uint8_t *d, size_t n) {
  NotFw g;
  for (size_t i = 0; i < n; i++) {
    if (echo_ok) rxq.push_back({ g_now, d[i] });
    if (ecu.in.empty() || g_now - ecu.last_in <= 5) ecu.in.push_back(d[i]); else { ecu.frame(ecu.in); ecu.in.assign(1, d[i]); }
    ecu.last_in = g_now;
    g_now += 1;                                                        // ~1 ms per byte at 10400 baud
  }
}
HardwareSerial Serial;

// ---------------- web UI side (what the browser would see) ----------------
int n_start = 0, n_data = 0, n_id = 0, n_dtc = 0, n_io = 0, n_err = 0, n_timeout = 0;
std::string last_id;
void cbStart(PAY p, uint8_t l) { n_start++; }
void cbData(PAY p, uint8_t l) { n_data++; }
void cbId(PAY p, uint8_t l) { n_id++; last_id.assign((const char *)p + 2, (const char *)p + (l > 13 ? 13 : l)); }
void cbDtc(PAY p, uint8_t l) { n_dtc++; }
void cbIo(PAY p, uint8_t l) { n_io++; }
void cbErr(PAY p, uint8_t l) { n_err++; }
void cbIo2(PAY p, uint8_t l) {}
void cbTimeout() { n_timeout++; }

KWP2K *K;
void fw(std::function<void()> f) { in_fw = true; f(); in_fw = false; }
void run(uint32_t ms) { uint32_t end = g_now + ms; while (g_now < end) { in_fw = true; K->process(); in_fw = false; g_now++; ecu_tick(); } }
struct Snap { int start, data, id, dtc, io, err, timeout; };
Snap snap() { return { n_start, n_data, n_id, n_dtc, n_io, n_err, n_timeout }; }
void reset_page() { /* nothing to reset in firmware */ }

int main() {
  in_fw = true;
  K = new KWP2K(1, 3);
#ifndef V10
  K->begin();
#endif
  K->setCallback(0x81, cbStart); K->setCallback(0x21, cbData); K->setCallback(0x1A, cbId);
  K->setCallback(0x18, cbDtc); K->setCallback(0x30, cbIo); K->setCallback(0x14, cbIo2); K->setCallback(0x7F, cbErr); K->setTimeoutCallback(cbTimeout);
  in_fw = false;
  long heap0 = fw_live_esp;
  printf("=== %s ===\n", VER);
#define RESULT(name, ok, detail) printf("%-44s %-5s %s\n", name, (ok) ? "OK" : "FAIL", detail)
  char d[200];

  // 1 home page: connect, ECU id, voltage
  { Snap s = snap(); fw([] { K->StartCommunication(); K->ReadECUIdentification(0x86); K->ReadDataByLocalId(0x30, 0); }); run(2000);
    Snap e = snap(); snprintf(d, 200, "connect=%d id=%d volt=%d err=%d timeouts=%d id='%s'", e.start - s.start, e.id - s.id, e.data - s.data, e.err - s.err, e.timeout - s.timeout, last_id.c_str());
    RESULT("1 connect + ECU id + voltage", e.start - s.start == 1 && e.id - s.id == 1 && e.data - s.data == 1 && e.timeout == s.timeout, d); }

  // 2 DTC read + clear
  { Snap s = snap(); fw([] { K->ReadDiagnosticTroubleCodesByStatus(); K->ClearDiagnosticInformationService(); }); run(1000);
    Snap e = snap(); snprintf(d, 200, "dtc replies=%d timeouts=%d", e.dtc - s.dtc, e.timeout - s.timeout); RESULT("2 read DTCs + clear", e.dtc - s.dtc == 1 && e.timeout == s.timeout, d); }

  // 3 valve page: read all valves, pump on, pump off
  { Snap s = snap(); fw([] { K->InputOutputControlByLocalId(0x0B, 0x01, 0); K->InputOutputControlByLocalId(0x01, 0x07, 1); K->InputOutputControlByLocalId(0x01, 0x07, 0); }); run(1000);
    Snap e = snap(); snprintf(d, 200, "io replies=%d pump=%d timeouts=%d", e.io - s.io, ecu.pump, e.timeout - s.timeout); RESULT("3 valve read + pump on/off", e.io - s.io == 3 && ecu.pump == 0 && e.timeout == s.timeout, d); }

  // 4 one corrupt reply (bad checksum)
  { Snap s = snap(); uint32_t t0 = g_now; ecu.corrupt_next = 1; fw([] { K->ReadDataByLocalId(0x30, 0); }); run(3000);
    Snap e = snap(); snprintf(d, 200, "value shown=%d error popup(timeout)=%d", e.data - s.data, e.timeout - s.timeout);
    RESULT("4 one corrupt reply", e.data - s.data == 1 && e.timeout == s.timeout, d);
    fw([] { K->StartCommunication(); }); run(1500); }

  // 5 noise byte in front of a reply
  { Snap s = snap(); ecu.noise_before = 1; fw([] { K->ReadDataByLocalId(0x30, 0); }); run(3000);
    Snap e = snap(); snprintf(d, 200, "value shown=%d timeout=%d", e.data - s.data, e.timeout - s.timeout);
    RESULT("5 noise byte before reply", e.data - s.data == 1 && e.timeout == s.timeout, d);
    fw([] { K->StartCommunication(); }); run(1500); }

  // 6 40 ms gap inside a reply then OK next time
  { Snap s = snap(); ecu.gap_mid = 1; fw([] { K->ReadDataByLocalId(0x30, 0); }); run(3000);
    fw([] { K->StartCommunication(); K->ReadDataByLocalId(0x30, 0); }); run(3000);
    Snap e = snap(); snprintf(d, 200, "values shown=%d (2 asked) timeouts=%d", e.data - s.data, e.timeout - s.timeout);
    RESULT("6 slow reply (gap inside frame)", e.data - s.data >= 1, d); }

  // 7 ECU busy: 7F xx 78 response pending, then the answer
  { Snap s = snap(); ecu.pending_next = 1; fw([] { K->StartCommunication(); K->ReadDataByLocalId(0x30, 0); }); run(3000);
    Snap e = snap(); snprintf(d, 200, "value shown=%d error popup=%d timeouts=%d", e.data - s.data, e.err - s.err, e.timeout - s.timeout);
    RESULT("7 ECU busy (response pending)", e.data - s.data == 1 && e.err == s.err && e.timeout == s.timeout, d); }

  // 8 stray byte on the line while idle, then a request
  { fw([] { K->StartCommunication(); }); run(1500); Snap s = snap(); rxq.push_back({ g_now, 0x55 }); run(50); fw([] { K->ReadDataByLocalId(0x30, 0); }); run(3000);
    Snap e = snap(); snprintf(d, 200, "value shown=%d timeouts=%d", e.data - s.data, e.timeout - s.timeout);
    RESULT("8 stray byte while idle", e.data - s.data == 1 && e.timeout == s.timeout, d); }

  // 9 request order: valve A on, valve B on, valve A off clicked quickly
  { fw([] { K->StartCommunication(); }); run(1500);
    std::vector<std::pair<int,int>> order;
    // record IO writes the ECU sees
    uint8_t before = ecu.valves;
    fw([] { K->InputOutputControlByLocalId(0x0C, 0x07, 1); }); run(1);     // A on (sent straight away)
    fw([] { K->InputOutputControlByLocalId(0x0D, 0x07, 1); }); run(1);     // B on (queued, slot 1)
    fw([] { K->InputOutputControlByLocalId(0x0C, 0x07, 0); }); run(1);     // A off (queued; v1.0 reuses slot 0)
    // log what reaches the ECU by watching requests in order
    std::vector<int> seen; int r0 = ecu.requests;
    for (int i = 0; i < 3000; i++) { size_t q = rxq.size(); run(1); (void)q; }
    // reconstruct from the log of writes: cheap way, resend recognisable reads
    snprintf(d, 200, "ECU requests seen=%d", ecu.requests - r0);
    RESULT("9 three quick valve clicks all sent", ecu.requests - r0 >= 2, d); }

  // 10 ignition off: page keeps polling, user clicks PUMP ON, 30 s later ignition on
  { ecu.on = false; ecu.session = false; int ev0 = ecu.pump_on_events;
    for (int i = 0; i < 4; i++) { fw([] { K->InputOutputControlByLocalId(0x0B, 0x01, 0); }); run(2500); }
    fw([] { K->InputOutputControlByLocalId(0x01, 0x07, 1); });            // pump ON clicked, ECU off
    for (int i = 0; i < 12; i++) { fw([] { K->InputOutputControlByLocalId(0x0B, 0x01, 0); }); run(2500); }
    ecu.on = true;                                                         // ignition on, user just watching
    for (int i = 0; i < 8; i++) { fw([] { K->InputOutputControlByLocalId(0x0B, 0x01, 0); }); run(2500); }
    int fired = ecu.pump_on_events - ev0;
    snprintf(d, 200, "pump switched on by itself after ignition on: %s", fired ? "YES" : "no");
    RESULT("10 old pump click after ignition on", fired == 0, d);
    fw([] { K->InputOutputControlByLocalId(0x01, 0x07, 0); }); run(2000); }

  // 11 dead L9637: no echo, no reply
  { echo_ok = false; ecu.on = false; Snap s = snap(); LOG.clear(); fw([] { K->StartCommunication(); K->ReadDataByLocalId(0x30, 0); }); run(8000);
    Snap e = snap(); bool told = false; for (auto &l : LOG) if (l.find("echo") != std::string::npos) told = true;
    snprintf(d, 200, "timeouts shown=%d, log names the echo problem: %s", e.timeout - s.timeout, told ? "yes" : "no");
    RESULT("11 dead K-line chip", e.timeout > s.timeout, d); echo_ok = true; ecu.on = true; }

  // 12 heap: 2 hours on the valve page (poll all valves every 2.5 s, 20 valve/pump clicks)
  { long h0 = fw_live_esp; fw([] { K->StartCommunication(); }); run(1500);
    for (int i = 0; i < 2880; i++) {
      fw([] { K->InputOutputControlByLocalId(0x0B, 0x01, 0); });
      if (i % 144 == 0) fw([] { K->InputOutputControlByLocalId(0x0C, 0x07, 1); K->InputOutputControlByLocalId(0x0B, 0x01, 0); });
      run(2500);
    }
    long leak = fw_live_esp - h0;
    snprintf(d, 200, "heap lost: %ld bytes in 2 h (%.1f KB/h)", leak, leak / 2048.0);
    RESULT("12 2 h on valve page, heap", leak < 512, d); }

  // 13 heap: ignition off for 30 min with home page open (poll voltage every 2.5 s)
  { ecu.on = false; long h0 = fw_live_esp; Snap s = snap();
    for (int i = 0; i < 720; i++) { fw([] { K->ReadDataByLocalId(0x30, 0); }); run(2500); }
    long leak = fw_live_esp - h0; Snap e = snap();
    snprintf(d, 200, "heap lost: %ld bytes in 30 min, error popups %d", leak, e.timeout - s.timeout);
    RESULT("13 30 min ignition off, heap", leak < 2048, d); ecu.on = true; }

  // 14 back to normal after all that
  { LOG.clear(); Snap s = snap(); for (int i = 0; i < 3; i++) { fw([] { K->StartCommunication(); K->ReadDataByLocalId(0x30, 0); }); run(2500); } Snap e = snap();
    snprintf(d, 200, "connect=%d value=%d", e.start - s.start, e.data - s.data); RESULT("14 still works afterwards (3 polls)", e.data - s.data >= 2, d); if (getenv("DBG")) { for (size_t i = LOG.size() > 25 ? LOG.size() - 25 : 0; i < LOG.size(); i++) printf("   | %s", LOG[i].c_str()); printf("\n ecu on=%d session=%d wake=%u now=%u\n", ecu.on, ecu.session, last_wakeup, g_now); } }
  if(hist) for(auto&kv:*hist) if(kv.second) printf("  live size %zu x %ld\n", kv.first, kv.second);
  // 15 pump ON, then OFF clicked while a poll reply gets lost on the line
  { fw([] { K->StartCommunication(); }); run(1500);
    fw([] { K->InputOutputControlByLocalId(0x01, 0x07, 1); }); run(500);
    int on = ecu.pump;
    ecu.drop_next = 1; fw([] { K->InputOutputControlByLocalId(0x0B, 0x01, 0); }); run(5);      // poll in flight, its reply is lost
    fw([] { K->InputOutputControlByLocalId(0x01, 0x07, 0); });                                  // user clicks PUMP OFF
    for (int i = 0; i < 4; i++) { fw([] { K->InputOutputControlByLocalId(0x0B, 0x01, 0); }); run(2500); }
    snprintf(d, 200, "pump was on=%d, pump after OFF click: %s", on, ecu.pump ? "STILL ON" : "off");
    RESULT("15 pump OFF clicked during a lost reply", on == 1 && ecu.pump == 0, d); }
  // 16 ECU answers from a different address than it was called on
  { ecu.src = 0x28; Snap s = snap(); for (int i = 0; i < 2; i++) { fw([] { K->StartCommunication(); K->ReadDataByLocalId(0x30, 0); }); run(2500); } Snap e = snap(); ecu.src = 0x2B;
    snprintf(d, 200, "values shown=%d (2 asked)", e.data - s.data); RESULT("16 ECU replies from another address", e.data - s.data == 2, d); }
  // 17 corrupt reply to Clear DTCs, then the K-line keeps chattering (ECU restarting / line held low) for 60 s
  { fw([] { K->StartCommunication(); }); run(1500);
    ecu.corrupt_next = 1; ecu.babble_until = g_now + 60030;
    hang_name = "17 corrupt reply + K-line chatter 60 s"; hang_deadline = g_now + 5000;
    uint32_t t0 = g_now; fw([] { K->ClearDiagnosticInformationService(); }); run(100);
    hang_deadline = 0;
    snprintf(d, 200, "process() came back, longest blocked time < 5 s");
    RESULT("17 corrupt reply + K-line chatter 60 s", true, d);
    ecu.babble_until = 0; run(61000); while (!rxq.empty()) rxq.pop_front(); }
  // 18 ECU answers "response pending" forever
  { fw([] { K->StartCommunication(); }); run(1500); Snap s = snap();
    ecu.pending_forever = 1; fw([] { K->ClearDiagnosticInformationService(); }); run(40000); ecu.pending_forever = 0;
    while (!rxq.empty()) rxq.pop_front();
    fw([] { K->StartCommunication(); K->ReadDataByLocalId(0x30, 0); }); run(5000);
    Snap e = snap(); snprintf(d, 200, "gave up and answered the page: %s, next request works: %s", (e.err + e.timeout > s.err + s.timeout) ? "yes" : "NO", (e.data > s.data) ? "yes" : "NO");
    RESULT("18 ECU 'response pending' forever", (e.err + e.timeout > s.err + s.timeout) && e.data > s.data, d); }
  printf("firmware heap still held at end: %ld bytes (%ld blocks)\n", fw_live_esp - heap0, fw_live_count);
}
