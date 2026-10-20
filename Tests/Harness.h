#pragma once

#include <memory>
#include <vector>

#include "DSP/UmbralEngine.h"
#include "TestSignals.h"

namespace harness
{
using Buf = std::vector<float>;

struct Result
{
    Buf L, R;                                  // aligned with the input (latency removed), same length as the input
    std::vector<umbral::Diagnostics> trace;    // one entry per processed block
    int latency = 0;
};

inline int g_totalFaults = 0;   // numerical faults seen by every render() so far (asserted to be 0 at the end of the test run)

struct Options
{
    double fs = 48000.0;
    int block = 480;                            // also the diagnostics sampling period (10 ms at 48 kHz)
    uint32_t seed = 1;
    int traceEvery = 480;                       // samples between diagnostic snapshots
};

// Renders `inL` (and `inR` if non-null) through a fresh engine; flushes the latency with silence and aligns the output.
inline Result render (const Buf& inL, const Buf* inR, const umbral::Params& p, const Options& o = {})
{
    umbral::Engine e;
    e.setParams (p);
    e.setSeed (o.seed);
    e.prepare (o.fs, o.block);
    Result r;
    r.latency = e.getLatencySamples();
    const size_t n = inL.size(), total = n + (size_t) r.latency;
    Buf l (total, 0.0f), rr (total, 0.0f);
    std::copy (inL.begin(), inL.end(), l.begin());
    if (inR) std::copy (inR->begin(), inR->end(), rr.begin());
    for (size_t pos = 0; pos < total;)
    {
        const int len = (int) std::min<size_t> ((size_t) o.block, total - pos);
        float* ch[2] = { l.data() + pos, rr.data() + pos };
        e.process (ch, inR ? 2 : 1, len);
        pos += (size_t) len;
        r.trace.push_back (e.getDiagnostics());
    }
    g_totalFaults += e.getDiagnostics().numericalFaults;
    r.L.assign (l.begin() + r.latency, l.end());
    r.R.assign (rr.begin() + r.latency, rr.end());
    return r;
}

inline Result render (const Buf& in, const umbral::Params& p, const Options& o = {}) { return render (in, nullptr, p, o); }

inline umbral::Params make (float memory, float erosion, float drift, float threshold, float depth, float blend = 1.0f, float outDb = 0.0f)
{
    umbral::Params p;
    p.memory = memory; p.erosion = erosion; p.drift = drift; p.threshold = threshold; p.depth = depth; p.blend = blend; p.outputDb = outDb;
    return p;
}
} // namespace harness
