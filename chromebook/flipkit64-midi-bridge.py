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
<p>Keep this tab open while you use FlipKit64. Your controllers are relayed to Linux as <b>"Chromebook MIDI"</b>. If Chrome asks to let this page control and reprogram your MIDI devices, allow it: that is what carries the TempoPAD's Play, Stop and Record buttons.</p>
<div class="card"><div class="row"><span>Link to Linux</span><span id="ws" class="dim">connecting...</span></div>
<div class="row"><span>MIDI access</span><span id="midi" class="dim">waiting...</span></div></div>
<div class="card"><b>Controllers</b><div id="inputs" class="dim">none yet - plug one in</div></div>
<div class="card"><b>Last message</b><div id="last"></div><div class="dim" id="count">0 messages relayed</div></div>
<button id="go" hidden>Allow MIDI access</button>
<p style="margin-top:18px"><a href="/monitor" target="_blank" style="color:#ff8c2e">Open the button tester</a> - see exactly what each pad and button sends.</p>
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
    // SysEx carries the TempoPAD's transport (MMC) and Easy-mode pads, so ask for it first.
    try { access = await navigator.requestMIDIAccess({ sysex: true }); $('midi').textContent = 'allowed'; }
    catch (e1) { access = await navigator.requestMIDIAccess({ sysex: false }); $('midi').textContent = 'allowed without SysEx (TempoPAD Play/Stop/Record and Easy mode will not work)'; }
    $('midi').className = 'ok'; $('go').hidden = true;
    access.onstatechange = refresh; refresh();
  } catch (err) {
    $('midi').textContent = 'blocked - click the button'; $('midi').className = 'bad'; $('go').hidden = false;
  }
}
$('go').onclick = start;
connect(); start();
</script></body></html>
"""


MONITOR = r"""<!doctype html>
<html><head><meta charset="utf-8"><title>FlipKit64 button tester</title>
<meta name="viewport" content="width=device-width,initial-scale=1">
<style>
 :root{color-scheme:dark}
 *{box-sizing:border-box}
 body{margin:0;background:#150f22;color:#f6f3fb;font:15px/1.45 system-ui,sans-serif}
 main{max-width:880px;margin:0 auto;padding:22px 16px 40px}
 h1{font-size:22px;margin:0 0 2px;color:#ff8c2e}
 h2{font-size:15px;margin:0 0 8px}
 p{color:#9d92b8;margin:0 0 14px}
 .card{background:#211836;border:1px solid #3a2c58;border-radius:10px;padding:14px;margin-bottom:12px}
 .ok{color:#1ad9fc}.bad{color:#ff3d5a}.dim{color:#9d92b8}
 .chips{display:flex;flex-wrap:wrap;gap:6px}
 .chip{background:#2a2040;color:#f6f3fb;border:1px solid #3a2c58;border-radius:999px;padding:6px 11px;font-size:13px;cursor:pointer}
 .chip.sel{background:#ff8c2e;color:#150f22;border-color:#ff8c2e;font-weight:600}
 .chip.got::after{content:" \2713";color:#1ad9fc}
 .chip.sel.got::after,.chip.sel.none::after{color:#150f22}
 .chip.none::after{content:" \2014 nothing";color:#ff3d5a}
 button.act{background:#ff8c2e;color:#150f22;border:0;border-radius:8px;padding:9px 14px;font-weight:600;font-size:14px;cursor:pointer}
 button.sec{background:#2a2040;color:#f6f3fb;border:1px solid #3a2c58;border-radius:8px;padding:9px 14px;font-size:14px;cursor:pointer}
 .bar{display:flex;flex-wrap:wrap;gap:8px;align-items:center}
 select,input{background:#150f22;color:#f6f3fb;border:1px solid #3a2c58;border-radius:6px;padding:7px 8px;font:inherit}
 table{width:100%;border-collapse:collapse;font-size:13px}
 td,th{text-align:left;padding:5px 6px;border-bottom:1px solid #2c2244;vertical-align:top}
 th{color:#9d92b8;font-weight:600}
 td.hex{font-family:ui-monospace,monospace;color:#fee150;white-space:nowrap}
 .logwrap{max-height:420px;overflow:auto}
 #now{font-weight:600;color:#ff8c2e}
</style></head><body><main>
<h1>Button tester</h1>
<p>Pick what you're about to press, then press it on the controller. Everything it sends shows up below. When you're done, press <b>Copy results</b> and paste them to Claude.</p>

<div class="card">
 <div class="bar" style="justify-content:space-between"><span>MIDI access: <span id="midi" class="dim">waiting...</span></span><span id="ins" class="dim"></span></div>
 <div class="bar" style="margin-top:8px"><button class="act" id="go" hidden>Allow MIDI access</button></div>
</div>

<div class="card">
 <h2>1. What are you pressing? <span class="dim" style="font-weight:400">Now testing: <span id="now">-</span></span></h2>
 <div class="dim" style="margin:6px 0 4px">Top row</div>
 <div class="chips" id="top"></div>
 <div class="dim" style="margin:10px 0 4px">Right column</div>
 <div class="chips" id="side"></div>
 <div class="dim" style="margin:10px 0 4px">Pads (press a few: top-left, bottom-left, top-right, bottom-right)</div>
 <div class="chips" id="pads"></div>
 <div class="bar" style="margin-top:12px"><input id="custom" placeholder="Something else (type a label)" style="flex:1;min-width:200px"><button class="sec" id="setcustom">Use label</button></div>
 <p style="margin:10px 0 0">If you press a button and nothing shows up, click it again here to mark it as <span class="bad">nothing</span>. That's useful too.</p>
</div>

<div class="card">
 <div class="bar" style="justify-content:space-between;margin-bottom:8px">
  <h2 style="margin:0">2. What it sent <span class="dim" id="count" style="font-weight:400"></span></h2>
  <div class="bar"><button class="act" id="copy">Copy results</button><button class="sec" id="clear">Clear</button></div>
 </div>
 <div class="dim" id="clock" style="margin-bottom:6px"></div>
 <div class="logwrap"><table><thead><tr><th>Testing</th><th>Message</th><th>Bytes</th><th>From</th></tr></thead><tbody id="log"></tbody></table></div>
</div>

<div class="card">
 <h2>3. Can FlipKit64 light the pads?</h2>
 <p>This sends messages back to the controller. Tell Claude what the pads do for each test (nothing, some light up, colours change...). Try it in Drum mode first, then Custom.</p>
 <div class="bar" style="margin-bottom:10px">
  <label>Send to <select id="out"></select></label>
  <label>Channel <select id="ch"></select></label>
  <label>As <select id="kind"><option value="note">Note On</option><option value="cc">CC</option></select></label>
 </div>
 <div class="bar">
  <button class="sec" id="sweep">Light notes 36-99 (colour sweep)</button>
  <button class="sec" id="cycle">One pad, every value</button>
  <button class="sec" id="alloff">All off</button>
 </div>
 <div class="dim" id="ledstat" style="margin-top:8px"></div>
 <div class="bar" style="margin-top:12px"><input id="raw" placeholder="Raw bytes, e.g. 90 24 7f" style="flex:1;min-width:200px;font-family:ui-monospace,monospace"><button class="sec" id="sendraw">Send</button></div>
</div>
</main><script>
const $ = id => document.getElementById(id);
const TOP = ['▼','▲','Mood','Easy','Drum','Key','User','Custom'];
const SIDE = ['Octave','Transpose','Velocity','Light','MMC','Stop','Play','Record'];
const PADS = ['Pads in Easy','Pads in Drum','Pads in Key','Pads in User','Pads in Custom'];
const NOTES = ['C','C#','D','D#','E','F','F#','G','G#','A','A#','B'];
const MMC = {1:'Stop',2:'Play',3:'Deferred play',4:'Fast forward',5:'Rewind',6:'Record',7:'Record exit',8:'Record pause',9:'Pause',0x0A:'Eject',0x44:'Locate'};
let access, label = '', rows = [], clocks = 0, lastClock = 0, chips = {}, cycleTimer = null;

function chipRow(el, names){
  for (const n of names){
    const b = document.createElement('button'); b.className = 'chip'; b.textContent = n;
    b.onclick = () => {
      if (label === n && !b.classList.contains('got')) { b.classList.toggle('none'); return; }
      setLabel(n);
    };
    el.appendChild(b); chips[n] = b;
  }
}
function setLabel(n){
  label = n; $('now').textContent = n || '-';
  for (const k in chips) chips[k].classList.toggle('sel', k === n);
}
chipRow($('top'), TOP); chipRow($('side'), SIDE); chipRow($('pads'), PADS);
$('setcustom').onclick = () => { const v = $('custom').value.trim(); if (v) setLabel(v); };

function noteName(n){ return NOTES[n % 12] + (Math.floor(n / 12) - 1); }
function decode(d){
  const s = d[0], hi = s & 0xF0, ch = (s & 0x0F) + 1;
  if (s === 0xFA) return 'Clock START';
  if (s === 0xFB) return 'Clock CONTINUE';
  if (s === 0xFC) return 'Clock STOP';
  if (s === 0xF0){
    if (d[1] === 0x7F && d[3] === 0x06) return 'MMC ' + (MMC[d[4]] || ('command ' + d[4]));
    return 'SysEx (' + d.length + ' bytes)';
  }
  if (hi === 0x90 && d[2] > 0) return 'Note On ' + d[1] + ' (' + noteName(d[1]) + ') vel ' + d[2] + ' ch ' + ch;
  if (hi === 0x80 || hi === 0x90) return 'Note Off ' + d[1] + ' (' + noteName(d[1]) + ') ch ' + ch;
  if (hi === 0xB0) return 'CC ' + d[1] + ' = ' + d[2] + ' ch ' + ch;
  if (hi === 0xC0) return 'Program ' + d[1] + ' ch ' + ch;
  if (hi === 0xA0) return 'Pressure on note ' + d[1] + ' = ' + d[2] + ' ch ' + ch;
  if (hi === 0xD0) return 'Channel pressure ' + d[1] + ' ch ' + ch;
  if (hi === 0xE0) return 'Pitch bend ' + (((d[2] << 7) | d[1]) - 8192) + ' ch ' + ch;
  return 'Other';
}
const hex = d => [...d].map(b => b.toString(16).padStart(2, '0')).join(' ');
function esc(s){ return String(s).replace(/[&<>]/g, c => ({'&':'&amp;','<':'&lt;','>':'&gt;'}[c])); }

function onMsg(e){
  const d = e.data;
  if (d[0] === 0xFE) return;                      // active sensing
  if (d[0] === 0xF8){ clocks++; lastClock = Date.now(); return; }
  const r = { label: label || '(nothing picked)', text: decode(d), hex: hex(d), from: e.target.name };
  rows.push(r); if (rows.length > 2000) rows.shift();
  if (label && chips[label]) { chips[label].classList.add('got'); chips[label].classList.remove('none'); }
  const tr = document.createElement('tr');
  tr.innerHTML = '<td>' + esc(r.label) + '</td><td>' + esc(r.text) + '</td><td class="hex">' + r.hex + '</td><td class="dim">' + esc(r.from) + '</td>';
  const tb = $('log'); tb.insertBefore(tr, tb.firstChild);
  while (tb.children.length > 400) tb.removeChild(tb.lastChild);
  $('count').textContent = '(' + rows.length + ')';
}
setInterval(() => {
  $('clock').textContent = clocks ? ('MIDI clock: ' + (Date.now() - lastClock < 1500 ? 'the controller is sending clock right now' : 'received earlier') + ' (' + clocks + ' ticks)') : '';
}, 500);

function summary(){
  const lines = ['FlipKit64 button tester results', ''];
  const marked = Object.keys(chips).filter(k => chips[k].classList.contains('none'));
  if (marked.length) lines.push('Sent nothing: ' + marked.join(', '), '');
  if (clocks) lines.push('MIDI clock ticks received: ' + clocks, '');
  let prev = null;
  for (const r of rows){
    if (r.label !== prev){ lines.push('[' + r.label + ']'); prev = r.label; }
    lines.push('  ' + r.text + '   | ' + r.hex + '   | ' + r.from);
  }
  return lines.join('\n');
}
$('copy').onclick = async () => {
  const t = summary();
  try { await navigator.clipboard.writeText(t); $('copy').textContent = 'Copied!'; }
  catch (err) {
    const ta = document.createElement('textarea'); ta.value = t; document.body.appendChild(ta); ta.select();
    document.execCommand('copy'); ta.remove(); $('copy').textContent = 'Copied!';
  }
  setTimeout(() => $('copy').textContent = 'Copy results', 1500);
};
$('clear').onclick = () => { rows = []; $('log').innerHTML = ''; $('count').textContent = ''; clocks = 0;
  for (const k in chips) chips[k].classList.remove('got', 'none'); };

for (let c = 1; c <= 16; c++){ const o = document.createElement('option'); o.value = c - 1; o.textContent = c; $('ch').appendChild(o); }
function out(){ return access && access.outputs.get($('out').value); }
function send(n, v){
  const o = out(); if (!o) { $('ledstat').textContent = 'No output picked - plug in the controller.'; return false; }
  const ch = +$('ch').value;
  o.send($('kind').value === 'cc' ? [0xB0 | ch, n, v] : [0x90 | ch, n, v]);
  return true;
}
function stopCycle(){ if (cycleTimer) { clearInterval(cycleTimer); cycleTimer = null; } }
$('sweep').onclick = () => {
  stopCycle(); let ok = true;
  for (let i = 0; i < 64 && ok; i++) ok = send(36 + i, (i * 2) + 1);
  if (ok) $('ledstat').textContent = 'Sent notes 36-99 with values 1-127. Did any pads change?';
};
$('cycle').onclick = () => {
  stopCycle(); let v = 0;
  cycleTimer = setInterval(() => {
    if (!send(36, v)) { stopCycle(); return; }
    $('ledstat').textContent = 'Note/CC 36, value ' + v + ' of 127 (watch the bottom-left pad)';
    if (++v > 127) { stopCycle(); $('ledstat').textContent = 'Done: went through values 0-127 on note/CC 36.'; }
  }, 120);
};
$('alloff').onclick = () => {
  stopCycle(); let ok = true;
  for (let i = 0; i < 64 && ok; i++) ok = send(36 + i, 0);
  if (ok) $('ledstat').textContent = 'Sent value 0 to notes 36-99.';
};
$('sendraw').onclick = () => {
  const o = out(); if (!o) return;
  try { const b = $('raw').value.trim().split(/[\s,]+/).map(x => parseInt(x, 16));
        if (b.some(isNaN)) throw 0; o.send(b); $('ledstat').textContent = 'Sent ' + hex(b); }
  catch (err) { $('ledstat').textContent = "Couldn't send that - use hex bytes like 90 24 7f"; }
};

function refresh(){
  const names = [];
  for (const i of access.inputs.values()) { i.onmidimessage = onMsg; names.push(i.name); }
  $('ins').textContent = names.length ? 'Listening to: ' + names.join(', ') : 'No controller found - plug it in';
  const sel = $('out'), keep = sel.value; sel.innerHTML = '';
  for (const o of access.outputs.values()){ const op = document.createElement('option'); op.value = o.id; op.textContent = o.name; sel.appendChild(op); }
  if (keep && access.outputs.get(keep)) sel.value = keep;
}
async function start(){
  try {
    try { access = await navigator.requestMIDIAccess({ sysex: true }); $('midi').textContent = 'allowed'; }
    catch (e1) { access = await navigator.requestMIDIAccess({ sysex: false }); $('midi').textContent = 'allowed (without SysEx - MMC may not show)'; }
    $('midi').className = 'ok'; $('go').hidden = true;
    access.onstatechange = refresh; refresh();
  } catch (err) {
    $('midi').textContent = 'blocked - click the button'; $('midi').className = 'bad'; $('go').hidden = false;
  }
}
$('go').onclick = start;
start();
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
        self.clients = 0          # connected bridge tabs (the launcher asks via /status)

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
            self.clients += 1
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
            self.clients -= 1
            print("Chrome disconnected", flush=True)
            writer.close()
            return

        if path.startswith("/status"):
            body = str(self.clients).encode()
            writer.write(b"HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n"
                         + f"Content-Length: {len(body)}\r\nConnection: close\r\n\r\n".encode() + body)
            await writer.drain()
            writer.close()
            return

        body = (MONITOR if path.startswith("/monitor") else PAGE).encode()
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
