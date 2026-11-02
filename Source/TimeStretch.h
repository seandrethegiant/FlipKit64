#pragma once
// Time-stretch for pad voices: plays a sample longer or shorter without changing its pitch (and the
// other way round: Tune / Keys shift the pitch without changing the length).
//
// Built on Signalsmith Stretch (MIT, ThirdParty/). JUCE-free so it can be tested on its own.
// Everything here runs on the audio thread except StretchPool::prepare(), which allocates.
#include "signalsmith-stretch/signalsmith-stretch.h"
#include <algorithm>
#include <cmath>
#include <memory>
#include <vector>

namespace TimeStretch
{
    using Stretcher = signalsmith::stretch::SignalsmithStretch<float>;

    constexpr double minLength = 0.25, maxLength = 4.0;   // how much longer (>1) or shorter (<1) a pad plays
    constexpr double maxSpeed = 16.0;                      // most source frames read per output frame
    constexpr int chunk = 512;                             // output frames handled per call
    constexpr int numSlots = 16;                           // voices that can stretch at once

    // Length multiplier for a pad: the manual amount, times srcBpm / hostBpm when the pad follows the tempo.
    inline double lengthRatio (double manual, double srcBpm, double hostBpm)
    {
        double r = std::clamp (manual, minLength, maxLength);
        if (srcBpm >= 20.0 && hostBpm >= 20.0) r *= srcBpm / hostBpm;
        return std::clamp (r, minLength, maxLength);
    }

    // The engines and the scratch buffers every stretched voice shares (one audio thread, so one set).
    struct Pool
    {
        std::vector<std::unique_ptr<Stretcher>> slots;
        std::vector<float> in, out;       // planar stereo: [left | right]
        int inCap = 0;                    // frames per channel in `in`
        double sampleRate = 0.0;

        bool ready() const { return ! slots.empty(); }

        // Message thread, before audio runs: allocate and warm up so process() never allocates.
        void prepare (double sr)
        {
            if (ready() && sampleRate == sr) return;
            sampleRate = sr;
            slots.clear();
            for (int i = 0; i < numSlots; ++i)
            {
                auto st = std::make_unique<Stretcher> (1234 + i);
                st->presetCheaper (2, (float) sr);
                slots.push_back (std::move (st));
            }
            const int seekMax = slots[0]->outputSeekLength ((float) maxSpeed) + 16;
            inCap = std::max (seekMax, (int) (chunk * maxSpeed) + 16);
            in.assign ((size_t) inCap * 2, 0.0f);
            out.assign ((size_t) chunk * 2, 0.0f);
            for (auto& st : slots)             // grow every internal buffer to its working size
            {
                float* ip[2] = { in.data(), in.data() + inCap };
                float* op[2] = { out.data(), out.data() + chunk };
                st->setTransposeFactor (1.0f);
                st->outputSeek (ip, st->outputSeekLength ((float) maxSpeed));
                st->process (ip, (int) (chunk * maxSpeed), op, chunk);
                st->reset();
            }
        }
    };

    // One stretched voice's position in its sample.
    struct Cursor
    {
        int slot = -1;                    // engine in the pool, -1 = not stretching
        double pos = 0.0;                 // next source frame to read (whole numbers)
        double regionStart = 0.0, regionEnd = 0.0, fadeLen = 64.0;
        bool reverse = false;
        double rateRatio = 1.0;           // sample rate / host rate
        double inAcc = 0.0;               // fractional source frames carried between chunks
        bool ended = false;               // the whole region has been fed in
        int tailLeft = -1;                // output frames still to come once it has ended
        int preRoll = 0;                  // source frames fed ahead of the output at the start

        bool active() const { return slot >= 0; }
    };

    // Reads n source frames in play order (backwards when reversed) with the region edge faded out.
    inline void fill (Cursor& c, const float* sl, const float* sr, int n, float* dl, float* dr)
    {
        for (int k = 0; k < n; ++k)
        {
            const double remaining = c.reverse ? c.pos - c.regionStart : c.regionEnd - 1.0 - c.pos;
            if (remaining <= 0.0 || c.ended)
            {
                dl[k] = dr[k] = 0.0f;
                c.ended = true;
            }
            else
            {
                const int idx = (int) c.pos;
                const float f = remaining < c.fadeLen ? (float) (remaining / c.fadeLen) : 1.0f;
                dl[k] = sl[idx] * f;
                dr[k] = sr[idx] * f;
            }
            c.pos += c.reverse ? -1.0 : 1.0;
        }
    }

    // Start stretching: clears the engine and pre-rolls it so the first output lines up with the start of the sound.
    // `transpose` = pitch factor including the sample-rate ratio (what the tape-style voice calls `step`).
    inline void begin (Pool& pool, Cursor& c, int slot, const float* sl, const float* sr,
                       double lengthRatioNow, double transpose)
    {
        auto& st = *pool.slots[(size_t) slot];
        c.slot = slot;
        c.inAcc = 0.0;
        c.ended = false;
        c.tailLeft = -1;
        const double speed = std::min (maxSpeed, c.rateRatio / lengthRatioNow);
        st.reset();
        st.setTransposeFactor ((float) transpose);
        const int seekLen = std::min (pool.inCap, st.outputSeekLength ((float) speed));
        float* ip[2] = { pool.in.data(), pool.in.data() + pool.inCap };
        fill (c, sl, sr, seekLen, ip[0], ip[1]);
        st.outputSeek (ip, seekLen);
        c.preRoll = seekLen;
    }

    // Produce up to `n` (<= chunk) output frames into pool.out (left | right at offset `chunk`).
    // Returns how many are valid: fewer than n means the sound has finished.
    inline int render (Pool& pool, Cursor& c, const float* sl, const float* sr, int n,
                       double lengthRatioNow, double transpose)
    {
        auto& st = *pool.slots[(size_t) c.slot];
        const double speed = std::min (maxSpeed, c.rateRatio / lengthRatioNow);
        const double want = c.inAcc + n * speed;
        const int inN = std::min (pool.inCap, (int) want);
        c.inAcc = want - inN;
        float* ip[2] = { pool.in.data(), pool.in.data() + pool.inCap };
        float* op[2] = { pool.out.data(), pool.out.data() + chunk };
        fill (c, sl, sr, inN, ip[0], ip[1]);
        st.setTransposeFactor ((float) transpose);
        st.process (ip, inN, op, n);

        // the input runs `preRoll` frames ahead of what is being heard, so that many more output frames are still due
        if (c.ended && c.tailLeft < 0) c.tailLeft = (int) std::ceil (c.preRoll / speed) + st.intervalSamples();   // + one hop so the faded edge is fully out
        if (c.tailLeft >= 0)
        {
            const int valid = std::min (n, c.tailLeft);
            c.tailLeft -= valid;
            return valid;
        }
        return n;
    }
}
