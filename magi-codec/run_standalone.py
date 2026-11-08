"""Try Magi Codec on its own, before wiring it into Magi.

    python run_standalone.py            # http://127.0.0.1:8642

Serves a stand-in dashboard page with the Codec panel. Stop real Magi first
(they share port 8642), or pass --port. Binds to 127.0.0.1 only.
"""
import argparse
import logging

from flask import Flask

from magi_codec import init_codec

DEMO_PAGE = """<!doctype html>
<html lang="en"><head>
<meta charset="utf-8"><meta name="viewport" content="width=device-width, initial-scale=1">
<title>Magi</title>
<link rel="manifest" href="/manifest.webmanifest">
<meta name="theme-color" content="#06140a">
<link rel="icon" href="/static/codec/icons/icon-192.png">
<link rel="apple-touch-icon" href="/static/codec/icons/icon-192.png">
<style>body{background:#000;color:#9dffb0;font-family:ui-monospace,Consolas,monospace;margin:0;padding:16px}</style>
</head><body>
<h1>Magi (standalone Codec demo)</h1>
<div id="codec-panel"></div>
<script src="/static/codec/codec-panel.js" defer></script>
</body></html>"""


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=8642)
    ap.add_argument("--data-dir", help="override the Codec data directory")
    a = ap.parse_args()
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(name)s: %(message)s")

    app = Flask(__name__, static_folder=None)
    app.get("/")(lambda: DEMO_PAGE)
    init_codec(app, data_dir=a.data_dir)
    app.run(host="127.0.0.1", port=a.port, threaded=True)


if __name__ == "__main__":
    main()
