#!/usr/bin/env python3
"""
Serves soundscapes/<itemId>.json to the phone.

The Magi dashboard listens on 127.0.0.1:8642 only. The phone reaches the PC
over Tailscale, so this module opens a *second* listener on the PC's
Tailscale address, same port (e.g. 100.101.102.103:8642), that can serve
nothing except:

    GET/HEAD /soundscapes/<itemId>.json   the timeline (ETag / 304 supported)
    GET/HEAD /soundscapes/ping            {"ok": true, "timelines": N}  (app's "Test connection")

Everything else is 404, other methods 405, there is no directory listing,
and item ids are matched against a strict pattern, so no path tricks reach
the disk. Binding two different addresses on the same port does not
conflict with Magi.

Three ways to use it:

1. From Magi's startup code (recommended):
       import soundscape_server
       soundscape_server.start_tailscale_listener(folder=r"C:\\Magi\\soundscapes")
   Optionally also add the route to Magi itself on 127.0.0.1 with
   flask_blueprint(...) or fastapi_router(...) below.

2. Standalone:
       python soundscape_server.py --bind tailscale --port 8642 --dir soundscapes

3. Any other framework: call handle(method, path, headers, folder) and send
   back the (status, headers, body) it returns.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import logging
import os
import re
import shutil
import subprocess
import sys
import threading
from email.utils import formatdate, parsedate_to_datetime
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import Mapping, Optional

log = logging.getLogger("soundscapes")

ROUTE = re.compile(r"^/soundscapes/([A-Za-z0-9_-]{1,128})\.json$")
PING = "/soundscapes/ping"
MAX_BYTES = 4 * 1024 * 1024


def handle(method: str, path: str, headers: Mapping[str, str], folder: Path) -> tuple[int, dict, bytes]:
    """Framework-free core. Returns (status, headers, body). Body is empty for HEAD."""
    path = path.split("?", 1)[0].split("#", 1)[0]
    base = {"Cache-Control": "no-cache", "X-Content-Type-Options": "nosniff"}
    if method not in ("GET", "HEAD"):
        return 405, {**base, "Allow": "GET, HEAD"}, b""

    if path == PING:
        count = sum(1 for _ in Path(folder).glob("*.json")) if Path(folder).is_dir() else 0
        body = json.dumps({"ok": True, "timelines": count}).encode()
        return 200, {**base, "Content-Type": "application/json"}, b"" if method == "HEAD" else body

    m = ROUTE.match(path)
    if not m:
        return 404, base, b""
    root = Path(folder).resolve()
    f = (root / f"{m.group(1)}.json").resolve()
    if f.parent != root or not f.is_file():
        return 404, base, b""
    st = f.stat()
    if st.st_size > MAX_BYTES:
        return 404, base, b""
    etag = '"' + hashlib.sha1(f"{st.st_mtime_ns}-{st.st_size}".encode()).hexdigest()[:20] + '"'
    hdrs = {**base, "Content-Type": "application/json; charset=utf-8", "ETag": etag,
            "Last-Modified": formatdate(st.st_mtime, usegmt=True)}

    inm = _get(headers, "If-None-Match")
    if inm is not None:
        if etag in [t.strip() for t in inm.split(",")] or inm.strip() == "*":
            return 304, hdrs, b""
    else:
        ims = _get(headers, "If-Modified-Since")
        if ims:
            try:
                if int(st.st_mtime) <= int(parsedate_to_datetime(ims).timestamp()):
                    return 304, hdrs, b""
            except (TypeError, ValueError):
                pass
    body = f.read_bytes()
    hdrs["Content-Length"] = str(len(body))
    return 200, hdrs, b"" if method == "HEAD" else body


def _get(headers: Mapping[str, str], name: str) -> Optional[str]:
    for k, v in headers.items():
        if k.lower() == name.lower():
            return v
    return None


# ------------------------------------------------------------------ stdlib server
def make_handler(folder: Path):
    class Handler(BaseHTTPRequestHandler):
        server_version = "soundscapes/1.0"
        sys_version = ""

        def _serve(self):
            status, hdrs, body = handle(self.command, self.path, dict(self.headers), folder)
            self.send_response(status)
            for k, v in hdrs.items():
                self.send_header(k, v)
            if "Content-Length" not in hdrs:
                self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            if body:
                self.wfile.write(body)

        do_GET = do_HEAD = _serve

        def _not_allowed(self):
            self.send_response(405)
            self.send_header("Allow", "GET, HEAD")
            self.send_header("Content-Length", "0")
            self.end_headers()

        do_POST = do_PUT = do_DELETE = do_PATCH = do_OPTIONS = _not_allowed

        def log_message(self, fmt, *args):
            log.info("%s %s", self.address_string(), fmt % args)

    return Handler


def tailscale_ipv4() -> Optional[str]:
    """The PC's Tailscale IPv4 (100.x.y.z), via the tailscale CLI."""
    candidates = [shutil.which("tailscale"),
                  r"C:\Program Files\Tailscale\tailscale.exe",
                  "/usr/bin/tailscale", "/Applications/Tailscale.app/Contents/MacOS/Tailscale"]
    for exe in filter(None, candidates):
        if not os.path.exists(exe):
            continue
        try:
            out = subprocess.run([exe, "ip", "-4"], capture_output=True, text=True, timeout=10).stdout
        except (OSError, subprocess.SubprocessError):
            continue
        for line in out.split():
            if re.fullmatch(r"100\.\d+\.\d+\.\d+", line.strip()):
                return line.strip()
    return None


def serve(host: str, port: int, folder: Path) -> ThreadingHTTPServer:
    httpd = ThreadingHTTPServer((host, port), make_handler(Path(folder)))
    httpd.daemon_threads = True
    return httpd


def start_tailscale_listener(folder, port: int = 8642, host: Optional[str] = None) -> Optional[ThreadingHTTPServer]:
    """
    Starts the soundscape-only listener on the Tailscale address in a daemon
    thread and returns the server (call .shutdown() to stop). Returns None and
    logs a warning if Tailscale is not up; never raises into the caller.
    """
    try:
        host = host or tailscale_ipv4()
        if not host:
            log.warning("soundscapes: no Tailscale IPv4 found; phone access disabled")
            return None
        httpd = serve(host, port, Path(folder))
        threading.Thread(target=httpd.serve_forever, name="soundscapes", daemon=True).start()
        log.info("soundscapes: serving %s on http://%s:%d/soundscapes/", folder, host, port)
        return httpd
    except Exception:
        log.exception("soundscapes: could not start the Tailscale listener")
        return None


# ------------------------------------------------------------------ framework hooks for Magi
def flask_blueprint(folder):
    """Flask: app.register_blueprint(soundscape_server.flask_blueprint(r"C:\\Magi\\soundscapes"))"""
    from flask import Blueprint, Response, request

    bp = Blueprint("soundscapes", __name__)

    @bp.route("/soundscapes/<path:rest>", methods=["GET", "HEAD"])
    def soundscape(rest):
        status, hdrs, body = handle(request.method, "/soundscapes/" + rest, dict(request.headers), Path(folder))
        return Response(body, status=status, headers=hdrs)

    return bp


def fastapi_router(folder):
    """FastAPI: app.include_router(soundscape_server.fastapi_router(r"C:\\Magi\\soundscapes"))"""
    from fastapi import APIRouter, Request, Response

    router = APIRouter()

    @router.api_route("/soundscapes/{rest:path}", methods=["GET", "HEAD"])
    def soundscape(rest: str, request: Request):
        status, hdrs, body = handle(request.method, "/soundscapes/" + rest, dict(request.headers), Path(folder))
        return Response(content=body, status_code=status, headers=hdrs)

    return router


def main(argv=None):
    ap = argparse.ArgumentParser(description="Serve soundscape timelines (read-only).")
    ap.add_argument("--bind", default="tailscale", help="'tailscale' (default), an IP, or 127.0.0.1")
    ap.add_argument("--port", type=int, default=8642)
    ap.add_argument("--dir", type=Path, default=Path(__file__).resolve().parent / "soundscapes")
    args = ap.parse_args(argv)
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(message)s")
    host = tailscale_ipv4() if args.bind == "tailscale" else args.bind
    if not host:
        sys.exit("Tailscale IPv4 not found. Is Tailscale running? Or pass --bind <ip>.")
    args.dir.mkdir(parents=True, exist_ok=True)
    httpd = serve(host, args.port, args.dir)
    print(f"serving {args.dir} at http://{host}:{args.port}/soundscapes/<itemId>.json  (Ctrl+C to stop)")
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
