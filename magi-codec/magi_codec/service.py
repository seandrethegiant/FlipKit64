"""Framework-independent core of Magi Codec.

The Flask blueprint in routes.py is a thin adapter over CodecService, so if
Magi ever moves to another web framework only that adapter has to change.
"""
from __future__ import annotations

import json
import logging
import secrets
import threading
import time
from datetime import datetime, timezone
from pathlib import Path

from . import config as cfgmod

log = logging.getLogger("magi.codec")

EVENT_TYPES = ("conversion_done", "disk_low", "camera_motion", "test")
MAX_TITLE = 120
MAX_MESSAGE = 1500

# Vibration: long - short - long (ms on / off / on ...)
VIBRATE = [700, 150, 200, 150, 700]

# How long a push service should keep trying to deliver, per event type.
TTL = {"conversion_done": 24 * 3600, "disk_low": 12 * 3600, "camera_motion": 600, "test": 3600}

SOUND_SLOTS = ("ring", "answer", "blip", "close")
SOUND_EXTS = (".mp3", ".ogg", ".wav", ".m4a", ".aac", ".webm", ".opus")

# If the built-in disk monitor is off, an externally posted disk_low re-arms after this.
DISK_LOW_FALLBACK_REARM = 12 * 3600


class EventError(ValueError):
    """Bad intake payload (-> HTTP 400)."""


class CodecService:
    def __init__(self, data_dir=None, static_dir=None, callers_file=None, sender=None):
        self.data_dir = Path(data_dir) if data_dir else cfgmod.default_data_dir()
        pkg = Path(__file__).resolve().parent
        self.static_dir = Path(static_dir) if static_dir else pkg.parent / "static" / "codec"
        self.callers_file = Path(callers_file) if callers_file else pkg / "callers.json"
        self.config = cfgmod.load_config(self.data_dir)
        self.subs_path = self.data_dir / "subscriptions.json"
        self.calls_path = self.data_dir / "calls.json"
        self.state_path = self.data_dir / "state.json"
        self._lock = threading.RLock()
        # sender(subscription_info, payload_str, ttl) -> None; raises on failure.
        # Injected in tests; the real one uses pywebpush.
        self._sender = sender or self._pywebpush_send

    # ------------------------------------------------------------------ config
    @property
    def public_key(self) -> str:
        return self.config["vapid_public_key"]

    @property
    def intake_token(self) -> str:
        return self.config["intake_token"]

    @property
    def dashboard_url(self) -> str:
        return self.config.get("dashboard_url") or "/"

    def callers(self) -> dict:
        """Re-read on each call so edits to callers.json apply without a restart."""
        data = cfgmod.read_json(self.callers_file, {})
        return {
            "callers": data.get("callers", {}),
            "event_callers": data.get("event_callers", {}),
        }

    def caller_for(self, event_type: str) -> tuple[str, dict]:
        c = self.callers()
        cid = c["event_callers"].get(event_type) or next(iter(c["callers"]), "unknown")
        info = c["callers"].get(cid) or {"name": "UNKNOWN", "frequency": "000.00", "portrait": ""}
        return cid, info

    def sounds(self) -> dict:
        """Which user-supplied sound files exist in static/codec/sounds/."""
        found = {}
        d = self.static_dir / "sounds"
        for slot in SOUND_SLOTS:
            for ext in SOUND_EXTS:
                if (d / (slot + ext)).is_file():
                    found[slot] = f"/static/codec/sounds/{slot}{ext}"
                    break
        return found

    # ------------------------------------------------------------ subscriptions
    def _subs(self) -> dict:
        return cfgmod.read_json(self.subs_path, {})

    def subscribe(self, body) -> int:
        sub = (body or {}).get("subscription", body) if isinstance(body, dict) else None
        if not isinstance(sub, dict):
            raise EventError("missing subscription")
        endpoint = sub.get("endpoint")
        keys = sub.get("keys") or {}
        if not isinstance(endpoint, str) or not endpoint.startswith("https://"):
            raise EventError("subscription endpoint must be an https URL")
        if not isinstance(keys.get("p256dh"), str) or not isinstance(keys.get("auth"), str):
            raise EventError("subscription keys missing")
        label = str((body or {}).get("label") or "")[:80]
        with self._lock:
            subs = self._subs()
            subs[endpoint] = {
                "subscription": {"endpoint": endpoint, "keys": {"p256dh": keys["p256dh"], "auth": keys["auth"]}},
                "label": label,
                "created": _now_iso(),
                "vapid_public_key": self.public_key,
            }
            cfgmod.write_json(self.subs_path, subs)
            return len(subs)

    def unsubscribe(self, endpoint) -> bool:
        if not isinstance(endpoint, str):
            raise EventError("missing endpoint")
        with self._lock:
            subs = self._subs()
            existed = subs.pop(endpoint, None) is not None
            if existed:
                cfgmod.write_json(self.subs_path, subs)
            return existed

    def subscription_count(self) -> int:
        return len(self._subs())

    def is_subscribed(self, endpoint) -> bool:
        return isinstance(endpoint, str) and endpoint in self._subs()

    # -------------------------------------------------------------------- push
    def _pywebpush_send(self, subscription_info, payload, ttl):
        from pywebpush import webpush  # imported lazily so Magi starts even if missing

        webpush(
            subscription_info=subscription_info,
            data=payload,
            vapid_private_key=str(self.data_dir / self.config["vapid_private_key_file"]),
            vapid_claims={"sub": self.config["vapid_subject"]},  # fresh dict: pywebpush mutates it
            ttl=ttl,
            headers={"Urgency": "high"},
            timeout=10,
        )

    def push_call(self, call: dict) -> dict:
        """Send a call to every subscription. Never raises."""
        payload = json.dumps({
            "callId": call["id"],
            "title": "INCOMING CALL",
            "body": f'{call["caller_name"]} · {call["frequency"]}',
            "url": f'/codec/{call["id"]}',
            "icon": "/static/codec/icons/icon-192.png",
            "badge": "/static/codec/icons/badge-96.png",
            "vibrate": VIBRATE,
            "ts": int(call["epoch"] * 1000),
        })
        result = {"sent": 0, "failed": 0, "removed": 0}
        try:
            subs = self._subs()
        except Exception:
            log.exception("codec: could not read subscriptions")
            return result
        dead = []
        for endpoint, rec in subs.items():
            try:
                self._sender(rec["subscription"], payload, TTL.get(call["type"], 3600))
                result["sent"] += 1
            except Exception as e:  # noqa: BLE001 - log every push failure, keep going
                status = getattr(getattr(e, "response", None), "status_code", None)
                if status in (404, 410):
                    dead.append(endpoint)
                    log.info("codec: subscription gone (%s), removing: %s", status, endpoint[:60])
                else:
                    result["failed"] += 1
                    log.warning("codec: push failed (%s) to %s: %s", status, endpoint[:60], e)
        if dead:
            with self._lock:
                subs = self._subs()
                for ep in dead:
                    subs.pop(ep, None)
                cfgmod.write_json(self.subs_path, subs)
            result["removed"] = len(dead)
        return result

    # ------------------------------------------------------------------- calls
    def _calls(self) -> list:
        data = cfgmod.read_json(self.calls_path, [])
        return data if isinstance(data, list) else []

    def list_calls(self, limit=50) -> list:
        calls = self._calls()
        return list(reversed(calls))[: max(1, min(int(limit), 500))]

    def get_call(self, call_id):
        for c in self._calls():
            if c.get("id") == call_id:
                return c
        return None

    def mark_answered(self, call_id) -> bool:
        with self._lock:
            calls = self._calls()
            for c in calls:
                if c.get("id") == call_id:
                    if not c.get("answered"):
                        c["answered"] = True
                        c["answered_at"] = _now_iso()
                        cfgmod.write_json(self.calls_path, calls)
                    return True
        return False

    def _save_call(self, call):
        with self._lock:
            calls = self._calls()
            calls.append(call)
            limit = int(self.config.get("history_limit") or 500)
            cfgmod.write_json(self.calls_path, calls[-limit:])

    def _update_call(self, call_id, **fields):
        with self._lock:
            calls = self._calls()
            for c in calls:
                if c.get("id") == call_id:
                    c.update(fields)
                    cfgmod.write_json(self.calls_path, calls)
                    return

    # ------------------------------------------------------------ rate limits
    def _state(self) -> dict:
        s = cfgmod.read_json(self.state_path, {})
        return s if isinstance(s, dict) else {}

    def _set_state(self, **kv):
        with self._lock:
            s = self._state()
            s.update(kv)
            cfgmod.write_json(self.state_path, s)

    def _rate_check(self, etype) -> str | None:
        """Return a reason string if the event should be suppressed."""
        now = time.time()
        s = self._state()
        if etype == "camera_motion":
            cooldown = float(self.config.get("camera_motion_cooldown_seconds") or 300)
            last = float(s.get("camera_motion_last") or 0)
            if now - last < cooldown:
                return f"camera_motion cooldown ({int(cooldown - (now - last))}s left)"
        elif etype == "disk_low":
            if s.get("disk_low_fired"):
                monitor_on = bool((self.config.get("disk") or {}).get("enabled"))
                fired_at = float(s.get("disk_low_fired_at") or 0)
                if monitor_on or now - fired_at < DISK_LOW_FALLBACK_REARM:
                    return "disk_low already reported; waiting for free space to recover"
        return None

    def _rate_record(self, etype):
        now = time.time()
        if etype == "camera_motion":
            self._set_state(camera_motion_last=now)
        elif etype == "disk_low":
            self._set_state(disk_low_fired=True, disk_low_fired_at=now)

    def rearm_disk_low(self):
        if self._state().get("disk_low_fired"):
            log.info("codec: free space recovered, disk_low re-armed")
            self._set_state(disk_low_fired=False)

    def disk_low_armed(self) -> bool:
        return not self._state().get("disk_low_fired")

    # ------------------------------------------------------------------ events
    @staticmethod
    def validate_event(body) -> dict:
        if not isinstance(body, dict):
            raise EventError("body must be a JSON object")
        etype = body.get("type")
        if etype not in EVENT_TYPES:
            raise EventError(f"type must be one of {', '.join(EVENT_TYPES)}")
        title, message = body.get("title", ""), body.get("message", "")
        if not isinstance(title, str) or not isinstance(message, str):
            raise EventError("title and message must be strings")
        title, message = title.strip(), message.strip()
        if not title and not message:
            raise EventError("title or message required")
        return {"type": etype, "title": title[:MAX_TITLE], "message": message[:MAX_MESSAGE]}

    def handle_event(self, body) -> dict:
        """Validate, rate-limit, record and push. Raises EventError on bad input."""
        ev = self.validate_event(body)
        with self._lock:
            reason = self._rate_check(ev["type"])
            if reason:
                log.info("codec: %s suppressed: %s", ev["type"], reason)
                return {"ok": True, "suppressed": True, "reason": reason}
            self._rate_record(ev["type"])
        call = self.create_call(ev)
        push = self.push_call(call)
        self._update_call(call["id"], push=push)
        log.info("codec: call %s (%s) pushed: %s", call["id"], ev["type"], push)
        return {"ok": True, "suppressed": False, "call_id": call["id"], "push": push,
                "subscriptions": self.subscription_count()}

    def create_call(self, ev) -> dict:
        cid, info = self.caller_for(ev["type"])
        now = time.time()
        call = {
            "id": secrets.token_hex(6),
            "type": ev["type"],
            "caller": cid,
            "caller_name": info.get("name", cid.upper()),
            "frequency": info.get("frequency", "000.00"),
            "title": ev["title"],
            "text": ev["message"],
            "timestamp": _now_iso(now),
            "epoch": now,
            "answered": False,
        }
        self._save_call(call)
        return call


def _now_iso(ts=None) -> str:
    return datetime.fromtimestamp(ts if ts is not None else time.time(), timezone.utc).isoformat(timespec="seconds")
