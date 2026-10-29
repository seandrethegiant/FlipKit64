#!/usr/bin/env python3
"""FlipKit64 MIDI bridge for Chromebooks.

ChromeOS keeps USB MIDI controllers for itself, so Linux apps can't see them.
Chrome can, through Web MIDI. This bridge serves a small page at
http://localhost:8765 that reads your controllers in Chrome and relays their
MIDI to a virtual port in Linux called "Chromebook MIDI", which FlipKit64 uses
like a normal MIDI input.

Needs only Python 3 and python3-rtmidi. No other libraries.
"""
import asyncio, base64, hashlib, struct, sys

import rtmidi

PORT = 8765
PORT_NAME = "Chromebook MIDI"
GUID = b"258EAFA5-E914-47DA-95CA-C5AB0DC85B11"

PAGE = r"""<!doctype html>
<html><head><meta charset="utf-8"><title>FlipKit64 MIDI bridge</title>
<meta name="viewport" content="width=device-width,initial-scale=1">
<style>
 :root{color-scheme:dark}
 body{margin:0;background:#150f22;color:#f6f3fb;font:16px/1.5 system-ui,sans-serif}
 main{max-width:560px;margin:0 auto;padding:28px 16px}
 h1{font-size:22px;margin:0 0 4px;color:#ff8c2e}
 p{color:#9d92b8;margin:0 0 18px}
 .card{background:#211836;border:1px solid #3a2c58;border-radius:10px;padding:14px 16px;margin-bottom:12px}
 .row{display:flex;justify-content:space-between;gap:12px;padding:6px 0}
 .ok{color:#1ad9fc}.bad{color:#ff3d5a}.dim{color:#9d92b8}
 #last{font-family:ui-monospace,monospace;color:#fee150;min-height:1.5em}
 button{background:#ff8c2e;color:#150f22;border:0;border-radius:8px;padding:10px 16px;font-weight:600;font-size:15px}
</style></head><body><main>
<h1>FlipKit64 MIDI bridge</h1>
<p>Keep this tab open while you use FlipKit64. Your controllers are relayed to Linux as <b>"Chromebook MIDI"</b>.</p>
<div class="card"><div class="row"><span>Link to Linux</span><span id="ws" class="dim">connecting...</span></div>
<div class="row"><span>MIDI access</span><span id="midi" class="dim">waiting...</span></div></div>
<div class="card"><b>Controllers</b><div id="inputs" class="dim">none yet - plug one in</div></div>
<div class="card"><b>Last message</b><div id="last"></div><div class="dim" id="count">0 messages relayed</div></div>
<button id="go" hidden>Allow MIDI access</button>
</main><script>
let ws, sent = 0, access;
const $ = id => document.getElementById(id);
function connect(){
  ws = new WebSocket('ws://' + location.host + '/midi');
  ws.binaryType = 'arraybuffer';
  ws.onopen = () => { $('ws').textContent = 'connected'; $('ws').className = 'ok'; };
  ws.onclose = () => { $('ws').textContent = 'not running - start FlipKit64'; $('ws').className = 'bad'; setTimeout(connect, 1500); };
}
function onMsg(e){
  if (e.data[0] >= 0xF8) return;            // skip clock / active sensing
  if (ws && ws.readyState === 1) { ws.send(e.data); sent++; }
  $('last').textContent = [...e.data].map(b => b.toString(16).padStart(2,'0')).join(' ');
  $('count').textContent = sent + ' messages relayed';
}
function refresh(){
  const names = [];
  for (const inp of access.inputs.values()) { inp.onmidimessage = onMsg; names.push(inp.name); }
  $('inputs').innerHTML = names.length ? names.map(n => '<div class="row"><span>' + n.replace(/</g,'&lt;') + '</span><span class="ok">relaying</span></div>').join('') : 'none yet - plug one in';
  $('inputs').className = names.length ? '' : 'dim';
}
async function start(){
  try {
    access = await navigator.requestMIDIAccess({ sysex: false });
    $('midi').textContent = 'allowed'; $('midi').className = 'ok'; $('go').hidden = true;
    access.onstatechange = refresh; refresh();
  } catch (err) {
    $('midi').textContent = 'blocked - click the button'; $('midi').className = 'bad'; $('go').hidden = false;
  }
}
$('go').onclick = start;
connect(); start();
</script></body></html>
"""


def ws_frame_parse(buf):
    """Return (opcode, payload, consumed) for one complete frame, or None if incomplete."""
    if len(buf) < 2:
        return None
    b0, b1 = buf[0], buf[1]
    opcode, masked, ln = b0 & 0x0F, b1 & 0x80, b1 & 0x7F
    i = 2
    if ln == 126:
        if len(buf) < 4: return None
        ln = struct.unpack(">H", buf[2:4])[0]; i = 4
    elif ln == 127:
        if len(buf) < 10: return None
        ln = struct.unpack(">Q", buf[2:10])[0]; i = 10
    mask = b""
    if masked:
        if len(buf) < i + 4: return None
        mask = buf[i:i + 4]; i += 4
    if len(buf) < i + ln:
        return None
    data = bytearray(buf[i:i + ln])
    if masked:
        for k in range(ln):
            data[k] ^= mask[k & 3]
    return opcode, bytes(data), i + ln


class Bridge:
    def __init__(self, out):
        self.out = out

    async def handle(self, reader, writer):
        try:
            head = await reader.readuntil(b"\r\n\r\n")
        except Exception:
            writer.close(); return
        lines = head.decode("latin-1").split("\r\n")
        path = lines[0].split(" ")[1] if len(lines[0].split(" ")) > 1 else "/"
        hdrs = {l.split(":", 1)[0].strip().lower(): l.split(":", 1)[1].strip() for l in lines[1:] if ":" in l}

        if hdrs.get("upgrade", "").lower() == "websocket" and path.startswith("/midi"):
            key = hdrs.get("sec-websocket-key", "").encode()
            acc = base64.b64encode(hashlib.sha1(key + GUID).digest()).decode()
            writer.write(("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                          f"Sec-WebSocket-Accept: {acc}\r\n\r\n").encode())
            await writer.drain()
            print("Chrome connected", flush=True)
            buf = b""
            try:
                while True:
                    chunk = await reader.read(4096)
                    if not chunk:
                        break
                    buf += chunk
                    while True:
                        fr = ws_frame_parse(buf)
                        if fr is None:
                            break
                        opcode, data, used = fr
                        buf = buf[used:]
                        if opcode == 0x2 and data:        # binary frame = one MIDI message
                            self.out.send_message(list(data))
                        elif opcode == 0x8:               # close
                            raise ConnectionError
                        elif opcode == 0x9:               # ping -> pong
                            writer.write(bytes([0x8A, len(data)]) + data)
            except (ConnectionError, asyncio.IncompleteReadError):
                pass
            print("Chrome disconnected", flush=True)
            writer.close()
            return

        body = PAGE.encode()
        writer.write(b"HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nCache-Control: no-store\r\n"
                     + f"Content-Length: {len(body)}\r\nConnection: close\r\n\r\n".encode() + body)
        await writer.drain()
        writer.close()


async def main():
    try:
        out = rtmidi.MidiOut(rtmidi.API_LINUX_ALSA)
        out.open_virtual_port(PORT_NAME)
    except Exception as e:
        print("Could not open the Linux MIDI system:", e)
        print("Fix: run  sudo usermod -aG audio $USER  then shut down Linux and open it again.")
        sys.exit(1)
    print(f'Virtual MIDI port "{PORT_NAME}" is ready.', flush=True)
    server = await asyncio.start_server(Bridge(out).handle, "0.0.0.0", PORT)
    print(f"Open http://localhost:{PORT} in Chrome and keep that tab open.", flush=True)
    async with server:
        await server.serve_forever()


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except OSError as e:
        if "Address already in use" in str(e):
            print("The MIDI bridge is already running.")
            sys.exit(0)
        raise
    except KeyboardInterrupt:
        pass
