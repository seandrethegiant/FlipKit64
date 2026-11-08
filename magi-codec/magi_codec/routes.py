"""Flask adapter: all Codec HTTP routes live on one blueprint."""
from __future__ import annotations

import hmac
import logging
import time

from flask import Blueprint, current_app, jsonify, request, send_from_directory

from .service import CodecService, EventError

log = logging.getLogger("magi.codec")

LOOPBACK = {"127.0.0.1", "::1", "::ffff:127.0.0.1"}

# `tailscale serve` proxies tailnet traffic to 127.0.0.1, so a request from the
# phone also arrives from loopback. These headers mark proxied requests; their
# presence means "not really local".
PROXY_HEADERS = (
    "X-Forwarded-For", "Forwarded", "X-Real-IP", "X-Forwarded-Host",
    "Tailscale-User-Login", "Tailscale-User-Name", "Tailscale-Funnel-Request",
)


def is_local_request() -> bool:
    if request.remote_addr not in LOOPBACK:
        return False
    return not any(h in request.headers for h in PROXY_HEADERS)


def same_origin_ok() -> bool:
    """Block cross-site form/fetch posts to the dashboard endpoints."""
    return request.headers.get("Sec-Fetch-Site", "same-origin") in ("same-origin", "none")


def create_blueprint(service: CodecService) -> Blueprint:
    bp = Blueprint("magi_codec", __name__)
    static_dir = str(service.static_dir)
    last_test = {"t": 0.0}

    def svc() -> CodecService:
        return service

    def bad(msg, code=400):
        return jsonify({"ok": False, "error": msg}), code

    @bp.errorhandler(Exception)
    def _err(e):  # log, never crash Magi
        from werkzeug.exceptions import HTTPException
        if isinstance(e, HTTPException):
            return e
        log.exception("codec: unhandled error on %s", request.path)
        return jsonify({"ok": False, "error": "internal error (see Magi log)"}), 500

    # ---- PWA root files (must be at the site root for scope "/") -----------
    @bp.get("/sw.js")
    def sw():
        resp = send_from_directory(static_dir, "sw.js", mimetype="application/javascript", max_age=0)
        resp.headers["Cache-Control"] = "no-cache"
        resp.headers["Service-Worker-Allowed"] = "/"
        return resp

    @bp.get("/manifest.webmanifest")
    def manifest():
        resp = send_from_directory(static_dir, "manifest.webmanifest", mimetype="application/manifest+json", max_age=0)
        resp.headers["Cache-Control"] = "no-cache"
        return resp

    @bp.get("/static/codec/<path:filename>")
    def codec_static(filename):
        return send_from_directory(static_dir, filename, max_age=300)

    # ---- Call page ----------------------------------------------------------
    @bp.get("/codec/<call_id>")
    def call_page(call_id):
        resp = send_from_directory(static_dir, "call.html", mimetype="text/html", max_age=0)
        resp.headers["Cache-Control"] = "no-cache"
        return resp

    # ---- Push ---------------------------------------------------------------
    @bp.get("/api/push/public-key")
    def public_key():
        return jsonify({"publicKey": svc().public_key})

    @bp.post("/api/push/subscribe")
    def subscribe():
        if not same_origin_ok():
            return bad("cross-site request refused", 403)
        try:
            n = svc().subscribe(request.get_json(silent=True))
        except EventError as e:
            log.warning("codec: bad subscribe payload: %s", e)
            return bad(str(e))
        return jsonify({"ok": True, "subscriptions": n})

    @bp.post("/api/push/unsubscribe")
    def unsubscribe():
        if not same_origin_ok():
            return bad("cross-site request refused", 403)
        body = request.get_json(silent=True) or {}
        try:
            existed = svc().unsubscribe(body.get("endpoint"))
        except EventError as e:
            return bad(str(e))
        return jsonify({"ok": True, "removed": existed})

    @bp.post("/api/push/status")
    def status():
        body = request.get_json(silent=True) or {}
        return jsonify({"ok": True, "subscribed": svc().is_subscribed(body.get("endpoint")),
                        "subscriptions": svc().subscription_count()})

    @bp.post("/api/push/test")
    def send_test():
        """The dashboard's "Send test call" button (no token: it is your own dashboard)."""
        if not same_origin_ok():
            return bad("cross-site request refused", 403)
        now = time.time()
        if now - last_test["t"] < 3:
            return bad("slow down: one test call every 3 seconds", 429)
        last_test["t"] = now
        result = svc().handle_event({
            "type": "test",
            "title": "Test call",
            "message": "Signal check. If you can read this, Codec alerts reach your phone. "
                       "Tap anywhere to end the call.",
        })
        return jsonify(result)

    # ---- Event intake (local scripts only) ------------------------------------
    @bp.post("/api/codec/event")
    def event():
        token = request.headers.get("X-Magi-Token", "")
        if not is_local_request() or not hmac.compare_digest(token.encode(), svc().intake_token.encode()):
            log.warning("codec: rejected intake from %s (local=%s)", request.remote_addr, is_local_request())
            return bad("unauthorized", 401)
        body = request.get_json(silent=True)
        try:
            return jsonify(svc().handle_event(body))
        except EventError as e:
            log.warning("codec: bad event payload: %s", e)
            return bad(str(e))

    # ---- History / call page data ---------------------------------------------
    @bp.get("/api/codec/calls")
    def calls():
        try:
            limit = int(request.args.get("limit", 50))
        except ValueError:
            limit = 50
        return jsonify({"calls": svc().list_calls(limit), "callers": svc().callers()["callers"]})

    @bp.get("/api/codec/calls/<call_id>")
    def call(call_id):
        c = svc().get_call(call_id)
        if not c:
            return bad("no such call", 404)
        callers = svc().callers()["callers"]
        return jsonify({
            "call": c,
            "caller": callers.get(c.get("caller"), {}),
            "sounds": svc().sounds(),
            "dashboard_url": svc().dashboard_url,
        })

    @bp.post("/api/codec/calls/<call_id>/answered")
    def answered(call_id):
        if not same_origin_ok():
            return bad("cross-site request refused", 403)
        if not svc().mark_answered(call_id):
            return bad("no such call", 404)
        return jsonify({"ok": True})

    @bp.get("/api/codec/callers")
    def callers():
        return jsonify(svc().callers())

    return bp
