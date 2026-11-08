#!/usr/bin/env python3
"""
Synthesises the bundled ambience loops (assets/ambience/<scene>.ogg).

Every sound is made from scratch (filtered noise, sine partials, envelopes),
so the output contains no third-party audio and is released under CC0 1.0.

Seamless looping: all filtering is done in the frequency domain over the
whole loop (which is circular by construction), all modulations complete a
whole number of cycles per loop, and one-shot events (drips, thunder,
crackles...) wrap around the end of the buffer. The last sample therefore
flows straight into the first.

Usage:  python make_ambience.py OUT_DIR [--seconds 120] [--rate 32000] [--only rain,fire]
Needs:  numpy, and ffmpeg (with libvorbis) on PATH.
"""
from __future__ import annotations

import argparse
import shutil
import subprocess
import sys
import tempfile
import wave
from pathlib import Path

import numpy as np

SCENES = ["rain", "storm", "forest", "night", "ocean", "city", "tavern", "fire",
          "cave", "wind", "spaceship", "battle", "archive_quiet"]


class Gen:
    def __init__(self, seconds: float, rate: int, seed: int):
        self.sr = rate
        self.n = int(seconds * rate)
        self.rng = np.random.default_rng(seed)
        self.t = np.arange(self.n) / rate
        self.freqs = np.fft.rfftfreq(self.n, 1 / rate)

    # ---------------------------------------------------------- building blocks
    def noise(self, shape) -> np.ndarray:
        """Circular coloured noise. shape(freqs) -> amplitude per bin."""
        spec = (self.rng.standard_normal(self.freqs.size) + 1j * self.rng.standard_normal(self.freqs.size))
        spec *= shape(np.maximum(self.freqs, 1.0))
        spec[0] = 0
        x = np.fft.irfft(spec, self.n)
        return x / (np.std(x) + 1e-12)

    def band(self, lo: float, hi: float, slope: float = 0.0):
        """Smooth band-pass shape with an optional 1/f^slope tilt."""
        def shape(f):
            g = 1 / np.sqrt(1 + (lo / f) ** 4) / np.sqrt(1 + (f / hi) ** 4)
            return g / f ** (slope / 2)
        return shape

    def filt(self, x: np.ndarray, lo: float, hi: float) -> np.ndarray:
        s = np.fft.rfft(x)
        f = np.maximum(self.freqs, 1.0)
        s *= 1 / np.sqrt(1 + (lo / f) ** 4) / np.sqrt(1 + (f / hi) ** 4)
        return np.fft.irfft(s, self.n)

    def q(self, hz: float) -> float:
        """Rounds a frequency to a whole number of cycles per loop (seamless sines)."""
        seconds = self.n / self.sr
        return max(1, round(hz * seconds)) / seconds

    def lfo(self, cycles: int, phase: float | None = None) -> np.ndarray:
        """Sine with a whole number of cycles per loop, so it is seamless."""
        ph = self.rng.uniform(0, 2 * np.pi) if phase is None else phase
        return np.sin(2 * np.pi * cycles * np.arange(self.n) / self.n + ph)

    def wander(self, max_hz: float) -> np.ndarray:
        """Smooth random modulation in -1..1 (circular low-passed noise)."""
        def shape(f):
            return np.where(f < max_hz, 1.0, 0.0) * np.where(f > 0.0, 1.0, 0.0)
        x = self.noise(shape)
        return x / (np.max(np.abs(x)) + 1e-12)

    def place(self, buf: np.ndarray, ev: np.ndarray, pos: int, gain: float = 1.0):
        """Adds a one-shot event at pos, wrapping around the end of the loop."""
        pos %= self.n
        end = pos + ev.size
        if end <= self.n:
            buf[pos:end] += gain * ev
        else:
            k = self.n - pos
            buf[pos:] += gain * ev[:k]
            buf[: ev.size - k] += gain * ev[k:]

    def scatter(self, buf, make_event, per_second: float, gain=(0.5, 1.0)):
        count = self.rng.poisson(per_second * self.n / self.sr)
        for pos in self.rng.integers(0, self.n, count):
            self.place(buf, make_event(), int(pos), self.rng.uniform(*gain))

    def env(self, seconds: float, attack: float, decay: float) -> np.ndarray:
        t = np.arange(int(seconds * self.sr)) / self.sr
        return np.minimum(1, t / max(attack, 1e-4)) * np.exp(-t / max(decay, 1e-4))

    def burst(self, seconds, lo, hi, attack, decay):
        ev = self.rng.standard_normal(int(seconds * self.sr)) * self.env(seconds, attack, decay)
        s = np.fft.rfft(ev)
        f = np.maximum(np.fft.rfftfreq(ev.size, 1 / self.sr), 1.0)
        s *= 1 / np.sqrt(1 + (lo / f) ** 4) / np.sqrt(1 + (f / hi) ** 4)
        return np.fft.irfft(s, ev.size)

    def tone(self, seconds, f0, f1=None, decay=0.1, attack=0.002, partials=((1, 1.0),)):
        t = np.arange(int(seconds * self.sr)) / self.sr
        f1 = f0 if f1 is None else f1
        inst = f0 + (f1 - f0) * (t / seconds)
        phase = 2 * np.pi * np.cumsum(inst) / self.sr
        x = sum(a * np.sin(p * phase) for p, a in partials)
        return x * self.env(seconds, attack, decay)

    def reverb(self, x: np.ndarray, seconds: float, lo=80.0, hi=6000.0) -> np.ndarray:
        """Circular convolution with a decaying-noise impulse response."""
        m = int(seconds * self.sr)
        ir = self.rng.standard_normal(m) * np.exp(-np.arange(m) / (m / 6.0))
        full = np.zeros(self.n)
        full[:m] = ir
        y = np.fft.irfft(np.fft.rfft(x) * np.fft.rfft(full), self.n)
        y = self.filt(y, lo, hi)
        return y / (np.std(y) + 1e-12) * np.std(x)

    def stereo(self, make_mono) -> np.ndarray:
        return np.stack([make_mono(), make_mono()], axis=1)

    def pan(self, mono: np.ndarray, p: float) -> np.ndarray:
        """p in -1..1, equal power."""
        a = (p + 1) * np.pi / 4
        return np.stack([mono * np.cos(a), mono * np.sin(a)], axis=1)


# ------------------------------------------------------------------ the scenes
def rain(g: Gen, heavy=1.0):
    bed = g.stereo(lambda: g.noise(g.band(400, 9000, 0.6)))
    bed *= (1 + 0.15 * g.wander(0.05))[:, None]
    drops = np.zeros((g.n, 2))
    for ch in range(2):
        g.scatter(drops[:, ch], lambda: g.burst(0.02, 1500, 7000, 0.0005, 0.004), 40 * heavy, (0.5, 2.5))
    roof = g.stereo(lambda: g.noise(g.band(60, 500, 1.0))) * 0.5 * heavy
    return bed + 0.5 * drops + roof


def storm(g: Gen):
    x = rain(g, heavy=1.6) * 1.1
    wind = g.stereo(lambda: g.noise(g.band(80, 900, 1.0))) * (0.6 + 0.4 * g.wander(0.08))[:, None]
    thunder = np.zeros((g.n, 2))
    k = max(2, int(g.n / g.sr / 25))
    for i in range(k):
        pos = int((i + g.rng.uniform(0.1, 0.9)) * g.n / k)
        ev = g.burst(g.rng.uniform(5, 9), 25, g.rng.uniform(250, 600), g.rng.uniform(0.05, 0.6), g.rng.uniform(1.2, 2.5))
        ev *= 1 + 0.7 * np.abs(np.sin(np.linspace(0, g.rng.uniform(6, 14), ev.size)))  # rumbling
        p = g.rng.uniform(-0.6, 0.6)
        g.place(thunder[:, 0], ev, pos, np.cos((p + 1) * np.pi / 4) * 9)
        g.place(thunder[:, 1], ev, pos, np.sin((p + 1) * np.pi / 4) * 9)
    return x + 0.8 * wind + thunder


def birdsong(g: Gen, buf: np.ndarray, per_second: float):
    def chirp():
        f0 = g.rng.uniform(2200, 5200)
        notes = []
        for _ in range(g.rng.integers(2, 7)):
            d = g.rng.uniform(0.04, 0.14)
            notes.append(g.tone(d, f0, f0 * g.rng.uniform(0.8, 1.35), decay=d / 2, attack=0.01))
            notes.append(np.zeros(int(g.rng.uniform(0.02, 0.09) * g.sr)))
            f0 *= g.rng.uniform(0.9, 1.1)
        return np.concatenate(notes)
    for ch in range(2):
        g.scatter(buf[:, ch], chirp, per_second / 2, (0.05, 0.35))


def forest(g: Gen):
    leaves = g.stereo(lambda: g.noise(g.band(300, 4000, 0.8))) * (0.55 + 0.45 * g.wander(0.06))[:, None]
    birds = np.zeros((g.n, 2))
    birdsong(g, birds, 0.35)
    birds = birds + 0.3 * np.stack([g.reverb(birds[:, 0], 1.2, 1000, 8000), g.reverb(birds[:, 1], 1.2, 1000, 8000)], 1)
    return 0.6 * leaves + birds * 1.4


def night(g: Gen):
    air = g.stereo(lambda: g.noise(g.band(100, 1500, 1.2))) * 0.35
    crickets = np.zeros((g.n, 2))
    for c in range(5):
        f = g.q(g.rng.uniform(3800, 5200))
        rate = g.q(g.rng.uniform(14, 22))  # pulses per second
        gate = (np.sin(2 * np.pi * rate * g.t) > 0.3).astype(float)
        chirp_env = (0.5 + 0.5 * g.lfo(int(g.n / g.sr / g.rng.uniform(0.7, 1.4)))) ** 3
        presence = np.clip(0.6 + g.wander(0.03), 0, 1)
        sig = np.sin(2 * np.pi * f * g.t) * gate * chirp_env * presence
        sig = g.filt(sig, 2500, 7000)
        crickets += g.pan(sig, g.rng.uniform(-0.9, 0.9)) * g.rng.uniform(0.05, 0.12)
    owls = np.zeros(g.n)
    g.scatter(owls, lambda: np.concatenate([g.tone(0.35, 420, 380, 0.2, 0.05), np.zeros(int(0.25 * g.sr)), g.tone(0.6, 400, 360, 0.3, 0.05)]), 1 / 40, (0.04, 0.08))
    owls = g.reverb(owls, 1.5, 200, 3000)
    return air + crickets + g.pan(owls, 0.3)


def ocean(g: Gen):
    period = 9.0
    cycles = max(1, round(g.n / g.sr / period))
    phase = 2 * np.pi * cycles * np.arange(g.n) / g.n
    swell = (0.5 + 0.5 * np.sin(phase)) ** 2.2
    swell *= 0.6 + 0.4 * (0.5 + 0.5 * g.wander(0.02))
    wash = g.stereo(lambda: g.noise(g.band(150, 6000, 0.9)))
    hiss = g.stereo(lambda: g.noise(g.band(2000, 10000, 0.4))) * 0.4
    late = np.roll(swell, int(1.2 * g.sr))  # foam trails behind each crash
    low = g.stereo(lambda: g.noise(g.band(30, 200, 1.0))) * 0.6
    return wash * (0.15 + swell)[:, None] + hiss * (0.05 + late)[:, None] + low


def city(g: Gen):
    rumble = g.stereo(lambda: g.noise(g.band(30, 400, 1.6))) * (0.8 + 0.2 * g.wander(0.05))[:, None]
    hum = g.stereo(lambda: g.noise(g.band(400, 3000, 1.0))) * 0.25
    cars = np.zeros((g.n, 2))
    def car():
        d = g.rng.uniform(3, 7)
        e = np.sin(np.linspace(0, np.pi, int(d * g.sr))) ** 3
        return g.burst(d, 80, g.rng.uniform(1200, 2500), 0.01, 100) * e
    for _ in range(int(g.n / g.sr / 6)):
        ev = car()
        pos = int(g.rng.integers(0, g.n))
        sweep = np.linspace(-0.8, 0.8, ev.size) * g.rng.choice([-1, 1])
        a = (sweep + 1) * np.pi / 4
        g.place(cars[:, 0], ev * np.cos(a), pos, 0.9)
        g.place(cars[:, 1], ev * np.sin(a), pos, 0.9)
    horns = np.zeros(g.n)
    g.scatter(horns, lambda: g.tone(g.rng.uniform(0.2, 0.5), 400, None, 0.3, 0.02, ((1, 1), (2, 0.5), (3, 0.25))), 1 / 30, (0.03, 0.06))
    horns = g.reverb(g.filt(horns, 200, 1500), 1.0)
    return rumble + hum + cars + g.pan(horns, g.rng.uniform(-0.5, 0.5))


def babble(g: Gen, voices: int, level: float):
    """Unintelligible crowd: noise through moving formants, gated at syllable rate."""
    out = np.zeros((g.n, 2))
    for _ in range(voices):
        src = g.noise(g.band(120, 3500, 1.0))
        f1 = g.rng.uniform(350, 800)
        f2 = g.rng.uniform(1000, 2200)
        v = 0.6 * g.filt(src, f1 * 0.7, f1 * 1.4) + 0.4 * g.filt(src, f2 * 0.8, f2 * 1.25)
        syll = np.clip(g.wander(g.rng.uniform(3.5, 6.0)), 0, None) ** 1.5
        talk = np.clip(g.wander(0.08) + 0.3, 0, 1)
        out += g.pan(v * syll * talk, g.rng.uniform(-0.9, 0.9))
    out /= np.sqrt(voices)
    return out * level


def tavern(g: Gen):
    crowd = babble(g, 14, 1.0)
    crowd = np.stack([g.reverb(crowd[:, 0], 0.8, 150, 4000), g.reverb(crowd[:, 1], 0.8, 150, 4000)], 1) * 0.6 + crowd * 0.5
    clinks = np.zeros(g.n)
    g.scatter(clinks, lambda: g.tone(0.25, g.rng.uniform(1800, 3200), None, 0.05, 0.001, ((1, 1), (2.76, 0.4), (5.4, 0.2))), 0.25, (0.05, 0.2))
    thumps = np.zeros(g.n)
    g.scatter(thumps, lambda: g.burst(0.15, 60, 300, 0.002, 0.03), 0.15, (0.2, 0.5))
    hearth = fire(g) * 0.25
    return crowd + g.pan(clinks, 0.2) + g.pan(thumps, -0.3) + hearth


def fire(g: Gen):
    roar = g.stereo(lambda: g.noise(g.band(40, 600, 1.4))) * (0.6 + 0.3 * g.wander(0.3))[:, None]
    crackle = np.zeros((g.n, 2))
    for ch in range(2):
        g.scatter(crackle[:, ch], lambda: g.burst(0.012, 1200, 9000, 0.0002, 0.0025), 9, (0.5, 3.0))
        g.scatter(crackle[:, ch], lambda: g.burst(0.05, 300, 3000, 0.001, 0.012), 1.5, (0.5, 2.0))
    hiss = g.stereo(lambda: g.noise(g.band(3000, 9000, 0.3))) * 0.08 * (0.5 + 0.5 * g.wander(0.5))[:, None]
    return roar + crackle + hiss


def cave(g: Gen):
    room = g.stereo(lambda: g.noise(g.band(25, 250, 1.5))) * 0.5
    drips = np.zeros(g.n)
    def drip():
        f = g.rng.uniform(900, 2200)
        return g.tone(0.12, f * 1.4, f, 0.03, 0.0005)
    g.scatter(drips, drip, 0.6, (0.1, 0.45))
    wet = g.reverb(drips, 2.8, 300, 6000)
    l = 0.4 * drips + wet
    r = 0.4 * np.roll(drips, int(0.011 * g.sr)) + np.roll(g.reverb(drips, 2.8, 300, 6000), 37)
    return room + np.stack([l, r], 1) * 1.6


def wind(g: Gen):
    lowband = g.stereo(lambda: g.noise(g.band(80, 500, 1.2)))
    highband = g.stereo(lambda: g.noise(g.band(500, 2500, 0.8)))
    gust = np.clip(0.5 + 0.6 * g.wander(0.07), 0.05, 1.2)
    whistle = g.stereo(lambda: g.noise(g.band(900, 1100, 0.0))) * 0.3
    return lowband * (0.4 + 0.6 * gust)[:, None] + highband * (gust ** 2)[:, None] * 0.7 + whistle * (gust ** 3)[:, None]


def spaceship(g: Gen):
    hum = np.zeros(g.n)
    base = 55.0
    for h, a in [(1, 1.0), (2, 0.5), (3, 0.35), (4, 0.15), (6, 0.08)]:
        detune = g.rng.uniform(-0.04, 0.04)
        hum += a * np.sin(2 * np.pi * g.q(base * h + detune) * g.t + g.rng.uniform(0, 6.28))
    hum *= 0.9 + 0.1 * g.lfo(int(g.n / g.sr / 7))
    vent = g.stereo(lambda: g.noise(g.band(200, 3000, 1.0))) * 0.35
    rumble = g.stereo(lambda: g.noise(g.band(20, 120, 1.0))) * 0.6
    blips = np.zeros(g.n)
    g.scatter(blips, lambda: np.concatenate([g.tone(0.08, 1320, None, 0.05, 0.003), g.tone(0.1, 1760, None, 0.06, 0.003)]), 1 / 25, (0.02, 0.04))
    blips = g.reverb(blips, 0.6, 500, 6000)
    return np.stack([hum, hum], 1) * 0.8 + vent + rumble + g.pan(blips, 0.4)


def battle(g: Gen):
    rumble = g.stereo(lambda: g.noise(g.band(25, 300, 1.5))) * (0.7 + 0.3 * g.wander(0.15))[:, None]
    shouts = babble(g, 20, 1.0) * (0.6 + 0.4 * g.wander(0.1))[:, None]
    clash = np.zeros(g.n)
    def hit():
        f = g.rng.uniform(900, 2400)
        ring = g.tone(0.4, f, None, 0.08, 0.0005, ((1, 1), (2.41, 0.6), (3.9, 0.4), (5.3, 0.25)))
        ring[: int(0.05 * g.sr)] += g.burst(0.05, 2000, 9000, 0.0002, 0.01)
        return ring
    g.scatter(clash, hit, 1.2, (0.05, 0.3))
    booms = np.zeros(g.n)
    g.scatter(booms, lambda: g.burst(1.5, 25, 200, 0.01, 0.4), 1 / 8, (1.0, 2.5))
    far = lambda x: g.reverb(g.filt(x, 60, 5000), 1.6)
    return rumble + far(shouts[:, 0])[:, None] * [1, 0.8] + g.pan(far(clash), 0.1) * 1.5 + g.pan(far(booms), -0.2)


def archive_quiet(g: Gen):
    tone = g.stereo(lambda: g.noise(g.band(40, 1200, 1.6))) * 0.5
    hvac = np.sin(2 * np.pi * g.q(60) * g.t) * 0.03 + np.sin(2 * np.pi * g.q(120) * g.t) * 0.015
    pages = np.zeros(g.n)
    def page():
        d = g.rng.uniform(0.25, 0.6)
        e = np.sin(np.linspace(0, np.pi, int(d * g.sr))) ** 0.7
        return g.burst(d, 1500, 8000, 0.005, 10) * e
    g.scatter(pages, page, 1 / 18, (0.06, 0.15))
    pages = 0.5 * pages + g.reverb(pages, 1.2, 300, 7000)
    return tone + np.stack([hvac, hvac], 1) + g.pan(pages, g.rng.uniform(-0.6, 0.6))


BUILDERS = {name: globals()[name] for name in SCENES}


# --------------------------------------------------------------------- output
def master(x: np.ndarray, target_rms_db: float = -24.0) -> np.ndarray:
    x = x - x.mean(axis=0)
    rms = np.sqrt(np.mean(x ** 2)) + 1e-12
    x *= 10 ** (target_rms_db / 20) / rms
    # Gentle soft clip; keeps peaks under -1 dBFS without audible limiting at these levels.
    ceiling = 10 ** (-1 / 20)
    return np.tanh(x / ceiling) * ceiling


def write_ogg(x: np.ndarray, rate: int, out: Path, quality: float):
    pcm = (np.clip(x, -1, 1) * 32767).astype("<i2")
    with tempfile.TemporaryDirectory() as td:
        wav_path = Path(td) / "loop.wav"
        with wave.open(str(wav_path), "wb") as w:
            w.setnchannels(2)
            w.setsampwidth(2)
            w.setframerate(rate)
            w.writeframes(pcm.tobytes())
        subprocess.run(
            ["ffmpeg", "-hide_banner", "-loglevel", "error", "-y", "-i", str(wav_path),
             "-c:a", "libvorbis", "-q:a", str(quality),
             "-metadata", "title=" + out.stem, "-metadata", "copyright=CC0 1.0 (procedurally generated)",
             str(out)],
            check=True,
        )


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("out_dir", type=Path)
    ap.add_argument("--seconds", type=float, default=120.0, help="loop length (2-5 min recommended)")
    ap.add_argument("--rate", type=int, default=32000)
    ap.add_argument("--quality", type=float, default=0.0, help="libvorbis -q:a (-1..10)")
    ap.add_argument("--only", default="", help="comma-separated scene names")
    ap.add_argument("--seed", type=int, default=64)
    args = ap.parse_args()

    if not shutil.which("ffmpeg"):
        sys.exit("ffmpeg not found on PATH")
    names = [s.strip().lower() for s in args.only.split(",") if s.strip()] or SCENES
    args.out_dir.mkdir(parents=True, exist_ok=True)
    for i, name in enumerate(names):
        if name not in BUILDERS:
            sys.exit(f"unknown scene {name}")
        g = Gen(args.seconds, args.rate, args.seed + i * 1000)
        x = master(BUILDERS[name](g))
        out = args.out_dir / f"{name}.ogg"
        write_ogg(x, args.rate, out, args.quality)
        # Seam check: the step from the last sample to the first, relative to a typical step.
        seam = float(np.max(np.abs(x[0] - x[-1])))
        typical = float(np.percentile(np.abs(np.diff(x, axis=0)), 99))
        print(f"{name:14s} {out.stat().st_size / 1e6:5.2f} MB  seam/p99-step {seam / (typical + 1e-12):.2f}")


if __name__ == "__main__":
    main()
