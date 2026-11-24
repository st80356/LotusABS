#!/usr/bin/env python3
"""Bridge between a browser and the natively built LotusABS firmware (fw_native).
HTTP on :80 (static pages from data/www, dynamic URLs forwarded to the firmware),
WebSocket on :81 (forwarded to the firmware's websocket handler), control API on :8099
(scenario commands for the simulated ECU, firmware restarts). Restarts the firmware like a
real module reboots: after a watchdog reset (exit 42) or an OTA upload."""
import asyncio, os, sys, json, mimetypes
import websockets

WWW = sys.argv[1]
FW = sys.argv[2]
RTC = '/tmp/lotus_rtc.bin'
LOG = open('/tmp/bridge.log', 'a')

proc = None
up = False
http_waiters = {}
state_waiters = []
ws_conns = {}          # num -> websocket
next_id = 1
resets = []

def log(*a):
    print(*a, file=LOG, flush=True)

async def send_fw(line):
    if proc and proc.returncode is None and up:
        proc.stdin.write((line + '\n').encode()); await proc.stdin.drain()

async def fw_reader(p):
    global up
    while True:
        l = await p.stdout.readline()
        if not l: break
        l = l.decode().rstrip('\n')
        parts = l.split(' ', 3)
        if parts[0] == 'READY':
            up = True; log('firmware up')
        elif parts[0] == 'WS':
            num = int(parts[1]); msg = bytes.fromhex(parts[2]).decode()
            targets = list(ws_conns.values()) if num < 0 else [ws_conns.get(num)]
            log('WS out', num, len(targets), msg[:60])
            for w in targets:
                if w is not None:
                    try: await w.send(msg)
                    except Exception as e: log('WS send failed', repr(e))
        elif parts[0] == 'HTTP':
            hid = int(parts[1]); code = int(parts[2]); rest = parts[3].split(' ', 1)
            fut = http_waiters.pop(hid, None)
            if fut and not fut.done(): fut.set_result((code, rest[0], bytes.fromhex(rest[1]) if len(rest) > 1 else b''))
        elif parts[0] == 'STATE':
            st = json.loads(l[6:])
            while state_waiters:
                f = state_waiters.pop(0)
                if not f.done(): f.set_result(st)
        elif parts[0] == 'RESET':
            resets.append(parts[1]); log('RESET', parts[1])

async def run_fw():
    global proc, up
    while True:
        up = False
        proc = await asyncio.create_subprocess_exec(FW, RTC, stdin=asyncio.subprocess.PIPE, stdout=asyncio.subprocess.PIPE)
        reader = asyncio.create_task(fw_reader(proc))
        rc = await proc.wait()
        up = False
        log('firmware exited', rc)
        for w in list(ws_conns.values()):          # a reboot drops every connection
            try: await w.close()
            except Exception: pass
        ws_conns.clear()
        await asyncio.sleep(3.0)                     # boot time

restart_req = asyncio.Event() if False else None

async def kill_fw(after=0.3):
    await asyncio.sleep(after)
    if proc and proc.returncode is None: proc.kill()

# ---------------- HTTP ----------------
STATIC = {'/': 'index.html', '/index.html': 'index.html', '/valves.html': 'valves.html', '/dtc.html': 'dtc.html', '/log.html': 'log.html', '/disclaimer.html': 'disclaimer.html'}
async def http_handler(reader, writer):
    global next_id
    try:
        head = await reader.readuntil(b'\r\n\r\n')
        lines = head.decode(errors='replace').split('\r\n')
        method, path, _ = lines[0].split(' ', 2)
        hdrs = {k.lower(): v.strip() for k, v in (x.split(':', 1) for x in lines[1:] if ':' in x)}
        body = b''
        if 'content-length' in hdrs: body = await reader.readexactly(int(hdrs['content-length']))
        if not up:
            writer.close(); return
        bare = path.split('?')[0]
        if method == 'POST' and bare == '/update':
            ok = b'name="firmware"' in body[:400] or b'name="filesystem"' in body[:400]
            resp = (200, 'text/html', b'<META http-equiv="refresh" content="15;URL=/">Update Success! Rebooting...' if ok else b'Update error: no file')
            if ok: asyncio.create_task(kill_fw(0.5))
        elif bare in STATIC or bare.startswith(('/js/', '/css/', '/img/')):
            fn = os.path.join(WWW, STATIC.get(bare, bare.lstrip('/')))
            if os.path.isfile(fn):
                resp = (200, mimetypes.guess_type(fn)[0] or 'application/octet-stream', open(fn, 'rb').read())
            else: resp = (404, 'text/plain', b'not found')
        elif bare == '/update':
            resp = (200, 'text/html', b'<html><body>ElegantOTA page (not simulated)</body></html>')
        else:
            hid = next_id; next_id += 1
            fut = asyncio.get_event_loop().create_future(); http_waiters[hid] = fut
            await send_fw(f'HTTP {hid} {path}')
            try: resp = await asyncio.wait_for(fut, 3.0)
            except asyncio.TimeoutError:
                http_waiters.pop(hid, None); writer.close(); return     # module not answering: browser sees a dead connection
        code, ctype, data = resp
        writer.write(f'HTTP/1.1 {code} OK\r\nContent-Type: {ctype}\r\nContent-Length: {len(data)}\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n'.encode() + data)
        await writer.drain()
    except Exception as e:
        log('http error', repr(e))
    finally:
        try: writer.close()
        except Exception: pass

# ---------------- WebSocket ----------------
async def ws_handler(ws):
    if not up:
        await ws.close(); return
    free = [n for n in range(5) if n not in ws_conns]
    log('WS connect, open now:', sorted(ws_conns))
    if not free:                     # like the ESP8266 WebSockets library: 5 clients max, the 6th is dropped
        await ws.close(); return
    num = free[0]
    ws_conns[num] = ws
    await send_fw(f'WSCONN {num}')
    try:
        async for msg in ws:
            log('WS in', num, msg[:60])
            if isinstance(msg, str): await send_fw(f'WSTEXT {num} {msg.encode().hex()}')
    except Exception: pass
    finally:
        if ws_conns.get(num) is ws:
            del ws_conns[num]
            log('WS closed', num)
            await send_fw(f'WSDISC {num}')

# ---------------- control API (tests) ----------------
async def ctl_handler(reader, writer):
    try:
        head = await reader.readuntil(b'\r\n\r\n')
        lines = head.decode().split('\r\n')
        n = int(next((x.split(':')[1] for x in lines if x.lower().startswith('content-length')), '0'))
        cmd = (await reader.readexactly(n)).decode().strip()
        res = {}
        if cmd == 'RESETS': res = {'resets': resets, 'up': up}
        elif cmd == 'KILL': await kill_fw(0); res = {'ok': True}
        elif cmd == 'UP': res = {'up': up}
        else:
            if up:
                f = asyncio.get_event_loop().create_future(); state_waiters.append(f)
                await send_fw(cmd if cmd.startswith(('ECU', 'HANG')) else cmd)
                if not cmd.startswith('HANG'):
                    if not cmd.startswith('ECU'): await send_fw('STATE')
                    try: res = await asyncio.wait_for(f, 3.0)
                    except asyncio.TimeoutError: res = {'error': 'firmware not answering'}
            else: res = {'error': 'firmware down'}
        data = json.dumps(res).encode()
        writer.write(b'HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: %d\r\nConnection: close\r\n\r\n' % len(data) + data)
        await writer.drain()
    finally:
        writer.close()

async def main():
    asyncio.create_task(run_fw())
    await asyncio.start_server(http_handler, '127.0.0.1', 80)
    await asyncio.start_server(ctl_handler, '127.0.0.1', 8099)
    def full(conn, req):            # ESP8266 WebSockets library: no free slot -> connection dropped before the handshake
        if len(ws_conns) >= 5 or not up:
            return conn.respond(503, 'no free slot')
        return None
    async with websockets.serve(ws_handler, '127.0.0.1', 81, process_request=full):
        await asyncio.Future()

asyncio.run(main())
