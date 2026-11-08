"""Run:  python -m pytest tests   (or: python -m unittest discover tests)"""
import json
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))
sys.path.insert(0, str(ROOT / "tools"))

from flask import Flask  # noqa: E402

from magi_codec import init_codec  # noqa: E402
from magi_codec import disk_monitor  # noqa: E402

SUB = {"endpoint": "https://fcm.googleapis.com/fcm/send/abc", "keys": {"p256dh": "BPk", "auth": "xyz"}}


class FakeResp:
    def __init__(self, code):
        self.status_code = code


class PushError(Exception):
    def __init__(self, code):
        super().__init__(f"push {code}")
        self.response = FakeResp(code)


class CodecTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        # A Flask app with its own /static folder, like Magi probably has.
        static = Path(self.tmp.name) / "magi_static"
        static.mkdir()
        (static / "app.css").write_text("body{}")
        self.app = Flask(__name__, static_folder=str(static), static_url_path="/static")
        self.app.testing = True
        self.app.get("/")(lambda: "dashboard")
        self.svc = init_codec(self.app, data_dir=Path(self.tmp.name) / "data")
        self.sent = []
        self.fail_with = {}

        def sender(sub, payload, ttl):
            code = self.fail_with.get(sub["endpoint"])
            if code:
                raise PushError(code)
            self.sent.append((sub, json.loads(payload), ttl))

        self.svc._sender = sender
        self.c = self.app.test_client()
        self.token = self.svc.intake_token

    def tearDown(self):
        self.tmp.cleanup()

    def post_event(self, body, token=None, **env):
        return self.c.post("/api/codec/event", json=body,
                           headers={"X-Magi-Token": token if token is not None else self.token, **env.pop("headers", {})},
                           environ_base={"REMOTE_ADDR": env.pop("addr", "127.0.0.1")})

    # -- config / secrets
    def test_config_created_with_keys(self):
        data = Path(self.tmp.name) / "data"
        cfg = json.loads((data / "config.json").read_text())
        self.assertTrue(cfg["intake_token"])
        self.assertTrue((data / "vapid_private.pem").read_text().startswith("-----BEGIN PRIVATE KEY"))
        self.assertEqual(len(cfg["vapid_public_key"]), 87)  # 65 bytes base64url
        self.assertEqual(self.c.get("/api/push/public-key").json["publicKey"], cfg["vapid_public_key"])

    def test_existing_routes_untouched(self):
        self.assertEqual(self.c.get("/").data, b"dashboard")
        self.assertEqual(self.c.get("/static/app.css").status_code, 200)
        self.assertEqual(self.c.get("/static/codec/call.css").status_code, 200)
        r = self.c.get("/sw.js")
        self.assertIn("javascript", r.content_type)
        self.assertEqual(r.headers["Service-Worker-Allowed"], "/")
        self.assertEqual(self.c.get("/manifest.webmanifest").json["display"], "standalone")
        self.assertIn(b"answerBtn", self.c.get("/codec/anything").data)

    # -- subscribe
    def test_subscribe_unsubscribe(self):
        self.assertEqual(self.c.post("/api/push/subscribe", json={"subscription": SUB}).json["subscriptions"], 1)
        self.assertTrue(self.c.post("/api/push/status", json={"endpoint": SUB["endpoint"]}).json["subscribed"])
        self.assertEqual(self.c.post("/api/push/subscribe", json={"subscription": {"endpoint": "http://x"}}).status_code, 400)
        self.assertEqual(self.c.post("/api/push/subscribe", data="junk").status_code, 400)
        self.assertTrue(self.c.post("/api/push/unsubscribe", json={"endpoint": SUB["endpoint"]}).json["removed"])
        self.assertEqual(self.svc.subscription_count(), 0)

    def test_cross_site_refused(self):
        r = self.c.post("/api/push/subscribe", json={"subscription": SUB}, headers={"Sec-Fetch-Site": "cross-site"})
        self.assertEqual(r.status_code, 403)

    # -- intake auth
    def test_intake_requires_token_and_localhost(self):
        ev = {"type": "test", "title": "t", "message": "m"}
        self.assertEqual(self.post_event(ev, token="wrong").status_code, 401)
        self.assertEqual(self.post_event(ev, token="").status_code, 401)
        self.assertEqual(self.post_event(ev, addr="100.64.0.5").status_code, 401)
        # tailscale serve proxies from loopback but adds headers
        self.assertEqual(self.post_event(ev, headers={"X-Forwarded-For": "100.64.0.5"}).status_code, 401)
        self.assertEqual(self.post_event(ev, headers={"Tailscale-User-Login": "me@x"}).status_code, 401)
        self.assertEqual(self.post_event(ev).status_code, 200)

    def test_bad_payloads(self):
        for body in ([], {"type": "nope", "title": "x"}, {"type": "test"}, {"type": "test", "title": 5}):
            self.assertEqual(self.post_event(body).status_code, 400, body)
        r = self.c.post("/api/codec/event", data="{not json", headers={"X-Magi-Token": self.token,
                                                                     "Content-Type": "application/json"})
        self.assertEqual(r.status_code, 400)

    # -- calls + push
    def test_event_creates_call_and_pushes(self):
        self.c.post("/api/push/subscribe", json={"subscription": SUB})
        r = self.post_event({"type": "conversion_done", "title": "Done", "message": "File: a.mkv"}).json
        self.assertFalse(r["suppressed"])
        self.assertEqual(r["push"]["sent"], 1)
        _, payload, ttl = self.sent[0]
        self.assertEqual(payload["title"], "INCOMING CALL")
        self.assertEqual(payload["body"], "STEREO-OPS \u00b7 140.85")
        self.assertEqual(payload["url"], "/codec/" + r["call_id"])
        self.assertEqual(ttl, 24 * 3600)
        call = self.c.get("/api/codec/calls/" + r["call_id"]).json
        self.assertEqual(call["call"]["text"], "File: a.mkv")
        self.assertFalse(call["call"]["answered"])
        self.assertEqual(call["caller"]["name"], "STEREO-OPS")
        self.assertEqual(self.c.post(f"/api/codec/calls/{r['call_id']}/answered").status_code, 200)
        self.assertTrue(self.c.get("/api/codec/calls").json["calls"][0]["answered"])
        self.assertEqual(self.c.get("/api/codec/calls/nope").status_code, 404)

    def test_gone_subscriptions_removed_others_kept(self):
        gone = dict(SUB, endpoint="https://push.example/gone")
        flaky = dict(SUB, endpoint="https://push.example/flaky")
        for s in (SUB, gone, flaky):
            self.c.post("/api/push/subscribe", json={"subscription": s})
        self.fail_with = {gone["endpoint"]: 410, flaky["endpoint"]: 500}
        r = self.post_event({"type": "test", "title": "t", "message": "m"}).json
        self.assertEqual(r["push"], {"sent": 1, "failed": 1, "removed": 1})
        self.assertEqual(self.svc.subscription_count(), 2)

    def test_push_exception_never_crashes(self):
        self.c.post("/api/push/subscribe", json={"subscription": SUB})
        self.svc._sender = mock.Mock(side_effect=RuntimeError("boom"))
        r = self.post_event({"type": "test", "title": "t", "message": "m"})
        self.assertEqual(r.status_code, 200)
        self.assertEqual(r.json["push"]["failed"], 1)

    def test_test_button(self):
        self.c.post("/api/push/subscribe", json={"subscription": SUB})
        self.assertEqual(self.c.post("/api/push/test", json={}).json["push"]["sent"], 1)
        self.assertEqual(self.c.post("/api/push/test", json={}).status_code, 429)

    # -- rate limits
    def test_camera_motion_cooldown(self):
        ev = {"type": "camera_motion", "title": "Motion", "message": "Front door"}
        self.assertFalse(self.post_event(ev).json["suppressed"])
        self.assertTrue(self.post_event(ev).json["suppressed"])
        state = json.loads(self.svc.state_path.read_text())
        state["camera_motion_last"] -= 301
        self.svc.state_path.write_text(json.dumps(state))
        self.assertFalse(self.post_event(ev).json["suppressed"])

    def test_conversion_done_never_limited(self):
        for _ in range(5):
            self.assertFalse(self.post_event({"type": "conversion_done", "title": "x", "message": "y"}).json["suppressed"])

    def test_disk_monitor_threshold_crossing(self):
        self.svc.config["disk"].update(path="/", threshold_gb=50, rearm_margin_gb=5)
        GB = disk_monitor.GB

        def usage(free_gb):
            return mock.patch.object(disk_monitor.shutil, "disk_usage", return_value=mock.Mock(free=free_gb * GB))

        with usage(40):
            self.assertEqual(disk_monitor.check_once(self.svc), "fired")
            self.assertEqual(disk_monitor.check_once(self.svc), "low")
        with usage(52):  # above threshold but inside the margin: still not re-armed
            self.assertEqual(disk_monitor.check_once(self.svc), "ok")
        with usage(40):
            self.assertEqual(disk_monitor.check_once(self.svc), "low")
        with usage(60):
            self.assertEqual(disk_monitor.check_once(self.svc), "rearmed")
        with usage(30):
            self.assertEqual(disk_monitor.check_once(self.svc), "fired")
        calls = self.svc.list_calls()
        self.assertEqual([c["type"] for c in calls], ["disk_low", "disk_low"])
        self.assertEqual(calls[0]["caller_name"], "DISK-WARDEN")

    def test_history_limit(self):
        self.svc.config["history_limit"] = 3
        for i in range(5):
            self.post_event({"type": "conversion_done", "title": str(i), "message": "m"})
        self.assertEqual([c["title"] for c in self.svc.list_calls()], ["4", "3", "2"])


class NotifyHelperTest(unittest.TestCase):
    def test_message_format(self):
        import codec_notify
        self.assertEqual(codec_notify.format_duration(2843), "47m 23s")
        self.assertEqual(codec_notify.format_duration(3725), "1h 02m 05s")
        self.assertEqual(codec_notify.build_message("Ready.", "D:/3D/Movie_SBS.mkv", 65),
                         "File: Movie_SBS.mkv\nTime: 1m 05s\nReady.")

    def test_no_token_returns_false(self):
        import codec_notify
        with mock.patch.object(codec_notify, "_token", return_value=None):
            self.assertFalse(codec_notify.notify("t", "m"))

    def test_magi_down_returns_false(self):
        import codec_notify
        self.assertFalse(codec_notify.notify("t", "m", url="http://127.0.0.1:9", token="x", timeout=1))


if __name__ == "__main__":
    unittest.main()
