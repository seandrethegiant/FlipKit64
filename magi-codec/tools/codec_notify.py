#!/usr/bin/env python3
"""Send a Codec call to Magi. Standard library only, so StereoServe needs no extra packages.

Command line:
    python codec_notify.py "Conversion complete" "Dune_Part2_SBS.mkv is ready." --type conversion_done
    python codec_notify.py "Conversion complete" --type conversion_done --file "D:/3D/Dune_Part2_SBS.mkv" --seconds 2843

From Python (StereoServe):
    from codec_notify import notify_conversion
    notify_conversion(output_path, elapsed_seconds)

The token is read from Magi's Codec config file (same machine), or from
--token / the MAGI_TOKEN environment variable. It never raises and never
blocks for more than a few seconds: a failed alert must not fail a conversion.
Exit code: 0 sent (or suppressed by a rate limit), 1 failed.
"""
from __future__ import annotations

import argparse
import json
import os
import sys
import urllib.error
import urllib.request
from pathlib import Path

DEFAULT_URL = os.environ.get("MAGI_URL", "http://127.0.0.1:8642")
TYPES = ("conversion_done", "disk_low", "camera_motion", "test")


def _config_path() -> Path:
    env = os.environ.get("MAGI_CODEC_DIR")
    if env:
        return Path(env) / "config.json"
    if os.name == "nt" and os.environ.get("LOCALAPPDATA"):
        return Path(os.environ["LOCALAPPDATA"]) / "Magi" / "codec" / "config.json"
    return Path.home() / ".magi" / "codec" / "config.json"


def _token(explicit: str | None) -> str | None:
    if explicit:
        return explicit
    if os.environ.get("MAGI_TOKEN"):
        return os.environ["MAGI_TOKEN"]
    try:
        with open(_config_path(), "r", encoding="utf-8") as f:
            return json.load(f).get("intake_token")
    except (OSError, ValueError):
        return None


def format_duration(seconds: float) -> str:
    s = int(round(seconds))
    h, rem = divmod(s, 3600)
    m, s = divmod(rem, 60)
    if h:
        return f"{h}h {m:02d}m {s:02d}s"
    if m:
        return f"{m}m {s:02d}s"
    return f"{s}s"


def build_message(message: str = "", file: str | None = None, seconds: float | None = None) -> str:
    lines = []
    if file:
        lines.append(f"File: {Path(file).name}")
    if seconds is not None:
        lines.append(f"Time: {format_duration(seconds)}")
    if message:
        lines.append(message)
    return "\n".join(lines)


def notify(title: str, message: str = "", type: str = "conversion_done", *, file: str | None = None,
           seconds: float | None = None, url: str = DEFAULT_URL, token: str | None = None,
           timeout: float = 20.0) -> bool:
    """Post an event to Magi. Returns True if Magi accepted it."""
    tok = _token(token)
    if not tok:
        print("codec_notify: no token (run Magi once, or pass --token / MAGI_TOKEN)", file=sys.stderr)
        return False
    body = json.dumps({"type": type, "title": title, "message": build_message(message, file, seconds)}).encode()
    req = urllib.request.Request(
        url.rstrip("/") + "/api/codec/event", data=body, method="POST",
        headers={"Content-Type": "application/json", "X-Magi-Token": tok},
    )
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            res = json.loads(r.read() or b"{}")
    except urllib.error.HTTPError as e:
        print(f"codec_notify: Magi refused the event ({e.code}): {e.read()[:200]!r}", file=sys.stderr)
        return False
    except Exception as e:  # Magi down, timeout, ...
        print(f"codec_notify: could not reach Magi at {url}: {e}", file=sys.stderr)
        return False
    if res.get("suppressed"):
        print(f"codec_notify: suppressed by rate limit: {res.get('reason')}")
    else:
        p = res.get("push") or {}
        print(f"codec_notify: call {res.get('call_id')} sent to {p.get('sent', 0)} device(s)")
    return True


def notify_conversion(output_file: str, seconds: float, extra: str = "", **kw) -> bool:
    """Convenience for StereoServe: one call when a job finishes."""
    return notify("Conversion complete", extra or "Ready to watch.", "conversion_done",
                  file=output_file, seconds=seconds, **kw)


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description="Send a Codec call to Magi.")
    ap.add_argument("title")
    ap.add_argument("message", nargs="?", default="")
    ap.add_argument("--type", default="conversion_done", choices=TYPES)
    ap.add_argument("--file", help="converted file; its name is added to the message")
    ap.add_argument("--seconds", type=float, help="total time taken; added to the message")
    ap.add_argument("--url", default=DEFAULT_URL)
    ap.add_argument("--token")
    a = ap.parse_args(argv)
    ok = notify(a.title, a.message, a.type, file=a.file, seconds=a.seconds, url=a.url, token=a.token)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
