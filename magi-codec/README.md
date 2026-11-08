# Magi Codec: phone call alerts

Phone alerts for the Magi dashboard. You get a push notification that says
**INCOMING CALL**, and tapping it opens a full-screen call page with a
portrait, a frequency readout, a ring and typed-out text. The first alert is
"3D conversion finished" from StereoServe. Disk-low alerts work too, and the
camera-motion intake route is ready for later.

This folder is a **drop-in module**. Magi's own code wasn't available when it
was built, so it plugs into any Flask app and can also run on its own for testing.

```
magi-codec/
  magi_codec/            Python package (copy into Magi)
    __init__.py          init_codec(app)
    service.py           core logic (no Flask): subscriptions, calls, push, rate limits
    routes.py            Flask blueprint (all HTTP routes)
    disk_monitor.py      free-space check every 5 min
    config.py            data dir, config, VAPID key generation
    callers.json         caller names, frequencies, portraits  <- edit me
  static/codec/          front end (copy into Magi next to magi_codec/)
    sw.js, manifest.webmanifest   served at /sw.js and /manifest.webmanifest
    codec-panel.js/.css  dashboard panel (enable button, test call, call log)
    call.html/.js/.css   the call page, /codec/<callId>
    codec-audio.js       ring / blip / connect / static sounds
    icons/, portraits/   original placeholder art
    sounds/              optional: your own sound files (git-ignored)
  tools/codec_notify.py  helper for StereoServe (stdlib only)
  tools/make_assets.py   regenerates the placeholder art
  run_standalone.py      try it without Magi
  tests/                 python -m unittest discover tests
```

---

## Install

```powershell
pip install -r requirements.txt
```

### Into Magi (Flask)

Copy `magi_codec/` and `static/codec/` into the Magi project. Keep
`static/codec` next to the `magi_codec` package's parent folder, or pass
`static_dir=`. Then:

```python
from magi_codec import init_codec
init_codec(app)          # after app = Flask(...)
```

Add three lines to the dashboard page's template:

```html
<!-- in <head> -->
<link rel="manifest" href="/manifest.webmanifest">
<!-- where the panel should appear -->
<div id="codec-panel"></div>
<!-- before </body> -->
<script src="/static/codec/codec-panel.js" defer></script>
```

What this adds to Magi:

- **Routes:** `/sw.js`, `/manifest.webmanifest`, `/static/codec/*`, `/codec/<id>` and `/api/push/*`, `/api/codec/*`. Magi's own routes and its `/static/` folder are left alone (a test checks this).
- **Failures:** if Codec fails to start, it logs the error and Magi keeps running without it. Errors inside Codec routes are logged and return JSON 500s.
- **Styling:** the panel's CSS is scoped under `.codec-panel`. Change the `--codec-*` variables to match Magi's style.

**If Magi isn't Flask:** all the logic is in `CodecService` (`service.py`), which
has no web-framework code. Port `routes.py` (about 150 lines) to the other framework.
The front end doesn't care what serves it.

### Try it standalone

```powershell
python run_standalone.py      # http://127.0.0.1:8642  (stop Magi first; same port)
```

### Where secrets live

On first start, Codec creates its data folder **outside** anything served or
committed:

- **Windows:** `%LOCALAPPDATA%\Magi\codec\`
- **Override:** set `MAGI_CODEC_DIR`

| File | What |
|------|------|
| `config.json` | intake token, VAPID public key, disk settings, dashboard URL |
| `vapid_private.pem` | VAPID private key |
| `subscriptions.json` | phones, keyed by push endpoint |
| `calls.json` | call history (last 500) |
| `state.json` | rate-limit state |

`.gitignore` covers these names too, in case you ever point `MAGI_CODEC_DIR`
inside the repo. If you delete the PEM, new keys are generated and every phone
has to press **Enable** again.

`config.json` settings you might change:

```json
"disk": { "enabled": true, "path": "D:\\", "threshold_gb": 50, "rearm_margin_gb": 5, "interval_seconds": 300 },
"camera_motion_cooldown_seconds": 300,
"dashboard_url": "/",
"vapid_subject": "mailto:magi@localhost"
```

Restart Magi after editing `config.json`. Changes to `callers.json` apply right away.

---

## Phase 0: HTTPS over Tailscale (you do this once)

- [ ] Tailscale admin console > **DNS**: turn on **MagicDNS**.
- [ ] Same page: turn on **HTTPS Certificates**.
- [ ] On the PC (PowerShell): `tailscale serve --bg 8642`
- [ ] `tailscale serve status` shows `https://<pc-name>.<tailnet>.ts.net` proxying to `http://127.0.0.1:8642`.
- [ ] **Do not** run `tailscale funnel`. Funnel would put Magi on the public internet. `serve` is tailnet-only.
- [ ] Magi stays bound to `127.0.0.1`. Don't change it to `0.0.0.0`.

**Verify:** on the phone, with Tailscale connected, open
`https://<pc-name>.<tailnet>.ts.net`. The dashboard loads with a padlock and no
certificate warning. Turn Tailscale off on the phone and the page stops loading.

The intake route (`/api/codec/event`) accepts only requests made on the PC
itself. Requests that come through `tailscale serve` also arrive from
127.0.0.1, but they carry proxy headers (`X-Forwarded-For`, `Tailscale-User-*`).
Codec refuses any request that has those headers, so the phone can't post events
even if it knows the token.

## Phase 1: Install to the home screen

- [ ] Open the tailnet URL in **Chrome on Android**.
- [ ] Chrome menu (⋮) > **Add to Home screen** > **Install**.
- [ ] The Magi icon (green radio waves) appears. Opening it shows the dashboard with no browser bar.
- [ ] On the desktop, `http://127.0.0.1:8642` still works as before. The panel shows "Not installed", which is fine there.

## Phase 2: Web Push

- [ ] Open the installed app and press **Enable Codec alerts**. Allow notifications.
- [ ] The status badge shows **ENABLED**.
- [ ] Press **Send test call**. The result line says "Sent to 1 device(s)".
- [ ] Swipe the app away (close it completely), then press **Send test call** on the PC's dashboard. The phone shows **INCOMING CALL · STEREO-OPS · 140.85**.

Status badges:

| Badge | Meaning |
|-------|---------|
| ENABLED | This device gets calls. |
| OFF | Press Enable. |
| BLOCKED | Notifications were denied. Android: long-press the Magi icon > App info > Notifications > Allow, then reload. |
| UNAVAILABLE | The page isn't on HTTPS (use the ts.net URL), or the browser has no Web Push. |

Subscriptions that come back 404/410 from the push service (app uninstalled,
permission revoked) are removed automatically. Other push errors are logged,
and the event still goes into the call log.

## Phase 3: Sending events

```powershell
curl.exe -X POST http://127.0.0.1:8642/api/codec/event -H "Content-Type: application/json" -H "X-Magi-Token: <token from config.json>" -d "{\"type\":\"conversion_done\",\"title\":\"Conversion complete\",\"message\":\"Movie_SBS.mkv is ready\"}"
```

| Response | Meaning |
|----------|---------|
| 200 `{"call_id": ..., "push": {...}}` | Call created and pushed. |
| 200 `{"suppressed": true, "reason": ...}` | Dropped by a rate limit. |
| 400 | Bad payload. |
| 401 | Wrong token, or not from this PC. |

**Rate limits**

| Type | Rule |
|------|------|
| `conversion_done` | Always goes through. |
| `test` | Always goes through. The dashboard button is limited to one press every 3 s. |
| `camera_motion` | At most one call per 5 min. |
| `disk_low` | Once per threshold crossing. It re-arms after free space recovers to threshold + 5 GB. |

### StereoServe helper

`tools/codec_notify.py` uses only the standard library. It reads the token from
the config file, so there's nothing to set up on the same PC.

```powershell
python codec_notify.py "Conversion complete" --type conversion_done --file "D:\3D\Movie_SBS.mkv" --seconds 2843
# message: "File: Movie_SBS.mkv / Time: 47m 23s / Ready to watch."
```

In the StereoServe conversion script:

```python
import time
from codec_notify import notify_conversion   # copy codec_notify.py next to the script

t0 = time.monotonic()
run_conversion(src, out_path)                 # your existing code
notify_conversion(out_path, time.monotonic() - t0)
```

The helper never raises an exception and times out after 20 s, so if Magi is
down the conversion still finishes normally. It returns `False` and prints why.

### Disk check

Every 5 minutes, Codec checks free space on `disk.path` (default `D:\`). It
fires one **DISK-WARDEN** call when free space drops below `threshold_gb`
(default 50). Set `"enabled": false` to turn the check off.

### Camera motion

The route is ready: post `{"type": "camera_motion", ...}`. Wiring up the Wyze
feed comes later.

**Test checklist**

- [ ] The curl line above rings the phone.
- [ ] The same curl with a wrong token returns 401.
- [ ] Opening `https://<pc>.ts.net/api/codec/event` from the phone (any method) returns 401 or 405, never 200.
- [ ] Post `camera_motion` twice. The second response says `suppressed`.
- [ ] Run `codec_notify.py` with `--file` and `--seconds`. The call text shows the file name and time.

## Phase 4: The notification

The notification has:

- **Title:** INCOMING CALL
- **Body:** `<CALLER> · <frequency>`
- `tag` set to the call id, plus `requireInteraction` (it stays until you act on it)
- Vibration: long, short, long
- The Magi icon, plus a status-bar badge
- An **ANSWER** action button

**About the sound:** the Web Push API can't set a custom notification sound on
Android, so the phone plays its normal notification sound. To give it a
distinct sound, set one for Chrome's notifications in Android settings. The
custom ring plays on the call page.

Tapping the notification or ANSWER opens `/codec/<callId>` in the installed app.
If the app is already open, it switches to the call instead of opening a second
window.

**Test checklist**

- [ ] With the app closed, send a test call. The phone vibrates long-short-long, and the notification stays until you act on it.
- [ ] Tapping it opens the call page in the Magi app, not a Chrome tab.
- [ ] Send two calls. You get two separate notifications (different tags).

## Phase 5: The call page

1. **Ringing.** "INCOMING" pulses, the portrait shows static, and the signal bar flickers. A big **ANSWER** button shows. Browsers block sound until you tap, so the ring usually starts on the tap.
2. **ANSWER.** It plays the ring-to-connect sound and the portrait comes in. The text then types out with a blip per character. Tap to skip the typing.
3. **▼ TAP TO END.** It shows a static-out, a CRT collapse and "CALL ENDED", then returns to the dashboard.
4. **Replay.** Calls you've already answered, opened from the **Call log**, skip the ring and show **PLAY** instead. The tap is still needed so the blips can play.
5. **Missed or answered.** Pressing ANSWER marks the call answered. Anything you never answered shows as **Missed** in the call log.

The layout works in portrait and landscape.

**Test checklist**

- [ ] Tap the notification > ANSWER. You hear the connect sound, then blips while the text types.
- [ ] Tap during typing. The text completes at once.
- [ ] Tap at the end. You see the static-out and land back on the dashboard.
- [ ] Rotate the phone. The layout reflows.
- [ ] The call log shows that call as **Answered**. Tapping it replays without the ring.
- [ ] A call you ignored shows as **Missed**.

---

## Callers, portraits and sounds

**Callers:** edit `magi_codec/callers.json`. Each caller has `name`,
`frequency`, `portrait` and `role` (the small line under the signal bar).
`event_callers` maps event types to callers.

**Portraits:** `static/codec/portraits/stereo-ops.png`, `disk-warden.png` and
`sentry.png`. Recommended size is **360 × 480 px (3:4)**, PNG or JPG. Any image
works: the page tints it green and adds scanlines. You can replace a file under
the same name, or point `portrait` at a new file. Don't re-run
`tools/make_assets.py` after swapping portraits in, because it overwrites these three files.

**Icons:** `static/codec/icons/icon-192.png`, `icon-512.png`,
`icon-maskable-512.png` (keep the artwork inside the middle 60%) and
`badge-96.png` (white on transparent, used in the status bar).

**Sounds:** the defaults are synthesized in code, so there are no audio files to
license. To use your own, drop `ring`, `answer`, `blip` and/or `close` files into
`static/codec/sounds/` (details in that folder's README). They're git-ignored,
so they stay on your PC and are never pushed.

## Troubleshooting

- **Nothing arrives on the phone:** check the Magi log for `codec: push failed`.
  - Make sure the PC can reach `fcm.googleapis.com`.
  - Battery optimisation can delay pushes. Android Settings > Apps > Chrome > Battery > Unrestricted.
- **"Enable" fails with a key error:** the VAPID keys were regenerated. Press **Disable**, then **Enable**.
- **Duplicate disk alerts while developing:** Flask's debug reloader runs the app twice. Codec only starts the disk monitor in the process that actually serves pages.
- **Logs:** everything goes to the `magi.codec` logger.

## Tests

```powershell
python -m unittest discover tests
```

The tests cover:

- Auth, including requests proxied through Tailscale
- Bad payloads
- Push success, removal of dead subscriptions and failures that don't crash Magi
- Rate limits and the disk threshold re-arm logic
- History cap and route isolation from Magi's own `/static`
- The `codec_notify` message format and its behaviour when Magi is down
