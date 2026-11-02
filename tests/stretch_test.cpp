// Standalone test for Source/TimeStretch.h (no JUCE).
//   g++ -std=c++17 -O2 -I ThirdParty -I Source tests/stretch_test.cpp -o stretch_test && ./stretch_test
#include "TimeStretch.h"
#include <cstdio>
#include <cstdlib>
#include <new>

static int fails = 0;
static void check (bool ok, const char* what) { std::printf ("%s %s\n", ok ? "PASS" : "FAIL", what); if (! ok) ++fails; }

// count heap allocations so we can prove the render path doesn't make any
static bool countAllocs = false;
static long allocs = 0;
void* operator new (std::size_t n) { if (countAllocs) ++allocs; void* p = std::malloc (n); if (! p) throw std::bad_alloc(); return p; }
void operator delete (void* p) noexcept { std::free (p); }
void operator delete (void* p, std::size_t) noexcept { std::free (p); }

using namespace TimeStretch;

struct Sample { std::vector<float> l, r; double rate; };

struct Result { std::vector<float> l; int frames = 0; };

// play a whole sample through a voice and collect the left channel
static Result play (Pool& pool, const Sample& s, double hostRate, double lenRatio, double semis, bool reverse, double glideSemis = 0.0)
{
    Cursor c;
    c.regionStart = 0; c.regionEnd = (double) s.l.size(); c.reverse = reverse; c.fadeLen = 64.0;
    c.rateRatio = s.rate / hostRate;
    c.pos = reverse ? c.regionEnd - 1.0 : 0.0;
    const double base = c.rateRatio * std::pow (2.0, semis / 12.0);
    begin (pool, c, 0, s.l.data(), s.r.data(), lenRatio, base);
    Result res;
    long guard = (long) (s.l.size() * lenRatio * 3) + 100000;
    while (guard-- > 0)
    {
        const double t = glideSemis != 0.0 ? base * std::pow (2.0, glideSemis * std::min (1.0, res.l.size() / (double) (s.l.size() * lenRatio)) / 12.0) : base;
        const int got = render (pool, c, s.l.data(), s.r.data(), chunk, lenRatio, t);
        res.l.insert (res.l.end(), pool.out.begin(), pool.out.begin() + got);
        if (got < chunk) break;
    }
    res.frames = (int) res.l.size();
    return res;
}

static double freqOf (const std::vector<float>& x, int a, int b, double sr)
{
    int zc = 0; int first = -1, last = -1;
    for (int i = a + 1; i < b; ++i) if (x[i - 1] < 0 && x[i] >= 0) { if (first < 0) first = i; last = i; ++zc; }
    return zc > 1 ? (zc - 1) * sr / (last - first) : 0.0;
}

// drum-ish hits: short decaying bursts at known positions
static Sample hits (double sr, const std::vector<double>& at, double seconds)
{
    Sample s; s.rate = sr; s.l.assign ((size_t) (seconds * sr), 0.0f);
    for (double t : at)
        for (int i = 0; i < (int) (0.06 * sr); ++i)
        {
            const size_t k = (size_t) (t * sr) + i;
            if (k < s.l.size()) s.l[k] += 0.9f * std::exp (-i / (0.008 * sr)) * std::sin (2 * M_PI * 180.0 * i / sr);
        }
    s.r = s.l; return s;
}
static std::vector<double> onsets (const std::vector<float>& x, double sr)
{
    std::vector<double> out; double lastT = -1;
    for (size_t i = 0; i < x.size(); ++i)
        if (std::abs (x[i]) > 0.25f && (lastT < 0 || i / sr - lastT > 0.15)) { out.push_back (i / sr); lastT = i / sr; }
    return out;
}

int main()
{
    const double sr = 44100.0;
    Pool pool; pool.prepare (sr);
    check (pool.ready() && (int) pool.slots.size() == numSlots, "pool prepared");

    // ratio maths
    check (std::abs (lengthRatio (1.0, 0.0, 120.0) - 1.0) < 1e-9, "no sync: manual only");
    check (std::abs (lengthRatio (1.0, 90.0, 120.0) - 0.75) < 1e-9, "sync: 90 bpm sample at 120 bpm plays 25% shorter");
    check (std::abs (lengthRatio (2.0, 120.0, 120.0) - 2.0) < 1e-9, "manual 2x");
    check (lengthRatio (10.0, 0.0, 120.0) == maxLength && lengthRatio (0.01, 0.0, 120.0) == minLength, "ratio clamped");
    check (std::abs (lengthRatio (1.0, 60.0, 240.0) - 0.25) < 1e-9 && lengthRatio (1.0, 30.0, 240.0) == minLength, "sync clamped");

    // pitch kept while stretching 2x
    Sample tone; tone.rate = sr; tone.l.resize (44100);
    for (size_t i = 0; i < tone.l.size(); ++i) tone.l[i] = 0.5f * std::sin (2 * M_PI * 440.0 * i / sr);
    tone.r = tone.l;
    {
        auto r = play (pool, tone, sr, 2.0, 0.0, false);
        check (std::abs (r.frames - 88200) < 2500, "2x stretch lasts ~2x");
        std::printf ("   frames %d (expect ~88200), freq %.1f\n", r.frames, freqOf (r.l, 20000, 70000, sr));
        check (std::abs (freqOf (r.l, 20000, 70000, sr) - 440.0) < 3.0, "2x stretch keeps 440 Hz");
    }
    {
        auto r = play (pool, tone, sr, 0.5, 0.0, false);
        check (std::abs (r.frames - 22050) < 2500, "0.5x stretch lasts ~half");
        check (std::abs (freqOf (r.l, 6000, 18000, sr) - 440.0) < 4.0, "0.5x stretch keeps 440 Hz");
    }
    // tune shifts pitch only
    {
        auto r = play (pool, tone, sr, 1.0, 12.0, false);
        std::printf("   +12st: frames %d freq %.1f\n", r.frames, freqOf (r.l, 10000, 35000, sr));
        check (std::abs (r.frames - 44100) < 2500, "+12 st keeps the length");
        check (std::abs (freqOf (r.l, 10000, 35000, sr) - 880.0) < 6.0, "+12 st doubles the frequency");
    }
    // 48k sample on a 44.1k host
    {
        Sample t48; t48.rate = 48000.0; t48.l.resize (48000);
        for (size_t i = 0; i < t48.l.size(); ++i) t48.l[i] = 0.5f * std::sin (2 * M_PI * 440.0 * i / 48000.0);
        t48.r = t48.l;
        auto r = play (pool, t48, sr, 1.0, 0.0, false);
        std::printf("   48k->44.1k: frames %d freq %.1f\n", r.frames, freqOf (r.l, 10000, 35000, sr));
        check (std::abs (r.frames - 44100) < 2500, "48k sample on 44.1k host lasts 1 s");
        check (std::abs (freqOf (r.l, 10000, 35000, sr) - 440.0) < 3.0, "48k sample keeps 440 Hz");
    }
    // transient timing: hits at 0, 0.5, 1.0 s should land at 0, 1.0, 2.0 s when doubled
    {
        auto s = hits (sr, { 0.0, 0.5, 1.0 }, 1.5);
        auto r = play (pool, s, sr, 2.0, 0.0, false);
        auto on = onsets (r.l, sr);
        std::printf ("   onsets:"); for (double t : on) std::printf (" %.3f", t); std::printf ("\n");
        check (on.size() == 3, "three hits come out");
        if (on.size() == 3)
            check (std::abs (on[0] - 0.0) < 0.03 && std::abs (on[1] - 1.0) < 0.04 && std::abs (on[2] - 2.0) < 0.04, "hits land at 2x the times");
        auto r2 = play (pool, s, sr, 0.5, 0.0, false);
        auto on2 = onsets (r2.l, sr);
        std::printf ("   onsets (0.5x):"); for (double t : on2) std::printf (" %.3f", t); std::printf ("\n");
        check (on2.size() == 3 && std::abs (on2[1] - 0.25) < 0.03 && std::abs (on2[2] - 0.5) < 0.03, "hits land at half the times");
    }
    // reverse: the last hit plays first
    {
        auto s = hits (sr, { 0.0, 0.5 }, 0.8);       // reversed: a hit near the end (0.5+) comes first
        auto r = play (pool, s, sr, 1.0, 0.0, true);
        double peakFirst = 0, peakLast = 0;
        for (int i = 0; i < 4000 && i < r.frames; ++i) peakFirst = std::max (peakFirst, (double) std::abs (r.l[i]));
        for (int i = std::max (0, r.frames - 6000); i < r.frames; ++i) peakLast = std::max (peakLast, (double) std::abs (r.l[i]));
        std::printf ("   reverse peaks start %.2f end %.2f frames %d\n", peakFirst, peakLast, r.frames);
        check (std::abs (r.frames - 35280) < 1500, "reverse length ok");
        check (peakLast > 0.1 && peakFirst < 0.1, "reverse plays the end first");
    }
    // pitch glide doesn't blow up
    {
        auto r = play (pool, tone, sr, 1.0, 0.0, false, 7.0);
        float mx = 0; for (float v : r.l) mx = std::max (mx, std::abs (v));
        check (mx < 1.5f && r.frames > 40000, "gliding pitch stays sane");
    }
    // render path allocates nothing
    {
        Cursor c; c.regionStart = 0; c.regionEnd = (double) tone.l.size(); c.rateRatio = 1.0; c.pos = 0;
        countAllocs = true; allocs = 0;
        begin (pool, c, 3, tone.l.data(), tone.r.data(), 1.7, 1.0);
        for (int i = 0; i < 40; ++i) render (pool, c, tone.l.data(), tone.r.data(), chunk, 1.7, std::pow (2.0, i / 120.0));
        countAllocs = false;
        std::printf ("   allocations during begin+render: %ld\n", allocs);
        check (allocs == 0, "no heap allocation while playing");
    }
    // end of region fades to silence (no click)
    {
        auto r = play (pool, tone, sr, 1.0, 0.0, false);
        float tail = 0; for (int i = std::max (0, r.frames - 50); i < r.frames; ++i) tail = std::max (tail, std::abs (r.l[i]));
        check (tail < 0.05f, "tail ends quiet");
    }
    std::printf (fails ? "\n%d FAILED\n" : "\nall passed\n", fails);
    return fails ? 1 : 0;
}
