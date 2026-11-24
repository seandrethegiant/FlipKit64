#pragma once
#include <atomic>
#include <cmath>
#include <cstdint>

// Song mode: a chain of rows, each one pattern played a number of times (MPC / Elektron style).
// The song is locked to the transport: beat 0 is the start of row 1, so any position in LUNA
// always lands on the same row and step, and scrubbing or looping a section works.
namespace Song
{
    constexpr int maxRows = 64, maxRepeats = 16;

    // Rows live in plain atomics so the editor can edit while the audio thread plays.
    struct Data
    {
        std::atomic<int> numRows { 0 };
        std::atomic<uint8_t> pattern[maxRows];       // 0..7
        std::atomic<uint8_t> repeats[maxRows];       // 1..maxRepeats
        Data() { clear(); }
        void clear()
        {
            numRows = 0;
            for (int i = 0; i < maxRows; ++i) { pattern[i].store (0); repeats[i].store (1); }
        }
        int rows() const { const int n = numRows.load(); return n < 0 ? 0 : (n > maxRows ? maxRows : n); }
        void insert (int at, int pat, int reps)          // message thread
        {
            const int n = rows();
            if (n >= maxRows) return;
            at = at < 0 ? 0 : (at > n ? n : at);
            for (int i = n; i > at; --i) { pattern[i].store (pattern[i - 1].load()); repeats[i].store (repeats[i - 1].load()); }
            pattern[at].store ((uint8_t) pat); repeats[at].store ((uint8_t) reps);
            numRows = n + 1;
        }
        void remove (int at)
        {
            const int n = rows();
            if (at < 0 || at >= n) return;
            for (int i = at; i < n - 1; ++i) { pattern[i].store (pattern[i + 1].load()); repeats[i].store (repeats[i + 1].load()); }
            numRows = n - 1;
        }
        void swap (int a, int b)
        {
            const int n = rows();
            if (a < 0 || b < 0 || a >= n || b >= n || a == b) return;
            const auto pa = pattern[a].load(), ra = repeats[a].load();
            pattern[a].store (pattern[b].load()); repeats[a].store (repeats[b].load());
            pattern[b].store (pa); repeats[b].store (ra);
        }
    };

    // A frozen copy for one audio block (or one editor paint), with every row's length in beats.
    struct Snapshot
    {
        int n = 0;
        int pattern[maxRows] {}, repeats[maxRows] {};
        double start[maxRows + 1] {};                // start[n] = the song's total length
        bool loop = true;
        double total() const { return start[n]; }
        double rowLength (int r) const { return start[r + 1] - start[r]; }

        // patternBeats[p] = one pass of pattern p in beats (its drum length x step size)
        void take (const Data& d, const double* patternBeats, int numPatterns, bool shouldLoop)
        {
            n = d.rows(); loop = shouldLoop;
            start[0] = 0.0;
            for (int i = 0; i < n; ++i)
            {
                int p = d.pattern[i].load(); p = p < 0 ? 0 : (p >= numPatterns ? numPatterns - 1 : p);
                int r = d.repeats[i].load(); r = r < 1 ? 1 : (r > maxRepeats ? maxRepeats : r);
                pattern[i] = p; repeats[i] = r;
                const double b = patternBeats[p] > 1.0e-6 ? patternBeats[p] : 4.0;
                start[i + 1] = start[i] + b * r;
            }
        }
    };

    // A row placed on the timeline (absolute beats), counting loops of the whole song.
    struct Placed
    {
        int row = -1, pattern = 0, repeats = 1;
        long long pass = 0;                          // which time through the song
        double start = 0.0, end = 0.0;
        bool valid() const { return row >= 0; }
    };

    inline Placed place (const Snapshot& s, long long pass, int row)
    {
        Placed p;
        p.row = row; p.pattern = s.pattern[row]; p.repeats = s.repeats[row]; p.pass = pass;
        p.start = pass * s.total() + s.start[row];
        p.end   = pass * s.total() + s.start[row + 1];
        return p;
    }

    // The row playing at ppq; invalid when the song is empty or (not looping) has ended.
    inline Placed locate (const Snapshot& s, double ppq)
    {
        if (s.n <= 0 || s.total() <= 0.0) return {};
        if (ppq < 0.0) ppq = 0.0;
        long long pass = 0;
        if (ppq >= s.total())
        {
            if (! s.loop) return {};
            pass = (long long) std::floor (ppq / s.total());
        }
        const double local = ppq - pass * s.total();
        int lo = 0, hi = s.n - 1;                    // last row whose start <= local
        while (lo < hi) { const int mid = (lo + hi + 1) / 2; if (s.start[mid] <= local) lo = mid; else hi = mid - 1; }
        return place (s, pass, lo);
    }

    // Calls f (Placed) for every row that overlaps [a, b), in time order.
    template <typename F>
    void forEachRow (const Snapshot& s, double a, double b, F&& f)
    {
        if (s.n <= 0 || s.total() <= 0.0 || b <= a) return;
        if (a < 0.0) a = 0.0;
        auto p = locate (s, a);
        int guard = 0;
        while (p.valid() && p.start < b && ++guard < 4 * maxRows)
        {
            f (p);
            int next = p.row + 1;
            long long pass = p.pass;
            if (next >= s.n) { if (! s.loop) break; next = 0; ++pass; }
            p = place (s, pass, next);
        }
    }

    // Where row r starts (for "play from here").
    inline double rowStart (const Snapshot& s, int r) { return (r >= 0 && r < s.n) ? s.start[r] : 0.0; }
}
