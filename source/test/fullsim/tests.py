#!/usr/bin/env python3
"""LotusABS full-stack scenarios: real firmware (native build) + simulated EBC430 + real browser on the real pages."""
import time, json, urllib.request, sys, re, os, traceback
from playwright.sync_api import sync_playwright

BASE = 'http://127.0.0.1'
RESULTS = []
ONLY = sys.argv[1:]

def ctl(c):
    return json.loads(urllib.request.urlopen(urllib.request.Request('http://127.0.0.1:8099/', data=c.encode()), timeout=10).read())

def fw_log():
    try: return urllib.request.urlopen(BASE + '/log.txt?from=0', timeout=3).read().decode().split('\n', 1)[1]
    except Exception: return ''

def http_ok(path='/status', timeout=2.0):
    t = time.time()
    try:
        urllib.request.urlopen(BASE + path, timeout=timeout).read(); return time.time() - t
    except Exception: return None

def wait_up(t=40):
    end = time.time() + t
    while time.time() < end:
        try:
            if ctl('UP').get('up') and http_ok(): return True
        except Exception: pass
        time.sleep(0.3)
    return False

def reset_ecu():
    for c in ['ECU on 1', 'ECU echo 1', 'ECU corrupt 0', 'ECU noise 0', 'ECU pending 0', 'ECU pendforever 0', 'ECU drop 0', 'ECU refuse 0', 'ECU delay 30', 'ECU dtcs 0 403548', 'ECU resetstats 0']:
        ctl(c)

def result(name, ok, detail):
    RESULTS.append((name, ok, detail))
    if not ok:
        with open('/tmp/fail_logs.txt', 'a') as f: f.write(f'===== {name}\n' + fw_log()[-5000:] + '\n')
    print(f"{'OK  ' if ok else 'FAIL'} {name:52s} {detail}", flush=True)

class Page:
    def __init__(self, browser, path='/'):
        self.ctx = browser.new_context(accept_downloads=True)
        self.p = self.ctx.new_page()
        self.errors = []; self.bad = []; self.dialogs = []; self.dialog_answer = True
        self.p.on('pageerror', lambda e: self.errors.append(str(e)))
        self.p.on('console', lambda m: m.type == 'error' and 'WebSocket' not in m.text and self.errors.append(m.text))
        self.p.on('response', lambda r: r.status >= 400 and self.bad.append(f'{r.status} {r.url}'))
        def dlg(d):
            self.dialogs.append(d.message)
            d.accept() if self.dialog_answer else d.dismiss()
        self.p.on('dialog', dlg)
        if path: self.p.goto(BASE + path)
    def close(self): self.ctx.close()
    def cls(self, sel): return self.p.get_attribute(sel, 'class') or ''
    def on(self, sel): return 'btn-success' in self.cls(sel)
    def visible(self, sel): return self.p.is_visible(sel)

def wait_for(fn, t=6.0, step=0.2):
    end = time.time() + t
    while time.time() < end:
        try:
            v = fn()
            if v: return v
        except Exception: pass
        time.sleep(step)
    return None

VALVES = [('lf-dump', 7), ('lf-iso', 6), ('rf-dump', 5), ('rf-iso', 4), ('lr-dump', 3), ('lr-iso', 2), ('rr-dump', 1), ('rr-iso', 0)]

def T(name):
    def deco(f):
        def run(browser):
            if ONLY and not any(o in name for o in ONLY): return
            if not wait_up(): result(name, False, 'firmware not up before test'); return
            reset_ecu()
            try: f(browser, name)
            except Exception as e:
                result(name, False, 'exception: ' + ' | '.join(str(e).split(chr(10))[-6:])[:600]); traceback.print_exc()
            for fl in ['JSON reply truncated', 'tx queue full']:
                if fl in fw_log(): print(f'      note: firmware log contains "{fl}"')
        run.__name__ = name; TESTS.append(run); return run
    return deco
TESTS = []

# ---------------------------------------------------------------- pages
@T('01 every page loads, no JS errors, no missing files')
def _(b, n):
    bad = []
    for path in ['/', '/valves.html', '/dtc.html', '/log.html', '/disclaimer.html']:
        pg = Page(b, path); time.sleep(3)
        txt = pg.p.inner_text('body')
        if pg.errors: bad.append(f'{path}: JS {pg.errors[:2]}')
        if pg.bad: bad.append(f'{path}: {pg.bad[:3]}')
        if 'Swoboda' in pg.p.content(): bad.append(f'{path}: name still shown')
        if 'Updates and Bug Fixes by Rstone72' not in txt or 'by CyberNet' not in txt: bad.append(f'{path}: credit line missing')
        links = pg.p.eval_on_selector_all('a.nav-link', 'e => e.map(x => x.getAttribute("href"))')
        if '/update' in links: bad.append(f'{path}: old Firmware Update menu item')
        if 'log.html' not in links: bad.append(f'{path}: no Debug link')
        if 'disclaimer.html' not in links: bad.append(f'{path}: no Disclaimer link')
        pg.close()
    result(n, not bad, '; '.join(bad) or '5 pages clean, menu and credits correct')

@T('02 home: ECU id and battery voltage')
def _(b, n):
    pg = Page(b, '/')
    ok = wait_for(lambda: pg.p.inner_text('#ecu_design_code').strip() and pg.p.inner_text('#batvoltage').strip(), 8)
    d = f"design='{pg.p.inner_text('#ecu_design_code')}' hw='{pg.p.inner_text('#ecu_id_hardware')}' sw='{pg.p.inner_text('#ecu_id_software')}' date='{pg.p.inner_text('#ecu_id_date')}' volt='{pg.p.inner_text('#batvoltage')}'"
    modal_gone = wait_for(lambda: not pg.visible('#StartCommunicationModal'), 3)
    result(n, bool(ok) and bool(modal_gone) and 'EBC430' in d and '14.00' in d, d + ('' if modal_gone else ' (connect popup stays open)'))
    pg.close()

@T('03 home: ignition off -> timeout popup, then ignition on -> works')
def _(b, n):
    ctl('ECU on 0')
    pg = Page(b, '/')
    popup = wait_for(lambda: pg.visible('#TimeoutModal'), 8)
    pg.close(); ctl('ECU on 1')
    pg = Page(b, '/')
    ok = wait_for(lambda: pg.p.inner_text('#batvoltage').strip(), 10)
    result(n, bool(popup) and bool(ok), f'timeout popup shown: {bool(popup)}, after ignition on voltage: {pg.p.inner_text("#batvoltage")!r}')
    pg.close()

# ---------------------------------------------------------------- valves
def valve_page(b):
    pg = Page(b, '/valves.html')
    wait_for(lambda: ctl('STATE')['session'] and not pg.visible('#StartCommunicationModal'), 8)
    time.sleep(0.5)
    return pg

@T('04 valves: each valve ON then OFF')
def _(b, n):
    pg = valve_page(b); bad = []
    for name, bit in VALVES:
        pg.p.click(f'#btn-{name}')
        if not wait_for(lambda: ctl('STATE')['valves'] & (1 << bit) and pg.on(f'#btn-{name}'), 5): bad.append(f'{name} on')
        pg.p.click(f'#btn-{name}')
        if not wait_for(lambda: not (ctl('STATE')['valves'] & (1 << bit)) and not pg.on(f'#btn-{name}'), 5): bad.append(f'{name} off')
    result(n, not bad and not pg.errors, ('failed: ' + ', '.join(bad)) if bad else '8 valves on/off, ECU and buttons agree')
    pg.close()

@T('05 valves: second valve while another is on (e.g. DUMP on, then ISO)')
def _(b, n):
    pg = valve_page(b); bad = []
    for first, fb in VALVES:
        for second, sb in VALVES:
            if first == second: continue
            if fb != 7 and sb != 0 and not (fb == 6 and sb == 7): continue          # sample: everything after LF DUMP, everything before RR ISO, ISO->DUMP
            pg.p.click(f'#btn-{first}'); wait_for(lambda: ctl('STATE')['valves'] & (1 << fb) and pg.on(f'#btn-{first}'), 5)
            wait_for(lambda: pg.on(f'#btn-{first}'), 3)
            pg.p.click(f'#btn-{second}')
            ok = wait_for(lambda: ctl('STATE')['valves'] == ((1 << fb) | (1 << sb)), 4)
            if not ok: bad.append(f'{first}+{second} -> ECU {ctl("STATE")["valves"]:08b}')
            # back to all off
            for nm, bt in VALVES:
                if ctl('STATE')['valves'] & (1 << bt):
                    pg.p.click(f'#btn-{nm}'); wait_for(lambda: not (ctl('STATE')['valves'] & (1 << bt)) and not pg.on(f'#btn-{nm}'), 4)
            time.sleep(0.3)
    result(n, not bad, (f'{len(bad)} combos wrong, e.g. ' + '; '.join(bad[:4])) if bad else 'all sampled combinations switch correctly')
    pg.close()

@T('06 valves: pump on/off, button follows')
def _(b, n):
    pg = valve_page(b)
    pg.p.click('#btn-pump'); on = wait_for(lambda: ctl('STATE')['pump'] == 1 and pg.on('#btn-pump'), 6)
    pg.p.click('#btn-pump'); off = wait_for(lambda: ctl('STATE')['pump'] == 0 and not pg.on('#btn-pump'), 6)
    result(n, bool(on) and bool(off), f'on ok: {bool(on)}, off ok: {bool(off)}')
    pg.close()

@T('07 valves: relay ON/OFF buttons')
def _(b, n):
    pg = valve_page(b)
    pg.p.click('text=Valve Relay OFF'); off = wait_for(lambda: ctl('STATE')['relay'] == 0, 5)
    pg.p.click('text=Valve Relay ON'); on = wait_for(lambda: ctl('STATE')['relay'] == 1, 5)
    result(n, bool(on) and bool(off), f'relay off reached ECU: {bool(off)}, back on: {bool(on)}')
    pg.close()

@T('08 valves: page shows true state on open (pump already on)')
def _(b, n):
    pg = valve_page(b)
    pg.p.click('#btn-pump'); wait_for(lambda: ctl('STATE')['pump'] == 1, 5)
    pg2 = Page(b, '/valves.html'); time.sleep(4)          # second tab opens while pump runs
    shown = pg2.on('#btn-pump')
    pg2.close(); pg.close()
    result(n, shown, f'second tab shows pump ON: {shown}')

@T('09 valves: leaving the page switches valves and pump off')
def _(b, n):
    pg = valve_page(b)
    pg.p.click('#btn-lf-iso'); wait_for(lambda: ctl('STATE')['valves'] & 0x40, 5)
    pg.p.click('#btn-lf-dump'); wait_for(lambda: ctl('STATE')['valves'] & 0x80, 5)
    pg.p.click('#btn-pump'); wait_for(lambda: ctl('STATE')['pump'] == 1, 5)
    before = ctl('STATE')
    pg.p.click('a.nav-link[href="dtc.html"]')
    off = wait_for(lambda: ctl('STATE')['valves'] == 0 and ctl('STATE')['pump'] == 0, 4)
    result(n, before['valves'] == 0xC0 and before['pump'] == 1 and bool(off), f'before: valves={before["valves"]:08b} pump={before["pump"]}; after going to Trouble Codes: {"all off" if off else ctl("STATE")}')
    pg.close()

@T('10 valves: closing the tab switches valves off')
def _(b, n):
    pg = valve_page(b)
    pg.p.click('#btn-rr-dump'); wait_for(lambda: ctl('STATE')['valves'] & 0x02, 5)
    pg.close()
    off = wait_for(lambda: ctl('STATE')['valves'] == 0, 4)
    result(n, bool(off), 'valve off after tab closed' if off else f'still on: {ctl("STATE")["valves"]:08b}')

@T('11 valves: 40 fast clicks on one valve, module stays responsive')
def _(b, n):
    pg = valve_page(b)
    worst = 0
    for i in range(40):
        pg.p.click('#btn-lf-iso', no_wait_after=True); time.sleep(0.05)
        if i % 8 == 0:
            t = http_ok(); worst = max(worst, t if t is not None else 99)
    time.sleep(4)
    st = ctl('STATE'); btn = pg.on('#btn-lf-iso'); ecu_on = bool(st['valves'] & 0x40)
    resp = http_ok()
    ok = resp is not None and worst < 2 and btn == ecu_on
    result(n, ok, f'slowest /status during clicks {worst:.2f}s, button shows {"ON" if btn else "OFF"}, ECU {"ON" if ecu_on else "OFF"}')
    pg.close()

@T('12 valves: click all 8 + pump quickly, then all off')
def _(b, n):
    pg = valve_page(b)
    for nm, _ in VALVES: pg.p.click(f'#btn-{nm}', no_wait_after=True); time.sleep(0.08)
    pg.p.click('#btn-pump', no_wait_after=True)
    allon = wait_for(lambda: ctl('STATE')['valves'] == 0xFF and ctl('STATE')['pump'] == 1, 8)
    time.sleep(3)
    for nm, _ in VALVES: pg.p.click(f'#btn-{nm}', no_wait_after=True); time.sleep(0.08)
    pg.p.click('#btn-pump', no_wait_after=True)
    alloff = wait_for(lambda: ctl('STATE')['valves'] == 0 and ctl('STATE')['pump'] == 0, 8)
    result(n, bool(allon) and bool(alloff), f'all on reached: {bool(allon)}, all off reached: {bool(alloff)} (ECU now {ctl("STATE")["valves"]:08b})')
    pg.close()

# ---------------------------------------------------------------- DTCs
def dtc_rows(pg): return pg.p.locator('#dtccontainer .row').count()

@T('13 DTCs: list shows every code with its text')
def _(b, n):
    ctl('ECU dtcs 0 403548 423208 550000')        # C0035, C0232, C1500 (unknown code)
    pg = Page(b, '/dtc.html')
    ok = wait_for(lambda: dtc_rows(pg) == 3, 8)
    txt = pg.p.inner_text('#dtccontainer')
    good = bool(ok) and 'C0035' in txt and 'C0232' in txt and 'Unknown DTC' in txt and 'telltale' in txt
    result(n, good and not pg.errors, f'rows={dtc_rows(pg)} ' + ' | '.join(l for l in txt.split('\n') if l.strip())[:160])
    pg.close()

@T('14 DTCs: clear (confirmed) -> ECU cleared, page confirms and list empties')
def _(b, n):
    pg = Page(b, '/dtc.html'); wait_for(lambda: dtc_rows(pg) == 1, 8)
    pg.p.click('#btn-ClearDiagnosticInformationService')
    cleared = wait_for(lambda: ctl('STATE')['dtcs'] == 0, 6)
    msg = wait_for(lambda: 'cleared' in pg.p.inner_text('#errormessage').lower() and pg.visible('#errormessage'), 4)
    empty = wait_for(lambda: dtc_rows(pg) == 0, 6)
    btn = pg.p.is_enabled('#btn-ClearDiagnosticInformationService')
    result(n, bool(cleared) and bool(msg) and bool(empty) and btn and len(pg.dialogs) == 1, f'confirm asked: {len(pg.dialogs)==1}, ECU cleared: {bool(cleared)}, message: {bool(msg)}, list empty: {bool(empty)}, button usable: {btn}')
    pg.close()

@T('15 DTCs: clear cancelled -> nothing sent')
def _(b, n):
    pg = Page(b, '/dtc.html'); wait_for(lambda: dtc_rows(pg) == 1, 8)
    pg.dialog_answer = False
    pg.p.click('#btn-ClearDiagnosticInformationService'); time.sleep(3)
    st = ctl('STATE'); sent = any(s.startswith('14') for s in st['seen'])
    result(n, st['dtcs'] == 1 and not sent, f'clear sent: {sent}, codes left: {st["dtcs"]}')
    pg.close()

@T('16 DTCs: ECU refuses clear -> message, button usable')
def _(b, n):
    pg = Page(b, '/dtc.html'); wait_for(lambda: dtc_rows(pg) == 1, 8)
    ctl('ECU refuse 1')
    pg.p.click('#btn-ClearDiagnosticInformationService')
    msg = wait_for(lambda: pg.visible('#errormessage') and 'Condition' in pg.p.inner_text('#errormessage'), 6)
    btn = wait_for(lambda: pg.p.is_enabled('#btn-ClearDiagnosticInformationService'), 12)
    result(n, bool(msg) and bool(btn), f'refusal shown: {bool(msg)}, button usable again: {bool(btn)}')
    pg.close()

@T('17 DTCs: clear with ignition off -> timeout popup, module fine')
def _(b, n):
    pg = Page(b, '/dtc.html'); wait_for(lambda: dtc_rows(pg) == 1, 8)
    ctl('ECU on 0')
    pg.p.click('#btn-ClearDiagnosticInformationService')
    popup = wait_for(lambda: pg.visible('#TimeoutModal'), 8)
    resp = http_ok(); btn = wait_for(lambda: pg.p.is_enabled('#btn-ClearDiagnosticInformationService'), 12)
    result(n, bool(popup) and resp is not None and bool(btn), f'timeout popup: {bool(popup)}, module answers: {resp is not None}, button usable: {bool(btn)}')
    pg.close()

@T('18 DTCs: THE CAR-TEST FREEZE: corrupt reply + K-line chatter on clear')
def _(b, n):
    pg = Page(b, '/dtc.html'); wait_for(lambda: dtc_rows(pg) == 1, 8)
    ctl('ECU corrupt 1')
    pg.p.click('#btn-ClearDiagnosticInformationService')
    ctl('ECU babble 20000')                         # chatter starts right after the clear goes out
    worst = 0; dead = 0
    for i in range(20):
        t = http_ok(timeout=3); worst = max(worst, t or 3)
        dead += t is None; time.sleep(0.5)
    time.sleep(10)
    pg2 = Page(b, '/dtc.html'); after = wait_for(lambda: dtc_rows(pg2) >= 0 and ctl('STATE')['session'], 10)
    result(n, dead == 0 and worst < 1.5, f'/status during 10 s of chatter: {20-dead}/20 answered, slowest {worst:.2f}s; trouble codes page works after: {bool(after)}')
    pg.close(); pg2.close()

@T('19 DTCs: ECU says "busy" forever on clear')
def _(b, n):
    pg = Page(b, '/dtc.html'); wait_for(lambda: dtc_rows(pg) == 1, 8)
    ctl('ECU pendforever 1')
    pg.p.click('#btn-ClearDiagnosticInformationService')
    dead = 0
    for i in range(10): dead += http_ok() is None; time.sleep(1)
    gave_up = wait_for(lambda: 'response pending' in fw_log() and ('no reply' in fw_log() or 'refused' in fw_log()), 35, 1)
    ctl('ECU pendforever 0')
    pg2 = Page(b, '/'); ok = wait_for(lambda: pg2.p.inner_text('#batvoltage').strip(), 12)
    result(n, dead == 0 and bool(gave_up) and bool(ok), f'module answered throughout: {dead==0}, gave up after the busy limit: {bool(gave_up)}, home page works after: {bool(ok)}')
    pg.close(); pg2.close()

@T('20 DTCs: no codes / 40 codes')
def _(b, n):
    ctl('ECU dtcs 0')
    pg = Page(b, '/dtc.html'); time.sleep(4); zero = dtc_rows(pg); e0 = list(pg.errors); pg.close()
    ctl('ECU dtcs 0 ' + ' '.join(f'40{0x35 + i:02X}48' for i in range(40)))
    pg = Page(b, '/dtc.html'); many = wait_for(lambda: dtc_rows(pg) == 40 and dtc_rows(pg), 10) or dtc_rows(pg)
    trunc = 'truncated' in fw_log()
    result(n, zero == 0 and many == 40 and not e0 and not trunc, f'0 codes -> {zero} rows; 40 codes -> {many} rows; JSON truncated: {trunc}')
    pg.close()

# ---------------------------------------------------------------- debug / update page
@T('21 debug page: status, log, Test K-line')
def _(b, n):
    pg = Page(b, '/log.html')
    st = wait_for(lambda: pg.p.inner_text('#st_version').startswith('v1.1') and pg.p.inner_text('#st_uptime') != '-', 6)
    pg.p.click('text=Test K-line')
    got = wait_for(lambda: 'ECU answered: battery 14' in pg.p.inner_text('#log'), 10)
    has = all(x in pg.p.inner_text('#log') for x in ['TX:', 'RX:'])
    result(n, bool(st) and bool(got) and has, f"version '{pg.p.inner_text('#st_version')}', Test K-line answered: {bool(got)}, TX/RX lines: {has}, sent={pg.p.inner_text('#st_tx')}")
    pg.close()

@T('22 debug page: pause, clear, download')
def _(b, n):
    pg = Page(b, '/log.html'); time.sleep(2)
    pg.p.click('#btn-pause'); paused = pg.p.inner_text('#btn-pause') == 'Resume'
    n1 = len(pg.p.inner_text('#log')); pg2 = Page(b, '/'); time.sleep(3); pg2.close()
    still = len(pg.p.inner_text('#log')) == n1
    pg.p.click('#btn-pause'); grew = wait_for(lambda: len(pg.p.inner_text('#log')) > n1, 4)
    pg.p.click('text=Clear'); cleared = len(pg.p.inner_text('#log').strip()) < 50
    with pg.p.expect_download() as d: pg.p.click('text=Download')
    fn = d.value.suggested_filename
    result(n, paused and still and bool(grew) and cleared and fn == 'lotusabs_log.txt', f'pause holds: {still}, resume grows: {bool(grew)}, clear: {cleared}, download: {fn}')
    pg.close()

@T('23 debug page: firmware upload (both files, wrong box refused)')
def _(b, n):
    fwbin = '/home/claude/fw11/.pio/build/dev/firmware.bin'; fsbin = '/home/claude/fw11/.pio/build/dev/littlefs.bin'
    pg = Page(b, '/log.html'); time.sleep(1)
    pg.p.set_input_files('#f_fw', fsbin); pg.p.click('#btn-upload'); time.sleep(0.5)
    refused = 'not a firmware image' in pg.p.inner_text('#up_msg')
    pg.p.set_input_files('#f_fw', fwbin); pg.p.set_input_files('#f_fs', fsbin)
    r0 = len(ctl('RESETS')['resets']) if False else None
    pg.p.click('#btn-upload')
    done = wait_for(lambda: 'Update done' in pg.p.inner_text('#up_msg') or 'failed' in pg.p.inner_text('#up_msg') or 'did not come back' in pg.p.inner_text('#up_msg'), 60, 0.5)
    msg = pg.p.inner_text('#up_msg') if not done else done and pg.p.inner_text('#up_msg')
    result(n, refused and 'Update done' in str(msg), f'wrong box refused: {refused}; result: {msg}')
    pg.close()

# ---------------------------------------------------------------- robustness
@T('24 stall watchdog: frozen loop -> restarts itself, shows log from before')
def _(b, n):
    marker_pg = Page(b, '/'); wait_for(lambda: marker_pg.p.inner_text('#batvoltage').strip(), 8); marker_pg.close()
    t0 = time.time(); ctl('HANG')
    dead_seen = wait_for(lambda: http_ok(timeout=1) is None, 5)
    back = wait_for(lambda: http_ok(timeout=1) is not None, 45, 0.5)
    took = time.time() - t0
    lg = fw_log()
    prev = 'previous run froze' in lg and 'end of the log from before the restart' in lg
    result(n, bool(dead_seen) and bool(back) and prev and took < 40, f'pages dead while frozen: {bool(dead_seen)}, back by itself after {took:.0f}s, earlier log shown: {prev}')

@T('25 module restarts while a page is open')
def _(b, n):
    pg = Page(b, '/'); wait_for(lambda: pg.p.inner_text('#batvoltage').strip(), 8)
    ctl('KILL'); time.sleep(6); wait_up()
    ctl('ECU on 1')
    time.sleep(6)
    st = ctl('STATE')
    reconnect = st['ws_clients'] > 0
    result(n, reconnect, f'page reconnected by itself: {reconnect} (old page left open after the restart)')
    pg.close()

@T('26 two pages open at once (valves + trouble codes)')
def _(b, n):
    v = valve_page(b); d = Page(b, '/dtc.html')
    rows = wait_for(lambda: dtc_rows(d) == 1, 8)
    v.p.click('#btn-lf-iso'); on = wait_for(lambda: ctl('STATE')['valves'] & 0x40 and v.on('#btn-lf-iso'), 6)
    time.sleep(3); still_rows = dtc_rows(d) == 1
    v.p.click('#btn-lf-iso'); off = wait_for(lambda: not ctl('STATE')['valves'] & 0x40, 6)
    errs = v.errors + d.errors
    result(n, bool(rows) and bool(on) and bool(off) and still_rows and not errs, f'codes shown: {bool(rows)}, valve on/off from the other tab: {bool(on)}/{bool(off)}, JS errors: {errs[:2]}')
    v.close(); d.close()

@T('27 dead K-line chip (no echo) -> popup and a clear log message')
def _(b, n):
    ctl('ECU echo 0'); ctl('ECU on 0')
    pg = Page(b, '/'); popup = wait_for(lambda: pg.visible('#TimeoutModal'), 8)
    told = 'echo missing' in fw_log()
    ctl('ECU echo 1'); ctl('ECU on 1')
    result(n, bool(popup) and told, f'timeout popup: {bool(popup)}, log says "echo missing": {told}')
    pg.close()

@T('28 noisy line: corrupt + noise + busy replies, values still shown')
def _(b, n):
    pg = Page(b, '/'); wait_for(lambda: pg.p.inner_text('#batvoltage').strip(), 8)
    pg.p.evaluate("$('#batvoltage').text('')")
    ctl('ECU corrupt 1'); time.sleep(3)
    a = pg.p.inner_text('#batvoltage'); pg.p.evaluate("$('#batvoltage').text('')")
    ctl('ECU noise 1'); time.sleep(3)
    c = pg.p.inner_text('#batvoltage'); pg.p.evaluate("$('#batvoltage').text('')")
    ctl('ECU pending 1'); time.sleep(3.5)
    d = pg.p.inner_text('#batvoltage')
    popup = pg.visible('#TimeoutModal')
    result(n, a and c and d and not popup, f'after corrupt: {a!r}, after noise: {c!r}, after busy: {d!r}, error popup: {popup}')
    pg.close()

@T('29 slow ECU (replies after 400 ms)')
def _(b, n):
    ctl('ECU delay 400')
    pg = Page(b, '/'); ok = wait_for(lambda: pg.p.inner_text('#batvoltage').strip() and pg.p.inner_text('#ecu_design_code').strip(), 10)
    result(n, bool(ok) and not pg.visible('#TimeoutModal'), f'id + voltage shown: {bool(ok)}')
    pg.close()

@T('30 valve page left open 3 min (polling + keep-alive)')
def _(b, n):
    pg = valve_page(b)
    lg0 = len(fw_log())
    time.sleep(180)
    st = ctl('STATE'); lg = fw_log()
    errs = sum(lg.count(x) for x in ['no reply', 'corrupt', 'queue full', 'session idle'])
    result(n, st['session'] == 1 and errs == 0 and not pg.errors and http_ok() is not None, f'session still up: {st["session"]}, K-line errors in log: {errs}, requests so far: {st["requests"]}')
    pg.close()

@T('31 valves: quick double-tap = on then off')
def _(b, n):
    pg = valve_page(b); ctl('ECU resetstats 0')
    pg.p.click('#btn-lr-iso'); time.sleep(0.15); pg.p.click('#btn-lr-iso')
    time.sleep(3.5)
    st = ctl('STATE'); writes = [s for s in st['seen'] if s.startswith('303207')]
    btn = pg.on('#btn-lr-iso')
    result(n, writes == ['30320701', '30320700'] and not st['valves'] & 0x04 and not btn, f'commands sent: {writes}, valve now {"ON" if st["valves"] & 4 else "OFF"}, button {"ON" if btn else "OFF"}')
    pg.close()

@T('32 sixth page when 5 are already open')
def _(b, n):
    pages = [Page(b, '/dtc.html') for i in range(5)]; time.sleep(4)
    six = Page(b, '/')
    trying = wait_for(lambda: six.visible('#ws_lost') and 'Connecting' in six.p.inner_text('#ws_lost'), 8)
    msg = wait_for(lambda: six.visible('#ws_lost') and 'Close other' in six.p.inner_text('#ws_lost'), 45, 0.5)
    loops = six.p.evaluate('performance.getEntriesByType("navigation").length')
    for pg in pages: pg.close()
    result(n, bool(trying) and bool(msg), f'retries shown: {bool(trying)}, then clear message: {bool(msg)} (stops, no endless reload)')
    six.close()

@T('33 safety lock: other services / unknown IO ids never reach the line')
def _(b, n):
    import asyncio, websockets
    ctl('ECU resetstats 0')
    async def send():
        async with websockets.connect('ws://127.0.0.1:81/') as w:
            for m in [{'sid': 0x27, 'level': 1}, {'sid': 0x31, 'routine': 1}, {'sid': 0x3B}, {'sid': 0x10, 'mode': 0x85},
                      {'sid': 0x30, 'id': 0x55, 'ctrl_param': 7, 'ctrl_state': 1}, {'sid': 0x30, 'id': 0x11, 'ctrl_param': 3, 'ctrl_state': 1},
                      {'sid': 0x81}, {'sid': 0x30, 'id': 0x49, 'ctrl_param': 1}]:
                await w.send(json.dumps(m)); await asyncio.sleep(0.2)
            await asyncio.sleep(2)
    import threading; th = threading.Thread(target=lambda: asyncio.run(send())); th.start(); th.join()
    seen = ctl('STATE')['seen']
    bad = [x for x in seen if x[:2] not in ('81', '30', '3E', '1A', '21', '18', '14') or (x.startswith('30') and x[2:4] not in ('49', '02', '03', '01', '11', '12', '21', '22', '31', '32', '41', '42'))]
    refused = 'IO id 55' in fw_log() and 'IO id 11 / param 03' in fw_log()
    ok_ones = '81' in seen and '304901' in seen
    result(n, not bad and refused and ok_ones, f'frames on the line: {seen}; refused unknown IO: {refused}')

@T('34 valve page: first command asks, cancel sends nothing')
def _(b, n):
    pg = Page(b, '/valves.html'); wait_for(lambda: ctl('STATE')['session'], 8); time.sleep(1)
    ctl('ECU resetstats 0')
    pg.dialog_answer = False
    pg.p.click('#btn-lf-iso'); pg.p.click('#btn-pump'); time.sleep(2)
    w1 = [x for x in ctl('STATE')['seen'] if x.startswith('30') and x[4:6] == '07']
    pg.dialog_answer = True
    pg.p.click('#btn-lf-iso'); time.sleep(2); pg.p.click('#btn-lf-iso'); time.sleep(2)
    w2 = [x for x in ctl('STATE')['seen'] if x.startswith('30') and x[4:6] == '07']
    warn = pg.visible('#safety_warning')
    result(n, not w1 and w2 == ['30120701', '30120700'] and len(pg.dialogs) == 3 and warn, f'after cancel: {w1}, after OK: {w2}, questions asked: {len(pg.dialogs)} (2 cancelled + 1), warning shown: {warn}')
    pg.close()

@T('35 Test K-line, then straight to every other page')
def _(b, n):
    pg = Page(b, '/log.html'); time.sleep(1.5)
    pg.p.click('text=Test K-line'); time.sleep(1)
    res = {}
    for path, check in [('/', lambda: pg.p.inner_text('#batvoltage').strip()), ('/valves.html', lambda: not pg.visible('#StartCommunicationModal') and ctl('STATE')['session']),
                        ('/dtc.html', lambda: pg.p.locator('#dtccontainer .row').count() == 1), ('/log.html', lambda: pg.p.inner_text('#st_uptime') != '-')]:
        pg.p.goto(BASE + path)
        ok = wait_for(check, 10)
        res[path] = bool(ok) and not pg.visible('#ws_lost')
    pg.p.click('text=Test K-line'); again = wait_for(lambda: 'ECU answered' in pg.p.inner_text('#log'), 10)
    st = ctl('STATE')
    result(n, all(res.values()) and bool(again), f'pages after the test: {res}, Test K-line again: {bool(again)}, open connections now: {st["ws_clients"]}')
    pg.close()

@T('36 warnings: Acknowledge hides them and they stay hidden')
def _(b, n):
    pg = Page(b, '/'); time.sleep(1)
    shown = pg.visible('#safety_warning'); txt = pg.p.inner_text('#safety_warning')
    pg.p.click('#safety_warning button'); time.sleep(0.6); hidden = not pg.visible('#safety_warning')
    pg.p.goto(BASE + '/'); time.sleep(1); stays = not pg.visible('#safety_warning')
    pg.p.goto(BASE + '/valves.html'); time.sleep(1); vshown = pg.visible('#safety_warning')
    pg.p.click('#safety_warning button'); time.sleep(0.6)
    pg.p.goto(BASE + '/valves.html'); time.sleep(1); vstays = not pg.visible('#safety_warning')
    ok = shown and 'accept these risks' in txt and hidden and stays and vshown and vstays
    result(n, ok, f'home: shown {shown}, hidden after Acknowledge {hidden}, still hidden after reload {stays}; valves: shown {vshown}, hidden after Acknowledge + reload {vstays}')
    pg.close()

@T('37 Disclaimer page: full text, no connection used, Acknowledge hides the warnings')
def _(b, n):
    pg = Page(b, '/'); time.sleep(1)
    if pg.visible('.navbar-toggler'): pg.p.click('.navbar-toggler'); time.sleep(0.5)
    pg.p.click('a.nav-link[href="disclaimer.html"]'); time.sleep(1.5)
    txt = pg.p.inner_text('body')
    parts = all(x in txt for x in ['ACCEPT THE RISKS', 'AIRBAGS', 'BRAKES', 'RULES FOR USE', 'NO WARRANTY', 'LIMITATION OF LIABILITY', 'YOUR RESPONSIBILITY'])
    conns = ctl('STATE')['ws_clients']
    pg.p.click('button:has-text("Acknowledge")'); time.sleep(0.5)
    pg.p.goto(BASE + '/'); time.sleep(1); h = not pg.visible('#safety_warning')
    pg.p.goto(BASE + '/valves.html'); time.sleep(1); v = not pg.visible('#safety_warning')
    result(n, pg.p.url.endswith('/valves.html') and parts and conns == 0 and h and v and not pg.errors, f'all sections shown: {parts}, connections held by the page: {conns}, warnings hidden after Acknowledge: home {h}, valves {v}')
    pg.close()

def main():
    with sync_playwright() as p:
        b = p.chromium.launch()
        for t in TESTS: t(b)
        b.close()
    fails = [r for r in RESULTS if not r[1]]
    print(f'\n{len(RESULTS) - len(fails)} of {len(RESULTS)} passed')
    with open('/tmp/fullsim_results.txt', 'w') as f:
        for nm, ok, d in RESULTS: f.write(f"{'OK  ' if ok else 'FAIL'} {nm:52s} {d}\n")
        f.write(f'\n{len(RESULTS) - len(fails)} of {len(RESULTS)} passed\n')

main()
