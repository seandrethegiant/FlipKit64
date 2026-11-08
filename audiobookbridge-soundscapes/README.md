# AudiobookBridge: Soundscapes

A quiet ambient sound bed (rain, tavern, forest, ship hum…) under the
narration, changing with the scene in the book. The phone only plays sounds;
all AI work (transcription and scene labels) runs on the PC.

```
audiobookbridge-soundscapes/
├─ android/soundscape/                 Android library module (Kotlin, Media3, coroutines, Compose)
│  ├─ src/main/java/com/audiobookbridge/soundscape/
│  │  ├─ model/   Scene, SceneRules, SoundscapeTimeline, TimelineJson        (pure Kotlin)
│  │  ├─ engine/  CrossfadeMachine, SceneDirector                             (pure Kotlin, unit-tested)
│  │  │           SoundscapeEngine (2 ExoPlayers), BookPosition, SoundscapePrefs,
│  │  │           SoundscapeController, StorylitHook
│  │  ├─ net/     TimelineRepository (HttpURLConnection, cache, ETag)
│  │  └─ ui/      SoundscapePlayerControls (chip + picker sheet), SoundscapeSettingsSection
│  ├─ src/main/assets/ambience/        13 seamless CC0 loops + README (sources/licences)
│  └─ src/test/                        JVM unit tests (28)
├─ pc/
│  ├─ soundscape_gen.py                Jellyfin → faster-whisper → Ollama → soundscapes/<id>.json
│  ├─ soundscape_server.py             read-only /soundscapes/<id>.json route, Tailscale listener, Magi hooks
│  ├─ requirements.txt
│  └─ tests/                           python -m unittest discover -s pc/tests
└─ tools/make_ambience.py              regenerates the ambience loops
```

> This lives in the FlipKit64 repo only because the AudiobookBridge repo was
> not available in the session it was built in. It is self-contained: move
> the folder into the AudiobookBridge repo as it is.

---

## Wiring it into the app

The package is `com.audiobookbridge.soundscape`. Rename it to the app's
package if you copy the sources instead of including the module.

**1. Add the module.** `settings.gradle.kts`: `include(":soundscape")` (with
`project(":soundscape").projectDir = file("…/android/soundscape")` if it sits
elsewhere). App: `implementation(project(":soundscape"))`. The root build
must declare `com.android.library`, `org.jetbrains.kotlin.android` and
`org.jetbrains.kotlin.plugin.compose` with `apply false`. Line the library
versions in `android/soundscape/build.gradle.kts` up with the app's.

**2. Create the engine next to the narration player**, in the
`MediaSessionService` (so ambience keeps playing with the screen off):

```kotlin
// in onCreate(), after building the narration ExoPlayer `player`
val engine = SoundscapeEngine(this, player, lifecycleScope)   // LifecycleService, or
                                                             // CoroutineScope(SupervisorJob() + Dispatchers.Main.immediate)
val soundscapePrefs = SoundscapePrefs(this) { jellyfinSession.userId }   // volume etc. saved per user
val controller = SoundscapeController(
    engine,
    soundscapePrefs,
    TimelineRepository(this) { soundscapePrefs.baseUrl },
    lifecycleScope,
)
Soundscapes.controller = controller   // however the app shares objects with the UI (DI, Application...)

// in onDestroy(), before player.release()
engine.release()
```

**3. Tell it when the book changes** (where the app starts a book):

```kotlin
controller.onBookChanged(
    bookId = album.id,                                  // the Jellyfin item id the PC file is named after
    chapterDurationsMs = tracks.map { it.runTimeTicks / 10_000 },  // null for a single-file book
)
```

Chapter durations matter for multi-track books: ExoPlayer's position is per
track, and the engine turns it into book time by adding the earlier tracks'
lengths. Use Jellyfin's `RunTimeTicks`, which is exactly what the PC
generator uses.

**4. UI.** On the player screen: `SoundscapePlayerControls(controller)`.
On the settings screen: `SoundscapeSettingsSection(controller)`. (Compose.
If the app uses Views, the controller's `uiState` StateFlow has everything a
View-based chip needs.)

**5. Cleartext HTTP.** The PC serves plain HTTP inside the tailnet. If the app
does not already allow cleartext, merge
`src/main/res/xml/soundscape_network_security_config.xml` into the app's
network security config. Use the PC's MagicDNS name
(`http://my-pc.tailnet-name.ts.net:8642`).

### How it behaves

- **Two extra ExoPlayers**, never the narration's queue. They loop
  (`REPEAT_MODE_ONE`), start at a random point, and are built with
  `handleAudioFocus = false`, so only the narration owns audio focus.
- **Follows the narration**: plays only while the narration has
  `playWhenReady` and is READY/BUFFERING with no suppression (a phone call or
  transient focus loss pauses it). Pause, stop, end of book: ambience pauses.
  A fade in progress freezes and continues on resume.
- **Sleep timer**: ambience volume is multiplied by the narration player's
  `volume`, so a timer that ramps `player.volume` fades both. A timer that
  lowers the system media volume fades both anyway (same stream). For
  anything else, call `engine.setSleepFade(fraction)`.
- **Volume**: master slider, default 15 % of the narration, saved per
  Jellyfin user. Buffering ducks by another 30 % (ramped over 300 ms).
  Intensity maps to 30-100 % of the slider level.
- **Crossfade**: 4 s, equal power. Switching back mid-fade reverses that
  fade in place; a third scene mid-fade reuses the quieter player.
- **Timeline**: position is polled every 500 ms in book time, so speed
  (0.5x-3x) has no effect on where switches land. Seeks, chapter skips and
  big jumps apply the right scene at once (even while paused). Otherwise no
  switch within 45 s of book time of the last one; segments under 20 s are
  ignored.
- **Loading**: cached copy first (instant, works offline), then a
  background fetch with `If-None-Match`; 3 s connect / 5 s read timeouts.
  404 removes the cache; any failure silently leaves manual mode. Nothing
  ever waits on the network before playing.
- **Per book**: picking a scene on the chip is remembered for that book and
  turns auto off for it; "Auto (follows the book)" turns it back on.
- **Failures**: every engine/controller entry point catches and logs (tag
  `Soundscape`); a broken asset is silenced and not retried every frame. The
  narration is never touched.

### Tests

The engine's logic is pure Kotlin, so it runs on the JVM:

```
./gradlew :soundscape:testDebugUnitTest
```

`CrossfadeMachineTest` (A→B, rapid switching, switch-back, pause mid-fade,
NONE fade-out, duck, volume chain), `SceneDirectorTest` (hysteresis, 20 s
rule, 0.5-3x speed, seeks, chapter jumps), `TimelineTest` (JSON incl. a file
written by `soundscape_gen.py`, normalisation, lookup).

---

## PC side (Windows, RTX 2070)

### Install

1. Python 3.10+, then in `pc/`: `pip install -r requirements.txt`
2. ffmpeg on PATH (`winget install Gyan.FFmpeg`).
3. [Ollama](https://ollama.com), then `ollama pull llama3.1:8b` (or any 7-8B
   instruct model; pass it with `--llm qwen2.5:7b`).
4. A Jellyfin API key (Dashboard → API Keys):
   ```powershell
   setx JELLYFIN_URL "http://localhost:8096"
   setx JELLYFIN_API_KEY "<key>"
   ```

**CUDA on Windows:** faster-whisper needs cuBLAS and cuDNN 9 for CUDA 12.
The pip packages in `requirements.txt` provide them; if loading still fails,
add their `bin` folders to PATH, e.g.
`…\site-packages\nvidia\cublas\bin` and `…\site-packages\nvidia\cudnn\bin`.
If CUDA can't load at all, the script says so and falls back to CPU (slow).
`--device cuda` makes it fail instead.

### Generate

```powershell
python soundscape_gen.py --item <jellyfinItemId>             # one book
python soundscape_gen.py --item <id> --model medium --force  # better transcript, rebuild
python soundscape_gen.py --all-new --keep-going              # every book without a JSON
python soundscape_gen.py --folder "D:\Books\Dune" --item <id>   # local files
```

- Books are Jellyfin `MusicAlbum`s (tracks = chapters, in disc/track order)
  or single `Audio` items. Times are book-global (track offsets from
  `RunTimeTicks`).
- If Jellyfin runs on the same PC, files are read from their library paths.
  Otherwise they are downloaded once into `cache/audio/`, or map paths with
  `--path-map "/data/books=D:\Books"`.
- Transcripts are cached per 10-minute chunk in `cache/transcripts/` (an
  interrupted run resumes; reruns skip Whisper), LLM labels in
  `cache/labels/`. `--force` rebuilds the JSON and re-asks the LLM but keeps
  transcripts; add `--retranscribe` to redo those too.
- Each 60 s window is labelled with strict JSON (Ollama structured output
  with an enum of scenes), given the previous scene and the end of the
  previous passage to avoid flip-flopping. Bad output is retried once, then
  the previous scene is kept. Windows with almost no words (music, silence)
  keep the previous scene without asking.
- Then: merge same-scene neighbours, absorb segments under 20 s and under
  45 s, write `soundscapes/<itemId>.json`.

Rough speed on an RTX 2070: Whisper `small` int8_float16 transcribes about
30-60x real time (a 10 h book in roughly 10-20 min); `medium` about half
that. Labelling is ~1-2 s per window with an 8B model (10 h = 600 windows ≈
10-20 min).

### Serve (Magi dashboard)

Magi stays on `127.0.0.1:8642`. For the phone, a second, separate listener
on the **Tailscale address, same port** serves only the soundscape route
(`GET/HEAD /soundscapes/<id>.json`, plus `/soundscapes/ping` for the app's
"Test connection"). Everything else is 404, there is no directory listing,
and ids are matched against `[A-Za-z0-9_-]`, so no path reaches outside the
folder.

In Magi's startup code:

```python
import soundscape_server
soundscape_server.start_tailscale_listener(folder=r"C:\path\to\pc\soundscapes", port=8642)

# Optional: the same route on Magi itself (127.0.0.1), for viewing locally
app.register_blueprint(soundscape_server.flask_blueprint(r"C:\path\to\pc\soundscapes"))   # Flask
# app.include_router(soundscape_server.fastapi_router(r"C:\path\to\pc\soundscapes"))      # FastAPI
```

Or run it on its own: `python soundscape_server.py --bind tailscale`.

Allow it through Windows Firewall on the Tailscale interface only:

```powershell
New-NetFirewallRule -DisplayName "Soundscapes (Tailscale)" -Direction Inbound -Protocol TCP `
  -LocalPort 8642 -InterfaceAlias Tailscale -Action Allow
```

In the app: Settings → Soundscapes → PC URL
`http://<pc-name>.<tailnet>.ts.net:8642` → **Save & test connection**.

---

## Test checklists (on the phone)

### Phase 1: manual soundscapes

- [ ] Play any book. Tap the **♪ Ambience** chip, pick **Rain**: rain fades in over ~4 s, quietly under the voice.
- [ ] Pick **Tavern** while rain plays: a smooth ~4 s crossfade, no gap, no loudness jump.
- [ ] Tap through 4-5 scenes quickly: it ends on the last one, no clicks or stacking.
- [ ] Pick **Off**: ambience fades out over ~4 s.
- [ ] Pause the book: ambience stops at once. Resume: it comes back. Pause in the middle of a crossfade, wait 10 s, resume: the fade finishes.
- [ ] Stop playback / let the book end: ambience stops.
- [ ] Set a 1-minute sleep timer: ambience fades and stops with the narration.
- [ ] Move the level slider: louder/quieter; restart the app: level is kept. Switch Jellyfin user: that user has their own level.
- [ ] Toggle ambience off and on in the sheet.
- [ ] Pick Rain on book A and Forest on book B; switch between them: each book remembers its scene.
- [ ] Unplug headphones: narration pauses, ambience pauses with it. Take a phone call: both pause, both resume after.
- [ ] Another app plays a notification sound: no fighting, narration keeps focus.
- [ ] Bad network on the Jellyfin side (narration buffering): ambience dips a little, then returns.
- [ ] Screen off for 5 minutes: ambience keeps looping, no gap at the loop point.

### Phase 2: timeline-driven scenes

- [ ] Settings → Soundscapes: enter the PC URL, **Save & test connection** → "Connected: N timelines".
- [ ] Wrong URL / PC off → a clear "Can't reach the PC" message within ~3 s; playback unaffected.
- [ ] Open a book that has a timeline: the chip shows **Auto · <scene>**, and the scene changes at the right places.
- [ ] Seek far ahead / back, and jump chapters: the right scene within a second, even while paused.
- [ ] Play at 0.5x, 1x, 2x, 3x: switches still land at the same story moments.
- [ ] Scenes never change faster than every ~45 s of book time during normal play.
- [ ] Pick a scene by hand: chip shows that scene; reopen the book: still manual. Pick **Auto**: follows the timeline again.
- [ ] Turn off Wi-Fi/Tailscale, reopen the book: cached timeline still works. Clear app data, PC off: falls back to manual with no error and no delay.
- [ ] Book with no timeline: manual mode, no errors.
- [ ] Auto scenes off in settings: always manual.

### Phase 3: PC generator

- [ ] `python -m unittest discover -s pc/tests` passes.
- [ ] `python soundscape_gen.py --item <short book id>` shows progress bars with ETA and writes `soundscapes/<id>.json`; the log says "whisper small on CUDA".
- [ ] Run it again with `--force`: transcription is skipped (cached), only labelling runs.
- [ ] `--all-new` processes only books without a JSON.
- [ ] From the phone's browser (on Tailscale): `http://<pc>.ts.net:8642/soundscapes/<id>.json` shows JSON; `http://<pc>.ts.net:8642/` is 404; Magi's dashboard is **not** reachable over Tailscale.
- [ ] Phone: open that book → Auto scenes follow the story.

### Phase 4: Storylit (later)

- [ ] Collect `engine.lightCues()` (or `engine.state`) in Storylit; bulbs change colour with the scene, ramping over `fadeMs` (4 s), and go back to normal when playback pauses (`active = false`).
