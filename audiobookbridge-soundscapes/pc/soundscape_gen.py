#!/usr/bin/env python3
"""
soundscape_gen.py - builds AudiobookBridge scene timelines on the PC.

For one audiobook (a Jellyfin item, or a local folder of audio files):
  1. lists its tracks (chapters) and durations, so all times are book-global,
  2. transcribes it with faster-whisper on the GPU, in ~10 minute chunks with
     word timestamps (cached, so reruns are cheap),
  3. labels every ~60 s window with a scene using a local LLM via Ollama,
  4. merges windows and applies the app's 20 s / 45 s rules,
  5. writes soundscapes/<itemId>.json, which the soundscape server shares
     with the phone.

Examples:
  python soundscape_gen.py --item 5f2c...e1           # one Jellyfin book
  python soundscape_gen.py --item 5f2c...e1 --model medium --force
  python soundscape_gen.py --all-new                  # every book without a timeline
  python soundscape_gen.py --folder "D:\\Books\\Dune" --item 5f2c...e1

Environment:
  JELLYFIN_URL       default http://localhost:8096
  JELLYFIN_API_KEY   required for --item / --all-new (Dashboard > API Keys)
  JELLYFIN_USER_ID   optional; needed by some older Jellyfin versions
  OLLAMA_URL         default http://localhost:11434
"""
from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.parse
import urllib.request
from dataclasses import dataclass
from pathlib import Path
from typing import Callable, Iterable, Optional

# Keep in sync with the app's Scene enum and SceneRules.
SCENES = ["NONE", "RAIN", "STORM", "FOREST", "NIGHT", "OCEAN", "RIVER", "WINTER", "WIND",
          "TRAVEL", "CITY", "VEHICLE", "TAVERN", "FIRE", "HOME", "ARCHIVE_QUIET", "CAVE",
          "SPACESHIP", "BATTLE"]
MIN_SEGMENT_MS = 20_000
MIN_SWITCH_MS = 45_000
TIMELINE_VERSION = 1

WINDOW_S = 60          # scene window length
CHUNK_S = 600          # transcription chunk length
SAMPLE_RATE = 16_000   # whisper input rate
MIN_WORDS = 8          # windows with fewer words keep the previous scene
PROMPT_VERSION = 3     # bump to invalidate cached labels when the prompt changes
GENERATOR = "soundscape_gen/1.0"

HERE = Path(__file__).resolve().parent
AUDIO_EXTS = {".mp3", ".m4a", ".m4b", ".aac", ".flac", ".ogg", ".opus", ".wav", ".wma"}


def log(msg: str):
    print(msg, file=sys.stderr, flush=True)


# =============================================================== data
@dataclass
class Track:
    id: str
    name: str
    duration_ms: int
    path: Optional[Path] = None      # local file, if we have one
    remote: bool = False             # must be downloaded from Jellyfin


@dataclass
class Book:
    id: str
    name: str
    tracks: list[Track]

    @property
    def duration_ms(self) -> int:
        return sum(t.duration_ms for t in self.tracks)

    def offsets_ms(self) -> list[int]:
        out, acc = [], 0
        for t in self.tracks:
            out.append(acc)
            acc += t.duration_ms
        return out


# =============================================================== jellyfin
class Jellyfin:
    def __init__(self, url: str, api_key: str, user_id: Optional[str] = None):
        self.url = url.rstrip("/")
        self.key = api_key
        self.user_id = user_id

    def _req(self, path: str, params: Optional[dict] = None) -> urllib.request.Request:
        q = dict(params or {})
        if self.user_id and "userId" not in q:
            q["userId"] = self.user_id
        url = f"{self.url}{path}" + ("?" + urllib.parse.urlencode(q) if q else "")
        return urllib.request.Request(url, headers={
            "Authorization": f'MediaBrowser Client="soundscape_gen", Device="PC", DeviceId="soundscape_gen", Version="1.0", Token="{self.key}"',
            "X-Emby-Token": self.key,
            "Accept": "application/json",
        })

    def get(self, path: str, params: Optional[dict] = None):
        with urllib.request.urlopen(self._req(path, params), timeout=30) as r:
            return json.load(r)

    def item(self, item_id: str) -> dict:
        res = self.get("/Items", {"ids": item_id, "fields": "Path,MediaSources"})
        items = res.get("Items") or []
        if not items:
            raise SystemExit(f"Jellyfin item {item_id} not found")
        return items[0]

    def book(self, item_id: str, path_map: list[tuple[str, str]]) -> Book:
        it = self.item(item_id)
        if it.get("Type") == "Audio":
            children = [it]
        else:
            res = self.get("/Items", {
                "parentId": item_id, "recursive": "true", "includeItemTypes": "Audio",
                "fields": "Path,MediaSources", "sortBy": "ParentIndexNumber,IndexNumber,SortName",
                "sortOrder": "Ascending",
            })
            children = res.get("Items") or []
        if not children:
            raise SystemExit(f"{it.get('Name')} has no audio tracks")
        tracks = []
        for c in children:
            ticks = c.get("RunTimeTicks") or 0
            local = map_path(c.get("Path"), path_map)
            tracks.append(Track(
                id=c["Id"], name=c.get("Name", c["Id"]), duration_ms=int(ticks // 10_000),
                path=local if local and local.is_file() else None,
                remote=not (local and local.is_file()),
            ))
        return Book(id=item_id, name=it.get("Name", item_id), tracks=tracks)

    def all_books(self, library_id: Optional[str]) -> list[dict]:
        params = {"recursive": "true", "includeItemTypes": "MusicAlbum", "sortBy": "SortName"}
        if library_id:
            params["parentId"] = library_id
        return self.get("/Items", params).get("Items") or []

    def download(self, track: Track, dest_dir: Path) -> Path:
        dest_dir.mkdir(parents=True, exist_ok=True)
        existing = list(dest_dir.glob(f"{track.id}.*"))
        if existing:
            return existing[0]
        tmp = dest_dir / f"{track.id}.part"
        for path in (f"/Items/{track.id}/Download", f"/Audio/{track.id}/stream"):
            params = {"static": "true"} if "stream" in path else None
            try:
                with urllib.request.urlopen(self._req(path, params), timeout=60) as r, open(tmp, "wb") as f:
                    shutil.copyfileobj(r, f, 1 << 20)
                    ctype = r.headers.get("Content-Type", "")
                ext = guess_ext(ctype)
                final = dest_dir / f"{track.id}{ext}"
                tmp.replace(final)
                return final
            except urllib.error.HTTPError as e:
                log(f"  download via {path} failed: HTTP {e.code}")
        raise SystemExit(f"could not download track {track.name}")


def guess_ext(content_type: str) -> str:
    return {
        "audio/mpeg": ".mp3", "audio/mp4": ".m4a", "audio/x-m4a": ".m4a", "audio/aac": ".aac",
        "audio/flac": ".flac", "audio/ogg": ".ogg", "audio/opus": ".opus", "audio/wav": ".wav",
    }.get(content_type.split(";")[0].strip().lower(), ".audio")


def map_path(server_path: Optional[str], path_map: list[tuple[str, str]]) -> Optional[Path]:
    """Translates a path as Jellyfin sees it into a path on this PC."""
    if not server_path:
        return None
    for src, dst in path_map:
        if server_path.startswith(src):
            rest = server_path[len(src):].lstrip("/\\")
            return Path(dst) / Path(*re.split(r"[/\\]", rest)) if rest else Path(dst)
    return Path(server_path)


# =============================================================== local folder
def natural_key(p: Path):
    return [int(s) if s.isdigit() else s.lower() for s in re.split(r"(\d+)", p.name)]


def ffprobe_duration_ms(path: Path) -> int:
    out = subprocess.run(
        ["ffprobe", "-v", "error", "-show_entries", "format=duration", "-of", "default=nw=1:nk=1", str(path)],
        capture_output=True, text=True, check=True,
    ).stdout.strip()
    return int(float(out) * 1000)


def folder_book(folder: Path, item_id: Optional[str]) -> Book:
    files = sorted((p for p in folder.iterdir() if p.suffix.lower() in AUDIO_EXTS), key=natural_key)
    if not files:
        raise SystemExit(f"no audio files in {folder}")
    tracks = [Track(id=hashlib.sha1(str(p).encode()).hexdigest()[:16], name=p.name,
                    duration_ms=ffprobe_duration_ms(p), path=p) for p in files]
    book_id = item_id or re.sub(r"[^A-Za-z0-9_-]", "_", folder.name)
    if not item_id:
        log(f"note: no --item given; writing {book_id}.json. The app looks files up by Jellyfin item id.")
    return Book(id=book_id, name=folder.name, tracks=tracks)


# =============================================================== transcription
def decode_chunk(path: Path, start_s: float, dur_s: float):
    """Decodes one chunk to 16 kHz mono float32 with ffmpeg (keeps memory flat on 20 h books)."""
    import numpy as np
    cmd = ["ffmpeg", "-nostdin", "-hide_banner", "-loglevel", "error",
           "-ss", f"{start_s:.3f}", "-t", f"{dur_s:.3f}", "-i", str(path),
           "-ac", "1", "-ar", str(SAMPLE_RATE), "-f", "f32le", "-"]
    raw = subprocess.run(cmd, capture_output=True, check=True).stdout
    return np.frombuffer(raw, dtype=np.float32)


class Transcriber:
    def __init__(self, model_name: str, device: str, language: Optional[str]):
        self.model_name = model_name
        self.device = device
        self.language = language
        self._model = None

    def model(self):
        if self._model is None:
            from faster_whisper import WhisperModel
            if self.device in ("cuda", "auto"):
                try:
                    self._model = WhisperModel(self.model_name, device="cuda", compute_type="int8_float16")
                    log(f"whisper {self.model_name} on CUDA (int8_float16)")
                except Exception as e:  # missing cuDNN/cuBLAS, no GPU...
                    if self.device == "cuda":
                        raise
                    log(f"CUDA unavailable ({e}); falling back to CPU int8 (much slower)")
            if self._model is None:
                self._model = WhisperModel(self.model_name, device="cpu", compute_type="int8")
        return self._model

    def chunk(self, audio, offset_s: float) -> list[list]:
        segments, _info = self.model().transcribe(
            audio, language=self.language, word_timestamps=True, vad_filter=True,
            beam_size=1, condition_on_previous_text=False,
        )
        words = []
        for seg in segments:
            for w in seg.words or []:
                words.append([round(offset_s + w.start, 2), round(offset_s + w.end, 2), w.word.strip()])
        return words


def transcribe_book(book: Book, tr: Transcriber, cache: Path, jf: Optional[Jellyfin],
                    force: bool, bar_factory) -> list[list]:
    """Returns [[start_s, end_s, word], ...] in book-global seconds."""
    tdir = cache / "transcripts" / book.id
    tdir.mkdir(parents=True, exist_ok=True)
    total_s = book.duration_ms / 1000
    words: list[list] = []
    with bar_factory(total=round(total_s), unit="s", desc="transcribe") as bar:
        for track, off_ms in zip(book.tracks, book.offsets_ms()):
            dur_s = track.duration_ms / 1000
            n_chunks = max(1, math.ceil(dur_s / CHUNK_S))
            for c in range(n_chunks):
                start = c * CHUNK_S
                length = min(CHUNK_S, dur_s - start)
                cf = tdir / f"{track.id}.{tr.model_name}.{c:04d}.json"
                if cf.is_file() and not force:
                    chunk_words = json.loads(cf.read_text(encoding="utf-8"))
                else:
                    if track.path is None:
                        if jf is None:
                            raise SystemExit(f"no local file for {track.name}")
                        track.path = jf.download(track, cache / "audio")
                    audio = decode_chunk(track.path, start, length)
                    chunk_words = tr.chunk(audio, start) if audio.size else []
                    atomic_write(cf, json.dumps(chunk_words, ensure_ascii=False))
                # Chunk-local -> book-global.
                for s, e, w in chunk_words:
                    words.append([s + off_ms / 1000, e + off_ms / 1000, w])
                bar.update(round(length))
    return words


# =============================================================== windows + LLM
def build_windows(words: list[list], total_ms: int, window_s: int = WINDOW_S) -> list[dict]:
    n = max(1, math.ceil(total_ms / (window_s * 1000)))
    buckets: list[list[str]] = [[] for _ in range(n)]
    for s, _e, w in words:
        k = min(n - 1, max(0, int(s // window_s)))
        buckets[k].append(w)
    out = []
    for k in range(n):
        start = k * window_s * 1000
        end = min(total_ms, (k + 1) * window_s * 1000)
        if end > start:
            out.append({"startMs": start, "endMs": end, "text": " ".join(buckets[k]).strip()})
    return out


SCENE_HELP = {
    "NONE": "no clear physical setting: introductions, credits, author's notes, abstract narration",
    "RAIN": "outdoors or indoors with rain falling",
    "STORM": "thunderstorm, gale, violent weather",
    "FOREST": "woods, jungle, countryside in daytime, birds",
    "NIGHT": "outdoors at night, quiet countryside, camp under the stars",
    "OCEAN": "sea, beach, harbour, aboard a sailing ship",
    "RIVER": "beside or crossing a river, stream, brook, waterfall, ford",
    "WINTER": "snow, ice, blizzard, frozen land, cold winter outdoors",
    "WIND": "open exposed places: mountains, desert, plains, rooftops",
    "TRAVEL": "journeying on a road or track by horse, cart, wagon or carriage",
    "CITY": "streets, traffic, markets, crowds of a town or city",
    "VEHICLE": "inside a moving car, truck, bus, train or plane",
    "TAVERN": "inn, pub, bar, feast hall, busy indoor gathering",
    "FIRE": "by a hearth or campfire, or something burning",
    "HOME": "an ordinary home: kitchen, living room, bedroom, a family house or flat",
    "ARCHIVE_QUIET": "hushed public or work interiors: library, study, office, museum, archive",
    "CAVE": "caves, tunnels, dungeons, mines, underground",
    "SPACESHIP": "starship, space station, inside advanced machinery",
    "BATTLE": "fighting, war, a skirmish or siege in progress",
}

SYSTEM_PROMPT = (
    "You choose a background ambience for one passage of an audiobook. "
    "Pick the physical environment where the scene happens, not the mood.\n"
    "Allowed scenes:\n" + "\n".join(f"- {k}: {v}" for k, v in SCENE_HELP.items()) + "\n"
    "Rules:\n"
    "- Keep the previous scene unless the passage clearly moves to a different place, weather or situation.\n"
    "- Dialogue keeps the scene of where it is spoken.\n"
    "- intensity is how prominent the ambience should be, 0..1: 0.2 calm or barely present, "
    "0.5 normal, 0.9 loud and dramatic.\n"
    'Answer with JSON only: {"scene": "<one allowed scene>", "intensity": <number 0..1>}'
)


class LabelError(ValueError):
    pass


def validate_label(raw: str) -> tuple[str, float]:
    try:
        obj = json.loads(raw)
    except json.JSONDecodeError:
        m = re.search(r"\{.*\}", raw or "", re.S)  # tolerate prose around the object
        if not m:
            raise LabelError(f"not JSON: {raw[:80]!r}")
        try:
            obj = json.loads(m.group(0))
        except json.JSONDecodeError as e:
            raise LabelError(f"not JSON: {e}")
    if not isinstance(obj, dict):
        raise LabelError("not an object")
    scene = str(obj.get("scene", "")).strip().upper().replace(" ", "_").replace("-", "_")
    if scene not in SCENES:
        raise LabelError(f"unknown scene {scene!r}")
    try:
        intensity = float(obj.get("intensity", 0.5))
    except (TypeError, ValueError):
        raise LabelError("intensity is not a number")
    if math.isnan(intensity):
        raise LabelError("intensity is NaN")
    return scene, max(0.0, min(1.0, intensity))


class Ollama:
    def __init__(self, url: str, model: str):
        self.url = url.rstrip("/")
        self.model = model

    def chat(self, system: str, user: str) -> str:
        body = {
            "model": self.model,
            "stream": False,
            "keep_alive": "15m",
            "format": {
                "type": "object",
                "properties": {"scene": {"type": "string", "enum": SCENES},
                               "intensity": {"type": "number"}},
                "required": ["scene", "intensity"],
            },
            "options": {"temperature": 0.1, "num_ctx": 4096},
            "messages": [{"role": "system", "content": system}, {"role": "user", "content": user}],
        }
        req = urllib.request.Request(f"{self.url}/api/chat", data=json.dumps(body).encode(),
                                     headers={"Content-Type": "application/json"})
        with urllib.request.urlopen(req, timeout=180) as r:
            return json.load(r)["message"]["content"]

    def check(self):
        try:
            with urllib.request.urlopen(f"{self.url}/api/tags", timeout=5) as r:
                names = {m["name"] for m in json.load(r).get("models", [])}
        except Exception as e:
            raise SystemExit(f"Ollama not reachable at {self.url} ({e}). Start it with `ollama serve`.")
        if self.model not in names and f"{self.model}:latest" not in names:
            raise SystemExit(f"Ollama has no model {self.model!r}. Run: ollama pull {self.model}")


def user_prompt(prev_scene: str, prev_tail: str, text: str, minute: int) -> str:
    ctx = f"(end of the previous passage: ...{prev_tail})\n\n" if prev_tail else ""
    return f"Previous scene: {prev_scene}\n{ctx}Passage at minute {minute}:\n\"\"\"\n{text}\n\"\"\""


def label_windows(windows: list[dict], ask: Callable[[str, str], str], cache_file: Optional[Path],
                  model_id: str, force: bool, bar_factory) -> list[dict]:
    """Adds scene/intensity to each window. ask(system, user) -> raw model text."""
    cache: dict = {}
    if cache_file and cache_file.is_file() and not force:
        try:
            cache = json.loads(cache_file.read_text(encoding="utf-8"))
        except json.JSONDecodeError:
            cache = {}
    prev_scene, prev_int, prev_tail = "NONE", 0.5, ""
    failures = 0
    with bar_factory(total=len(windows), unit="win", desc="scenes") as bar:
        for k, w in enumerate(windows):
            text = w["text"]
            if len(text.split()) < MIN_WORDS:
                w["scene"], w["intensity"] = prev_scene, prev_int
            else:
                key = hashlib.sha1(f"{PROMPT_VERSION}|{model_id}|{prev_scene}|{prev_tail}|{text}".encode()).hexdigest()
                hit = cache.get(str(k))
                if hit and hit.get("key") == key:
                    scene, intensity = hit["scene"], hit["intensity"]
                else:
                    prompt = user_prompt(prev_scene, prev_tail, text, k)
                    try:
                        scene, intensity = validate_label(ask(SYSTEM_PROMPT, prompt))
                    except (LabelError, KeyError) as e:
                        try:  # retry once, with a reminder
                            scene, intensity = validate_label(ask(
                                SYSTEM_PROMPT,
                                prompt + f"\n\nYour last answer was invalid ({e}). Reply with JSON only, "
                                         f"scene one of: {', '.join(SCENES)}."))
                        except (LabelError, KeyError) as e2:
                            failures += 1
                            log(f"  window {k}: bad LLM output twice ({e2}); keeping {prev_scene}")
                            scene, intensity = prev_scene, prev_int
                    cache[str(k)] = {"key": key, "scene": scene, "intensity": intensity}
                    if cache_file and k % 10 == 0:
                        atomic_write(cache_file, json.dumps(cache))
                w["scene"], w["intensity"] = scene, intensity
            prev_scene, prev_int = w["scene"], w["intensity"]
            prev_tail = " ".join(text.split()[-40:])
            bar.update(1)
    if cache_file:
        atomic_write(cache_file, json.dumps(cache))
    if failures:
        log(f"  {failures} window(s) fell back to the previous scene")
    return windows


# =============================================================== merge rules
def merge_segments(windows: Iterable[dict], min_segment_ms: int = MIN_SEGMENT_MS,
                   min_switch_ms: int = MIN_SWITCH_MS) -> list[dict]:
    """
    Turns labelled windows into timeline segments:
      - neighbours with the same scene are merged (duration-weighted intensity),
      - segments shorter than min_segment_ms are dropped into a neighbour (the app ignores them),
      - segments shorter than min_switch_ms are absorbed too, so the timeline never asks the app
        to switch faster than its hysteresis allows.
    The result is contiguous and covers the book from the first window to the last.
    """
    segs = [{"startMs": int(w["startMs"]), "endMs": int(w["endMs"]), "scene": w["scene"],
             "intensity": float(w["intensity"])} for w in windows if w["endMs"] > w["startMs"]]

    def merge_same(lst):
        out = []
        for s in lst:
            if out and out[-1]["scene"] == s["scene"]:
                p = out[-1]
                dp, ds = p["endMs"] - p["startMs"], s["endMs"] - s["startMs"]
                p["intensity"] = (p["intensity"] * dp + s["intensity"] * ds) / max(1, dp + ds)
                p["endMs"] = s["endMs"]
            else:
                out.append(dict(s))
        return out

    segs = merge_same(segs)
    for limit in (min_segment_ms, min_switch_ms):
        while len(segs) > 1:
            idx = next((i for i, s in enumerate(segs) if s["endMs"] - s["startMs"] < limit), None)
            if idx is None:
                break
            short = segs.pop(idx)
            if idx > 0:
                segs[idx - 1]["endMs"] = short["endMs"]
            else:
                segs[0]["startMs"] = short["startMs"]
            segs = merge_same(segs)
    for s in segs:
        s["intensity"] = round(s["intensity"], 3)
    return segs


def make_timeline(book: Book, segments: list[dict], meta: dict) -> dict:
    return {
        "bookId": book.id,
        "version": TIMELINE_VERSION,
        "generator": GENERATOR,
        "generatedAt": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "title": book.name,
        "durationMs": book.duration_ms,
        "chapters": [{"id": t.id, "name": t.name, "durationMs": t.duration_ms} for t in book.tracks],
        **meta,
        "segments": segments,
    }


# =============================================================== utils
def atomic_write(path: Path, text: str):
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, tmp = tempfile.mkstemp(dir=path.parent, prefix=path.name, suffix=".tmp")
    with os.fdopen(fd, "w", encoding="utf-8") as f:
        f.write(text)
    os.replace(tmp, path)


def make_bar_factory(quiet: bool):
    try:
        from tqdm import tqdm
    except ImportError:
        tqdm = None

    class Plain:
        """Fallback progress line with ETA when tqdm is missing."""
        def __init__(self, total, unit, desc):
            self.total, self.unit, self.desc, self.n, self.t0, self.last = total, unit, desc, 0, time.time(), 0

        def __enter__(self):
            return self

        def __exit__(self, *a):
            if not quiet:
                print(file=sys.stderr)

        def update(self, k):
            self.n += k
            now = time.time()
            if quiet or (now - self.last < 1 and self.n < self.total):
                return
            self.last = now
            rate = self.n / max(1e-6, now - self.t0)
            eta = (self.total - self.n) / rate if rate > 0 else 0
            print(f"\r{self.desc}: {self.n}/{self.total} {self.unit}  ETA {int(eta // 60)}m{int(eta % 60):02d}s",
                  end="", file=sys.stderr, flush=True)

    def factory(total, unit, desc):
        if tqdm is not None:
            return tqdm(total=total, unit=unit, desc=desc, disable=quiet, dynamic_ncols=True)
        return Plain(total, unit, desc)
    return factory


def parse_path_map(items: list[str]) -> list[tuple[str, str]]:
    out = []
    for it in items:
        if "=" not in it:
            raise SystemExit(f"--path-map needs SERVER=LOCAL, got {it!r}")
        a, b = it.split("=", 1)
        out.append((a, b))
    return out


# =============================================================== main
def process(book: Book, args, jf: Optional[Jellyfin], llm: Ollama, bars) -> Path:
    out = args.out / f"{book.id}.json"
    log(f"\n== {book.name}  ({len(book.tracks)} tracks, {book.duration_ms / 3_600_000:.1f} h)")
    if book.duration_ms <= 0:
        raise SystemExit("book has no duration (Jellyfin has not scanned it yet?)")
    t0 = time.time()
    tr = Transcriber(args.model, args.device, args.language)
    words = transcribe_book(book, tr, args.cache, jf, args.retranscribe, bars)
    windows = build_windows(words, book.duration_ms)
    label_file = args.cache / "labels" / f"{book.id}.{re.sub(r'[^A-Za-z0-9_.-]', '_', llm.model)}.json"
    label_windows(windows, llm.chat, label_file, f"{llm.model}|{args.model}", args.force, bars)
    segments = merge_segments(windows)
    timeline = make_timeline(book, segments, {"model": {"whisper": args.model, "llm": llm.model}})
    atomic_write(out, json.dumps(timeline, indent=1, ensure_ascii=False))
    counts: dict[str, int] = {}
    for s in segments:
        counts[s["scene"]] = counts.get(s["scene"], 0) + (s["endMs"] - s["startMs"]) // 60000
    summary = ", ".join(f"{k} {v}m" for k, v in sorted(counts.items(), key=lambda kv: -kv[1]))
    log(f"   wrote {out}  ({len(segments)} segments: {summary})  in {(time.time() - t0) / 60:.1f} min")
    return out


def main(argv=None):
    ap = argparse.ArgumentParser(description="Build AudiobookBridge soundscape timelines.",
                                 formatter_class=argparse.RawDescriptionHelpFormatter, epilog=__doc__)
    src = ap.add_mutually_exclusive_group(required=True)
    src.add_argument("--item", help="Jellyfin item id of the book (album or single audio item)")
    src.add_argument("--all-new", action="store_true", help="every book that has no timeline yet")
    ap.add_argument("--folder", type=Path, help="local folder of audio files (with --item: id to write)")
    ap.add_argument("--model", default="small", help="faster-whisper model: small (default) or medium")
    ap.add_argument("--llm", default=os.environ.get("SOUNDSCAPE_LLM", "llama3.1:8b"), help="Ollama model")
    ap.add_argument("--ollama", default=os.environ.get("OLLAMA_URL", "http://localhost:11434"))
    ap.add_argument("--language", default=None, help="e.g. en; default: auto-detect")
    ap.add_argument("--device", default="auto", choices=["auto", "cuda", "cpu"])
    ap.add_argument("--force", action="store_true", help="rebuild even if the JSON exists; re-ask the LLM")
    ap.add_argument("--retranscribe", action="store_true", help="also ignore cached transcripts")
    ap.add_argument("--library", default=os.environ.get("JELLYFIN_LIBRARY_ID"), help="limit --all-new to a library id")
    ap.add_argument("--path-map", action="append", default=[], metavar="SERVER=LOCAL",
                    help="map Jellyfin paths to local ones, e.g. /data/books=D:\\Books (repeatable)")
    ap.add_argument("--out", type=Path, default=HERE / "soundscapes")
    ap.add_argument("--cache", type=Path, default=HERE / "cache")
    ap.add_argument("--quiet", action="store_true")
    ap.add_argument("--keep-going", action="store_true", help="with --all-new: continue after a failed book")
    args = ap.parse_args(argv)

    for tool in ("ffmpeg", "ffprobe"):
        if not shutil.which(tool):
            raise SystemExit(f"{tool} not found on PATH (install ffmpeg)")
    args.out.mkdir(parents=True, exist_ok=True)
    bars = make_bar_factory(args.quiet)
    llm = Ollama(args.ollama, args.llm)
    llm.check()

    jf = None
    if not args.folder or args.all_new:
        key = os.environ.get("JELLYFIN_API_KEY")
        if not key:
            raise SystemExit("set JELLYFIN_API_KEY (Jellyfin Dashboard > API Keys)")
        jf = Jellyfin(os.environ.get("JELLYFIN_URL", "http://localhost:8096"), key, os.environ.get("JELLYFIN_USER_ID"))
    path_map = parse_path_map(args.path_map)

    if args.folder:
        if args.all_new:
            raise SystemExit("--folder works with --item, not --all-new")
        book = folder_book(args.folder, args.item)
        if (args.out / f"{book.id}.json").exists() and not args.force:
            log(f"{book.id}.json exists; use --force to rebuild")
            return 0
        process(book, args, None, llm, bars)
        return 0

    if args.item:
        if (args.out / f"{args.item}.json").exists() and not args.force:
            log(f"{args.item}.json exists; use --force to rebuild")
            return 0
        process(jf.book(args.item, path_map), args, jf, llm, bars)
        return 0

    albums = jf.all_books(args.library)
    todo = [a for a in albums if args.force or not (args.out / f"{a['Id']}.json").exists()]
    log(f"{len(albums)} books, {len(todo)} without a timeline")
    failed = []
    t0 = time.time()
    for i, a in enumerate(todo, 1):
        log(f"[{i}/{len(todo)}]")
        try:
            process(jf.book(a["Id"], path_map), args, jf, llm, bars)
        except (Exception, SystemExit) as e:
            failed.append(a.get("Name", a["Id"]))
            log(f"   FAILED: {e}")
            if not args.keep_going:
                raise
        done = time.time() - t0
        log(f"   overall: {i}/{len(todo)} books, ETA {done / i * (len(todo) - i) / 60:.0f} min")
    if failed:
        log(f"failed: {', '.join(failed)}")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
