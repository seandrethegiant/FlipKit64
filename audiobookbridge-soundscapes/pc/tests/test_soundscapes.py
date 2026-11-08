"""Run with:  python -m unittest discover -s pc/tests"""
import contextlib
import json
import sys
import tempfile
import threading
import time
import unittest
import urllib.error
import urllib.request
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

import soundscape_gen as gen  # noqa: E402
import soundscape_server as srv  # noqa: E402


@contextlib.contextmanager
def no_bar(total, unit, desc):
    class B:
        def update(self, k):
            pass
    yield B()


def win(start_s, end_s, scene, intensity=0.5):
    return {"startMs": start_s * 1000, "endMs": end_s * 1000, "scene": scene, "intensity": intensity}


class MergeTests(unittest.TestCase):
    def test_merges_same_scene_with_weighted_intensity(self):
        segs = gen.merge_segments([win(0, 60, "RAIN", 0.2), win(60, 180, "RAIN", 0.8), win(180, 240, "CAVE")])
        self.assertEqual([s["scene"] for s in segs], ["RAIN", "CAVE"])
        self.assertEqual(segs[0]["endMs"], 180_000)
        self.assertAlmostEqual(segs[0]["intensity"], 0.6, places=3)

    def test_short_tail_is_absorbed(self):
        segs = gen.merge_segments([win(0, 60, "RAIN"), win(60, 120, "CAVE"), win(120, 130, "FIRE")])
        self.assertEqual([s["scene"] for s in segs], ["RAIN", "CAVE"])
        self.assertEqual(segs[-1]["endMs"], 130_000)

    def test_segments_under_45s_are_absorbed_so_switches_respect_hysteresis(self):
        segs = gen.merge_segments([win(0, 60, "RAIN"), win(60, 90, "TAVERN"), win(90, 200, "RAIN")])
        self.assertEqual(len(segs), 1)
        self.assertEqual(segs[0]["scene"], "RAIN")
        for a, b in zip(segs, segs[1:]):
            self.assertGreaterEqual(b["startMs"] - a["startMs"], gen.MIN_SWITCH_MS)

    def test_short_first_segment_joins_the_next(self):
        segs = gen.merge_segments([win(0, 15, "NONE"), win(15, 120, "FOREST")])
        self.assertEqual(segs, [{"startMs": 0, "endMs": 120_000, "scene": "FOREST", "intensity": 0.5}])

    def test_contiguous_output(self):
        ws = [win(i * 60, (i + 1) * 60, s) for i, s in enumerate("RAIN RAIN CAVE CAVE FIRE NONE NONE RAIN".split())]
        segs = gen.merge_segments(ws)
        for a, b in zip(segs, segs[1:]):
            self.assertEqual(a["endMs"], b["startMs"])
        self.assertEqual(segs[0]["startMs"], 0)
        self.assertEqual(segs[-1]["endMs"], 480_000)


class WindowTests(unittest.TestCase):
    def test_words_go_to_their_minute(self):
        words = [[1.0, 1.2, "hello"], [59.9, 60.1, "edge"], [60.0, 60.3, "next"], [125.0, 125.5, "late"]]
        ws = gen.build_windows(words, total_ms=130_000)
        self.assertEqual([w["text"] for w in ws], ["hello edge", "next", "late"])
        self.assertEqual(ws[-1]["endMs"], 130_000)

    def test_offsets_are_book_global(self):
        b = gen.Book("b", "B", [gen.Track("t1", "1", 90_000), gen.Track("t2", "2", 30_000), gen.Track("t3", "3", 1)])
        self.assertEqual(b.offsets_ms(), [0, 90_000, 120_000])
        self.assertEqual(b.duration_ms, 120_001)


class LabelTests(unittest.TestCase):
    def test_validate(self):
        self.assertEqual(gen.validate_label('{"scene":"rain","intensity":0.4}'), ("RAIN", 0.4))
        self.assertEqual(gen.validate_label('Sure! {"scene": "archive quiet", "intensity": 3}'), ("ARCHIVE_QUIET", 1.0))
        for bad in ["", "nope", '{"scene":"LAVA","intensity":1}', '{"scene":"RAIN","intensity":"x"}', "[1]"]:
            with self.assertRaises(gen.LabelError):
                gen.validate_label(bad)

    def text(self, n=20, word="word"):
        return " ".join([word] * n)

    def test_retry_once_then_fall_back_to_previous(self):
        windows = [{"startMs": 0, "endMs": 60_000, "text": self.text()},
                   {"startMs": 60_000, "endMs": 120_000, "text": self.text(word="x")},
                   {"startMs": 120_000, "endMs": 180_000, "text": self.text(word="y")}]
        answers = iter(['{"scene":"TAVERN","intensity":0.7}',
                        "garbage", '{"scene":"CITY","intensity":0.5}',   # bad, then good on retry
                        "garbage", "still garbage"])                     # bad twice -> previous scene
        calls = []

        def ask(system, user):
            calls.append(user)
            return next(answers)

        out = gen.label_windows(windows, ask, None, "m", False, no_bar)
        self.assertEqual([w["scene"] for w in out], ["TAVERN", "CITY", "CITY"])
        self.assertEqual(len(calls), 5)
        self.assertIn("Previous scene: TAVERN", calls[1])
        self.assertIn("invalid", calls[2])

    def test_quiet_windows_keep_previous_scene_without_asking(self):
        windows = [{"startMs": 0, "endMs": 60_000, "text": self.text()},
                   {"startMs": 60_000, "endMs": 120_000, "text": "music"}]
        n = []
        out = gen.label_windows(windows, lambda s, u: n.append(1) or '{"scene":"OCEAN","intensity":0.3}',
                                None, "m", False, no_bar)
        self.assertEqual([w["scene"] for w in out], ["OCEAN", "OCEAN"])
        self.assertEqual(len(n), 1)

    def test_label_cache_makes_reruns_free(self):
        with tempfile.TemporaryDirectory() as td:
            cf = Path(td) / "labels.json"
            mk = lambda: [{"startMs": 0, "endMs": 60_000, "text": self.text()}]
            gen.label_windows(mk(), lambda s, u: '{"scene":"FIRE","intensity":0.6}', cf, "m", False, no_bar)

            def boom(s, u):
                raise AssertionError("should be cached")
            out = gen.label_windows(mk(), boom, cf, "m", False, no_bar)
            self.assertEqual(out[0]["scene"], "FIRE")


class ServerCoreTests(unittest.TestCase):
    def setUp(self):
        self.td = tempfile.TemporaryDirectory()
        self.dir = Path(self.td.name) / "soundscapes"
        self.dir.mkdir()
        (self.dir / "abc123.json").write_text('{"bookId":"abc123","version":1,"segments":[]}')
        (Path(self.td.name) / "secret.json").write_text("nope")

    def tearDown(self):
        self.td.cleanup()

    def test_serves_json_with_etag_and_304(self):
        st, h, body = srv.handle("GET", "/soundscapes/abc123.json", {}, self.dir)
        self.assertEqual(st, 200)
        self.assertEqual(json.loads(body)["bookId"], "abc123")
        st2, _, body2 = srv.handle("GET", "/soundscapes/abc123.json", {"if-none-match": h["ETag"]}, self.dir)
        self.assertEqual((st2, body2), (304, b""))

    def test_head_has_no_body(self):
        st, h, body = srv.handle("HEAD", "/soundscapes/abc123.json", {}, self.dir)
        self.assertEqual((st, body), (200, b""))
        self.assertTrue(int(h["Content-Length"]) > 0)

    def test_nothing_else_is_reachable(self):
        for p in ["/", "/soundscapes", "/soundscapes/", "/soundscapes/missing.json",
                  "/soundscapes/../secret.json", "/soundscapes/..%2Fsecret.json", "/soundscapes/abc123.txt",
                  "/soundscapes/sub/abc123.json", "/api/anything", "/soundscapes/abc123.json/"]:
            self.assertEqual(srv.handle("GET", p, {}, self.dir)[0], 404, p)
        self.assertEqual(srv.handle("POST", "/soundscapes/abc123.json", {}, self.dir)[0], 405)
        self.assertEqual(srv.handle("DELETE", "/soundscapes/abc123.json", {}, self.dir)[0], 405)

    def test_ping(self):
        st, _, body = srv.handle("GET", "/soundscapes/ping", {}, self.dir)
        self.assertEqual(st, 200)
        self.assertEqual(json.loads(body), {"ok": True, "timelines": 1})

    def test_real_http_listener(self):
        httpd = srv.serve("127.0.0.1", 0, self.dir)
        port = httpd.server_address[1]
        t = threading.Thread(target=httpd.serve_forever, daemon=True)
        t.start()
        try:
            base = f"http://127.0.0.1:{port}"
            with urllib.request.urlopen(base + "/soundscapes/abc123.json") as r:
                self.assertEqual(r.status, 200)
                etag = r.headers["ETag"]
            req = urllib.request.Request(base + "/soundscapes/abc123.json", headers={"If-None-Match": etag})
            with self.assertRaises(urllib.error.HTTPError) as cm:
                urllib.request.urlopen(req)
            self.assertEqual(cm.exception.code, 304)
            with self.assertRaises(urllib.error.HTTPError) as cm:
                urllib.request.urlopen(base + "/")
            self.assertEqual(cm.exception.code, 404)
            req = urllib.request.Request(base + "/soundscapes/abc123.json", data=b"x", method="POST")
            with self.assertRaises(urllib.error.HTTPError) as cm:
                urllib.request.urlopen(req)
            self.assertEqual(cm.exception.code, 405)
        finally:
            httpd.shutdown()
            httpd.server_close()


if __name__ == "__main__":
    unittest.main()
