// UMBRAL engine tests. Console program, no framework: every CHECK prints PASS/FAIL, the exit code is the failure count.
// The checks are objective measurements (spectra, partial frequencies, levels, state traces). They show that each
// control does something measurable and distinct; they cannot tell whether it sounds good - that needs ears.
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <random>

#include "Analysis.h"
#include "Harness.h"
#include "../Source/Presets.h"

// ---- allocation counter (T17: nothing may allocate on the audio thread) ---------------------------------------------
static std::atomic<long> g_allocs { 0 };
static std::atomic<bool> g_track { false };
void* operator new (std::size_t n) { if (g_track.load (std::memory_order_relaxed)) ++g_allocs; if (void* p = std::malloc (n ? n : 1)) return p; throw std::bad_alloc(); }
void* operator new[] (std::size_t n) { if (g_track.load (std::memory_order_relaxed)) ++g_allocs; if (void* p = std::malloc (n ? n : 1)) return p; throw std::bad_alloc(); }
void operator delete (void* p) noexcept { std::free (p); }
void operator delete[] (void* p) noexcept { std::free (p); }
void operator delete (void* p, std::size_t) noexcept { std::free (p); }
void operator delete[] (void* p, std::size_t) noexcept { std::free (p); }

using namespace harness;
using umbral::Params;

static int g_fail = 0, g_pass = 0;
static void check (bool ok, const char* fmt, ...)
{
    char buf[512];
    va_list a; va_start (a, fmt); std::vsnprintf (buf, sizeof buf, fmt, a); va_end (a);
    std::printf ("  [%s] %s\n", ok ? "PASS" : "FAIL", buf);
    ok ? ++g_pass : ++g_fail;
}
static void section (const char* s) { static auto t0 = std::chrono::steady_clock::now(); std::printf ("\n== %s   [t=%.0fs]\n", s, std::chrono::duration<double> (std::chrono::steady_clock::now() - t0).count()); }

static const double FS = 48000.0;
static size_t at (double s) { return (size_t) (s * FS); }

// ---- measurements ------------------------------------------------------------------------------------------------------
// mean |deviation| in cents of harmonics nLo..nHi from the exact harmonic series
static double inharmonicityCents (const Buf& x, size_t start, double f0, int nLo, int nHi)
{
    double acc = 0.0; int c = 0;
    for (int n = nLo; n <= nHi; ++n)
    {
        const double f = ana::peakFreq (x, start, 32768, n * f0, FS, 0.45 / n);
        acc += std::fabs (ana::cents (f, n * f0)); ++c;
    }
    return acc / c;
}
// mean peak-to-valley (dB) inside +-0.5 f0 around harmonics: how "harmonic" the spectrum still is
static double harmonicContrastDb (const Buf& x, size_t start, double f0, int nLo = 4, int nHi = 14)
{
    const auto m = ana::spectrum (x, start, 32768, 32768);
    const double bw = FS / 32768.0;
    double acc = 0.0; int c = 0;
    for (int n = nLo; n <= nHi; ++n)
    {
        double hi = 0.0, lo = 1e30;
        for (int k = (int) ((n - 0.5) * f0 / bw); k <= (int) ((n + 0.5) * f0 / bw); ++k) { hi = std::max (hi, m[(size_t) k]); lo = std::min (lo, m[(size_t) k]); }
        acc += ana::db (hi) - ana::db (lo); ++c;
    }
    return acc / c;
}
// std (dB) over time of the level of several bands: how much the timbre moves
static double timbreMotionDb (const Buf& x, size_t from, size_t to, size_t step)
{
    const double edges[] = { 150, 300, 600, 1200, 2400, 4800, 9600 };
    double acc = 0.0; int cnt = 0;
    for (int b = 0; b < 6; ++b)
    {
        std::vector<double> v;
        for (size_t s = from; s + 8192 <= to; s += step) v.push_back (ana::bandDb (x, s, 8192, edges[b], edges[b + 1], FS, 8192));
        double m = 0; for (double d : v) m += d; m /= (double) v.size();
        double var = 0; for (double d : v) var += (d - m) * (d - m);
        acc += std::sqrt (var / (double) v.size()); ++cnt;
    }
    return acc / cnt;
}
static double maxStep (const Buf& x, size_t a, size_t b)
{
    double m = 0.0;
    for (size_t i = a + 1; i < b && i < x.size(); ++i) m = std::max (m, (double) std::fabs (x[i] - x[i - 1]));
    return m;
}
static double timeTo (const std::vector<umbral::Diagnostics>& tr, int every, double frac, float umbral::Diagnostics::*field)
{
    float fin = 0.0f;
    for (size_t i = tr.size() * 8 / 10; i < tr.size(); ++i) fin = std::max (fin, tr[i].*field);
    for (size_t i = 0; i < tr.size(); ++i)
        if (tr[i].*field >= frac * fin) return (double) i * every / FS;
    return 1e9;
}
static bool finiteBounded (const Buf& x, float lim) { for (float v : x) if (!std::isfinite (v) || std::fabs (v) > lim) return false; return true; }

// ---- tests -------------------------------------------------------------------------------------------------------------
static void testReconstruction()
{
    section ("T01/T02  Perfect reconstruction, latency, sample rates, block sizes, mono/stereo");
    std::mt19937 rng (7);
    for (double fs : { 44100.0, 48000.0, 88200.0, 96000.0, 192000.0 })
    {
        const auto x = sig::pluck (146.83, 1.6, fs);
        Options o; o.fs = fs; o.block = 1 + (int) (rng() % 700);
        const auto r = render (x, make (0.5f, 0.5f, 0.3f, 0.3f, 0.0f, 1.0f), o);
        const double e = ana::errorDb (r.L, x, (size_t) (0.1 * fs), x.size() - (size_t) (0.2 * fs));
        check (e < -100.0, "fs=%6.0f block=%3d  depth 0 reproduces the input: %.1f dB (latency %d samples = %.1f ms)", fs, o.block, e, r.latency, 1000.0 * r.latency / fs);
    }
    {   // impulse: the peak must appear exactly at the reported latency
        Buf imp (30000, 0.0f); imp[1000] = 1.0f;
        const auto r = render (imp, make (0.5f, 0.5f, 0.3f, 0.3f, 0.0f, 1.0f));
        size_t pk = 0; for (size_t i = 0; i < r.L.size(); ++i) if (std::fabs (r.L[i]) > std::fabs (r.L[pk])) pk = i;
        check (pk == 1000 && std::fabs (r.L[1000] - 1.0f) < 1e-4f, "impulse at 1000 comes back at 1000 after latency removal (peak at %zu = %.5f)", pk, r.L[pk]);
    }
    {   // stereo, odd block sizes
        const auto a = sig::pluck (110, 2.0, FS), b = sig::pluck (165, 2.0, FS);
        Options o; o.block = 333;
        const auto r = render (a, &b, make (0.5f, 0.5f, 0.3f, 0.3f, 0.0f, 1.0f), o);
        check (ana::errorDb (r.L, a, 4800, a.size() - 9600) < -100 && ana::errorDb (r.R, b, 4800, b.size() - 9600) < -100, "stereo depth 0 reproduces both channels independently");
    }
}

static void testBlockAndDeterminism()
{
    section ("T03/T04  Block-size independence, determinism, seed");
    const auto a = sig::arpeggio ({ 110, 138.6, 164.8, 220 }, 0.6, 5.0, FS), b = sig::pluck (196, 5.0, FS);
    const Params p = make (0.6f, 0.6f, 0.5f, 0.2f, 0.8f, 0.8f, -3.0f);
    Result ref;
    bool identical = true;
    for (int bs : { 1, 7, 64, 480, 1024, 4096 })
    {
        Options o; o.block = bs;
        const auto r = render (a, &b, p, o);
        if (bs == 1) { ref = r; continue; }
        if (r.L != ref.L || r.R != ref.R) identical = false;
    }
    check (identical, "output is bit-identical for block sizes 1, 7, 64, 480, 1024, 4096");
    Options o1, o2; o2.seed = 2;
    const auto r1 = render (a, &b, p, o1), r1b = render (a, &b, p, o1), r2 = render (a, &b, p, o2);
    check (r1.L == r1b.L && r1.R == r1b.R, "same seed + same input -> bit-identical output (reproducible drift)");
    check (r1.L != r2.L, "a different seed gives a different (but equally valid) evolution (%.1f dB apart)", ana::errorDb (r2.L, r1.L, at (1), at (3)));
}

static void testRobustness()
{
    section ("T05/T06  Silence, NaN/Inf, huge levels, noise, parameter fuzz");
    std::mt19937 rng (3);
    auto rnd = [&] { return (float) (rng() % 1001) / 1000.0f; };
    {
        const Buf z (at (2));
        bool zero = true;
        for (float m : { 0.0f, 1.0f }) { const auto r = render (z, make (m, 1.0f, 1.0f, 0.0f, 1.0f, 1.0f, 12.0f)); for (float v : r.L) if (v != 0.0f) zero = false; }
        check (zero, "digital silence in -> exactly silence out, even with every control at its extreme");
    }
    {
        auto x = sig::pluck (220, 3.0, FS);
        x[5000] = std::nanf (""); x[5001] = INFINITY; x[9000] = -INFINITY; x[12000] = 1.0e9f;
        const auto r = render (x, make (0.5f, 1.0f, 1.0f, 0.0f, 1.0f, 1.0f));
        check (finiteBounded (r.L, 1.0e5f), "NaN / Inf / 1e9 samples in the input never reach the output");
    }
    {
        const auto n = sig::noise (4.0, FS, 1.0f);
        bool ok = true; float worst = 0.0f;
        for (int i = 0; i < 12; ++i)
        {
            const auto r = render (n, make (rnd(), rnd(), rnd(), rnd(), rnd(), 1.0f));
            ok = ok && finiteBounded (r.L, 40.0f);
            for (float v : r.L) worst = std::max (worst, std::fabs (v));
        }
        check (ok, "full-scale white noise, 12 random settings: finite, peak %.2f", worst);
    }
    {   // automation every block (fuzz) while playing a rich stereo signal
        umbral::Engine e; e.prepare (FS, 512);
        const auto a = sig::chord ({ 82.4, 123.5, 196, 233 }, 8.0, FS), b = sig::distorted (sig::pluck (110, 8.0, FS));
        Buf l = a, r = b; bool ok = true;
        for (size_t pos = 0; pos + 256 <= l.size(); pos += 256)
        {
            e.setParams (make (rnd(), rnd(), rnd(), rnd(), rnd(), rnd(), -24.0f + 36.0f * rnd()));
            if (rng() % 97 == 0) e.requestReset();
            float* ch[2] = { l.data() + pos, r.data() + pos };
            e.process (ch, 2, 256);
        }
        ok = finiteBounded (l, 60.0f) && finiteBounded (r, 60.0f);
        check (ok, "random automation of all controls every 256 samples (+ random resets): finite and bounded");
    }
    {
        auto x = sig::concat (sig::pluck (196, 1.5, FS), sig::silence (3.0, FS));
        const auto r = render (x, make (1.0f, 1.0f, 1.0f, 0.0f, 1.0f, 1.0f));
        const double tail = ana::rms (r.L, at (1.5) + 2 * r.latency, at (1.0));
        check (tail == 0.0, "memory is not an audible delay: after the note ends the output is exactly silent (tail rms %.3g) even with MEMORY=1", tail);
    }
}

static void testBlendOutput()
{
    section ("T07/T08  BLEND and OUTPUT");
    const auto x = sig::chord ({ 110, 164.8, 220, 277 }, 6.0, FS);
    const Params base = make (0.6f, 0.8f, 0.5f, 0.1f, 1.0f, 1.0f);
    Params p = base; p.blend = 0.0f;
    const auto dry = render (x, p);
    check (ana::errorDb (dry.L, x, 0, x.size()) < -140.0, "BLEND 0 % = the original signal, bit-exact (%.1f dB)", ana::errorDb (dry.L, x, 0, x.size()));
    p.blend = 1.0f; const auto wet = render (x, p);
    double prev = -1e9; bool mono = true;
    for (float b : { 0.2f, 0.4f, 0.6f, 0.8f, 1.0f })
    {
        p.blend = b; const auto r = render (x, p);
        const double d = ana::errorDb (r.L, x, at (2), at (3));
        if (d < prev - 0.3) mono = false;
        prev = d;
    }
    check (mono, "distance to the dry signal grows monotonically with BLEND");
    p.blend = 0.5f; const auto half = render (x, p);
    const double lv = ana::db (ana::rms (half.L, at (2), at (3)) / ana::rms (x, at (2), at (3)));
    check (std::fabs (lv) < 1.5, "BLEND 50 % keeps the level within 1.5 dB of the input (%.2f dB) even though wet is decorrelated", lv);
    {
        const double wetErr = ana::errorDb (wet.L, x, at (2), at (3));
        check (wetErr > -12.0, "BLEND 100 %% is the fully processed signal (differs from dry: %.1f dB)", wetErr);
    }
    {   // OUTPUT
        const auto dryX = sig::sine (330, 2.0, FS, 0.25f);
        for (float g : { -24.0f, -6.0f, 6.0f, 12.0f })
        {
            const auto r = render (dryX, make (0.5f, 0.5f, 0.3f, 0.3f, 0.0f, 0.0f, g));
            const double got = ana::db (ana::rms (r.L, at (0.5), at (1)) / ana::rms (dryX, at (0.5), at (1)));
            check (std::fabs (got - g) < 0.05, "OUTPUT %+5.1f dB -> measured %+.3f dB", g, got);
        }
        // a sudden jump must be smoothed (no step in the waveform)
        umbral::Engine e; e.setParams (make (0.5f, 0.5f, 0.3f, 0.3f, 0.5f, 0.6f, 0.0f)); e.prepare (FS, 256);
        Buf l = sig::sine (330, 2.0, FS, 0.25f);
        const double sineStep = maxStep (l, 100, 2000);
        double worst = 0.0;
        for (size_t pos = 0; pos + 256 <= l.size(); pos += 256)
        {
            if (pos == 256 * 100) e.setParams (make (0.5f, 0.5f, 0.3f, 0.3f, 0.5f, 0.6f, 12.0f));
            if (pos == 256 * 250) e.setParams (make (0.0f, 0.5f, 0.3f, 0.3f, 0.5f, 0.0f, -24.0f));
            float* ch[1] = { l.data() + pos };
            e.process (ch, 1, 256);
            if (pos > 256 * 90) worst = std::max (worst, maxStep (l, pos, pos + 256));
        }
        check (worst < 4.2 * sineStep * 1.1, "OUTPUT/BLEND jumps (0 -> +12 dB, then -> -24 dB, BLEND -> 0) are smoothed: largest sample step %.4f vs %.4f", worst, 4.0 * sineStep);
    }
}

static void testErosion()
{
    section ("T10  EROSION: selective degradation towards inharmonic/abstract timbre");
    const double f0 = 220.0;
    const auto x = sig::harmonicNote (f0, 9.0, FS, 24, 0.9, 1.0e6);
    double prevInh = -1.0, prevCon = 1e9; bool inc = true, dec = true;
    double inh[4], con[4]; int i = 0;
    for (float e : { 0.0f, 0.2f, 0.4f, 0.7f })
    {
        const auto r = render (x, make (0.5f, e, 0.0f, 0.1f, 0.9f, 1.0f));
        inh[i] = inharmonicityCents (r.L, at (6), f0, 6, 16);
        con[i] = harmonicContrastDb (r.L, at (6), f0);
        if (inh[i] < prevInh) inc = false;
        if (con[i] > prevCon + 0.5) dec = false;
        prevInh = inh[i]; prevCon = con[i]; ++i;
    }
    check (inh[0] < 3.0, "EROSION 0: harmonics stay on the series (mean deviation %.1f cents)", inh[0]);
    check (inc && inh[3] > 25.0, "inharmonicity grows with EROSION: %.1f / %.1f / %.1f / %.1f cents", inh[0], inh[1], inh[2], inh[3]);
    check (dec && con[0] - con[3] > 15.0, "harmonic structure (peak-to-valley) is lost progressively: %.1f / %.1f / %.1f / %.1f dB", con[0], con[1], con[2], con[3]);
    {   // not a conventional filter: the spectral tilt barely changes at moderate settings while the structure does
        const auto r0 = render (x, make (0.5f, 0.0f, 0.0f, 0.1f, 0.9f, 1.0f)), r1 = render (x, make (0.5f, 0.4f, 0.0f, 0.1f, 0.9f, 1.0f));
        double worst = 0.0;
        for (auto band : { std::pair<double, double> { 200, 800 }, { 800, 3200 }, { 3200, 12800 } })
            worst = std::max (worst, std::fabs (ana::bandDb (r1.L, at (6), 32768, band.first, band.second, FS) - ana::bandDb (r0.L, at (6), 32768, band.first, band.second, FS)));
        check (worst < 4.0, "EROSION 0.4 moves no octave-wide band by more than 4 dB (%.1f dB): it is not a low/high-pass", worst);
    }
    {   // continuous input must not turn into sudden artefacts at moderate settings: envelope of a pure partial stays steady
        const auto s = sig::sine (1500, 7.0, FS, 0.3f);   // measured from 3 s to 6 s: the last 20 ms before the file ends would show the edge, not the effect
        const auto r = render (s, make (0.5f, 0.3f, 0.3f, 0.1f, 0.5f, 1.0f));
        std::vector<double> v; for (size_t a = at (3); a + 960 <= at (6); a += 960) v.push_back (ana::rms (r.L, a, 960));
        double mn = 1e9, mx = 0; for (double d : v) { mn = std::min (mn, d); mx = std::max (mx, d); }
        check (ana::db (mx / mn) < 1.5, "moderate settings on a sustained partial: 20 ms envelope varies only %.2f dB (no flutter)", ana::db (mx / mn));
    }
}

static void testDrift()
{
    section ("T11  DRIFT: static at 0, slow smooth evolution above, deterministic");
    const double f0 = 196.0;
    const auto x = sig::harmonicNote (f0, 14.0, FS, 24, 0.9, 1.0e6);
    double mot[4], jit[4]; int i = 0;
    for (float d : { 0.0f, 0.15f, 0.5f, 1.0f })
    {
        const auto r = render (x, make (0.5f, 0.5f, d, 0.1f, 0.9f, 1.0f));
        mot[i] = timbreMotionDb (r.L, at (5), at (13), 4800);
        std::vector<double> f;   // partial 11, sampled every 100 ms
        for (size_t s = at (5); s + 16384 <= at (13); s += 4800) f.push_back (ana::cents (ana::peakFreq (r.L, s, 16384, 11 * f0, FS, 0.045, 16384), 11 * f0));
        double m = 0; for (double v : f) m += v; m /= (double) f.size();
        double var = 0; for (double v : f) var += (v - m) * (v - m);
        jit[i] = std::sqrt (var / (double) f.size());
        ++i;
    }
    check (mot[0] < 0.6, "DRIFT 0: fully static, band levels move only %.2f dB over 8 s", mot[0]);
    check (mot[1] < mot[2] && mot[2] < mot[3] && mot[3] > 3.0 * mot[0] + 1.0, "timbre motion grows with DRIFT: %.2f / %.2f / %.2f / %.2f dB", mot[0], mot[1], mot[2], mot[3]);
    check (jit[0] < 2.0 && jit[3] > 3.0 * jit[0], "partial 11 wanders %.1f / %.1f / %.1f / %.1f cents (std)", jit[0], jit[1], jit[2], jit[3]);
    {   // smoothness: no abrupt jumps in the partial trajectory at max drift
        const auto r = render (x, make (0.5f, 0.5f, 1.0f, 0.1f, 0.9f, 1.0f));
        double prev = 0, worst = 0, range = 0, lo = 1e9, hi = -1e9; bool first = true;
        for (size_t s = at (5); s + 16384 <= at (13); s += 2400)
        {
            const double c = ana::cents (ana::peakFreq (r.L, s, 16384, 11 * f0, FS, 0.045, 16384), 11 * f0);
            if (!first) worst = std::max (worst, std::fabs (c - prev));
            prev = c; first = false; lo = std::min (lo, c); hi = std::max (hi, c);
        }
        range = hi - lo;
        check (worst < 0.5 * std::max (range, 20.0), "max DRIFT: largest 50 ms step of a partial = %.1f cents against a %.1f cent range (smooth, no jumps)", worst, range);
    }
    {   // restart() (transport start) returns to exactly the state of a fresh instance
        const auto sh = sig::pluck (146.8, 4.0, FS, 20.0);
        const Params p = make (0.6f, 0.6f, 0.6f, 0.1f, 0.8f, 1.0f);
        const auto fresh = render (sh, p);
        umbral::Engine e; e.setParams (p); e.prepare (FS, 512);
        Buf junk = sig::chord ({ 100, 150, 233 }, 3.3, FS);
        for (size_t pos = 0; pos + 512 <= junk.size(); pos += 512) { float* c[1] = { junk.data() + pos }; e.process (c, 1, 512); }
        e.restart();
        Buf again = sh; again.resize (again.size() + (size_t) e.getLatencySamples(), 0.0f);
        for (size_t pos = 0; pos < again.size(); pos += 512) { float* c[1] = { again.data() + pos }; e.process (c, 1, (int) std::min<size_t> (512, again.size() - pos)); }
        Buf al (again.begin() + e.getLatencySamples(), again.end());
        check (al == fresh.L, "restart() at transport start: the evolution is bit-identical to a fresh instance's (every playback sounds the same)");
    }
    {   // requestReset(): state is really cleared
        const auto sus = sig::harmonicNote (146.8, 8.0, FS, 24, 0.9, 1.0e6);
        umbral::Engine e; e.setParams (make (0.8f, 0.6f, 0.6f, 0.1f, 0.9f, 1.0f)); e.prepare (FS, 512);
        Buf l = sus; float before = 0, after = 1;
        for (size_t pos = 0; pos + 512 <= at (6); pos += 512) { float* c[1] = { l.data() + pos }; e.process (c, 1, 512); }
        before = e.getDiagnostics().memory;
        e.requestReset();
        Buf z = sig::silence (0.3, FS);
        for (size_t pos = 0; pos + 512 <= z.size(); pos += 512) { float* c[1] = { z.data() + pos }; e.process (c, 1, 512); }
        after = e.getDiagnostics().memory;
        check (before > 0.5f && after < 0.05f, "requestReset(): timbral memory %.2f -> %.2f (cleared, even with MEMORY 0.8)", before, after);
    }
    {   // click-free reset
        const auto sus = sig::harmonicNote (130.8, 6.0, FS, 24, 0.9, 1.0e6);
        umbral::Engine e; e.setParams (make (0.7f, 0.8f, 0.6f, 0.1f, 1.0f, 1.0f)); e.prepare (FS, 256);
        Buf l = sus; double stepOut = 0.0; const double stepDry = maxStep (sus, 1000, sus.size());
        for (size_t pos = 0; pos + 256 <= l.size(); pos += 256)
        {
            if (pos == 256 * 600) e.requestReset();
            float* c[1] = { l.data() + pos }; e.process (c, 1, 256);
            if (pos > 256 * 560 && pos < 256 * 700) stepOut = std::max (stepOut, maxStep (l, pos, pos + 256));
        }
        check (stepOut < 2.0 * stepDry, "requestReset() mid-note: largest sample step %.4f vs %.4f for the dry waveform (no click)", stepOut, stepDry);
    }
}

static void testMemory()
{
    section ("T12  MEMORY: earlier sound shapes the next, dissipates, never replays audio");
    const double fA = 196.0, fB = 233.1;
    auto note = [&] (double f, double d) { return sig::pluck (f, d, FS, 30.0); };
    auto scenario = [&] (float, double gap) {
        Buf x = note (fA, 3.0);
        x = sig::concat (x, sig::silence (gap, FS));
        const size_t onset = x.size();
        x = sig::concat (x, note (fB, 2.0));
        return std::make_pair (x, onset);
    };
    for (double gap : { 0.0, 1.5 })
    {
        float ageLo = 0, ageHi = 0; double dist = 0;
        Buf outLo, outHi;
        for (float mem : { 0.0f, 1.0f })
        {
            auto sc = scenario (mem, gap);
            const auto r = render (sc.first, make (mem, 0.6f, 0.0f, 0.2f, 0.9f, 1.0f));
            const size_t idx = (sc.second + at (0.15)) / 480;
            (mem == 0.0f ? ageLo : ageHi) = r.trace[std::min (idx, r.trace.size() - 1)].age;
            (mem == 0.0f ? outLo : outHi) = r.L;
            if (mem == 1.0f) dist = ana::bandDistance (outLo, outHi, sc.second + at (0.1), at (0.4), FS);
        }
        if (gap == 0.0)
            check (ageHi > ageLo + 0.15, "legato A->B: partials of the new note start with persistence %.2f (MEMORY 1) vs %.2f (MEMORY 0)", ageHi, ageLo);
        else
            check (ageHi > ageLo + 0.1, "after a %.1f s gap the memory is %s: new note starts at age %.2f (MEMORY 1) vs %.2f (MEMORY 0)", gap, "still there only when MEMORY is high", ageHi, ageLo);
        check (dist > 0.8, "gap %.1f s: the first 400 ms of the new note sound different (%.2f dB band distance) depending on MEMORY", gap, dist);
    }
    {   // a single sustained note is unaffected by MEMORY (it acts through history, independent of the other controls)
        const auto s = sig::pluck (196, 8.0, FS, 30.0);
        const auto a = render (s, make (0.0f, 0.5f, 0.0f, 0.2f, 0.8f, 1.0f)), b = render (s, make (1.0f, 0.5f, 0.0f, 0.2f, 0.8f, 1.0f));
        const double d = ana::bandDistance (a.L, b.L, at (5), at (2), FS);
        check (d < 2.0, "one sustained note: MEMORY 0 vs 1 differ by only %.2f dB (memory acts through history, not on the present)", d);
    }
    {   // dissipation time grows with MEMORY
        double t[3]; int i = 0;
        for (float mem : { 0.1f, 0.5f, 0.9f })
        {
            const Buf x = sig::concat (note (fA, 4.0), sig::silence (25.0, FS));
            const auto r = render (x, make (mem, 0.6f, 0.0f, 0.2f, 0.9f, 1.0f));
            float peak = 0; for (size_t k = 0; k < at (4) / 480; ++k) peak = std::max (peak, r.trace[k].memory);
            size_t k = at (4) / 480; while (k < r.trace.size() && r.trace[k].memory > 0.1f * peak) ++k;
            t[i++] = (double) (k * 480) / FS - 4.0;
        }
        check (t[0] < t[1] && t[1] < t[2] && t[0] < 0.6 && t[2] > 3.0, "memory fades to 10 %% in %.2f s / %.2f s / %.2f s for MEMORY 0.1 / 0.5 / 0.9", t[0], t[1], t[2]);
    }
}

static void testThreshold()
{
    section ("T13  THRESHOLD: sustained notes bloom progressively, background noise stays natural, transients are protected");
    auto note = sig::pluck (164.8, 12.0, FS, 40.0);
    for (float& v : note) v *= 3.0f;                      // strong playing (about -10 dBFS rms) so that every THRESHOLD setting is fully open
    double t[3]; int i = 0;
    for (float thr : { 0.1f, 0.5f, 0.9f })
    {
        const auto r = render (note, make (0.5f, 0.6f, 0.2f, thr, 0.8f, 1.0f));
        t[i++] = timeTo (r.trace, 480, 0.5, &umbral::Diagnostics::transformation);
    }
    check (t[0] < t[1] && t[1] < t[2] && t[0] > 0.05 && t[2] > 1.5 && t[2] > 5.0 * t[0], "time to reach half the final transformation: %.2f s / %.2f s / %.2f s for THRESHOLD 0.1 / 0.5 / 0.9", t[0], t[1], t[2]);
    {   // the first moments of a note are natural
        const auto r = render (note, make (0.5f, 0.6f, 0.2f, 0.5f, 0.9f, 1.0f));
        const double early = ana::errorDb (r.L, note, at (0.0), at (0.05)), late = ana::errorDb (r.L, note, at (8), at (1));
        check (early < late - 12.0, "THRESHOLD 0.5: first 50 ms %.1f dB from the dry note vs %.1f dB once developed (natural start)", early, late);
    }
    {   // background noise: a quiet hum below the threshold must not trigger the engine
        const auto hum = sig::harmonicNote (100, 6.0, FS, 20, 0.5, 1e6);
        Buf quiet = hum; for (float& v : quiet) v *= 0.01f;                        // about -60 dBFS rms
        double tr[2]; int k = 0;
        for (float thr : { 0.9f, 0.05f }) { const auto r = render (quiet, make (0.5f, 0.8f, 0.2f, thr, 1.0f, 1.0f)); tr[k++] = r.trace[r.trace.size() * 3 / 4].transformation; }
        check (tr[0] < 0.01 && tr[1] > 0.05, "-60 dBFS background: transformation %.3f at THRESHOLD 0.9 (ignored) vs %.3f at 0.05 (reacts)", tr[0], tr[1]);
    }
    {   // pumping: a tremolo hovering around the threshold must not make the engagement chatter
        Buf x = sig::pluck (146.8, 10.0, FS, 60.0);
        for (size_t n = 0; n < x.size(); ++n) x[n] *= 0.063f * (float) std::pow (10.0, 1.5 * std::sin (2 * 3.14159265 * 5.0 * (double) n / FS) / 20.0);
        const auto r = render (x, make (0.5f, 0.6f, 0.2f, 0.45f, 0.8f, 1.0f));
        double worst = 0; for (size_t j = r.trace.size() / 3; j + 1 < r.trace.size(); ++j) worst = std::max (worst, (double) std::fabs (r.trace[j + 1].engagement - r.trace[j].engagement));
        check (worst < 0.06, "5 Hz level wobble around the threshold: engagement moves at most %.3f per 10 ms (no pumping)", worst);
    }
}

static void testDepth()
{
    section ("T09  DEPTH: intensity AND spectral reach, not a wet/dry control");
    const double f0 = 110.0;
    const auto x = sig::harmonicNote (f0, 9.0, FS, 40, 0.9, 1.0e6);
    {
        const auto r = render (x, make (0.5f, 0.8f, 0.2f, 0.1f, 0.0f, 1.0f));
        check (ana::errorDb (r.L, x, at (1), at (7)) < -100, "DEPTH 0 = transparent (%.1f dB)", ana::errorDb (r.L, x, at (1), at (7)));
        double prev = -200; bool mono = true; double v[5]; int i = 0;
        for (float d : { 0.0f, 0.25f, 0.5f, 0.75f, 1.0f })
        {
            const auto q = render (x, make (0.5f, 0.8f, 0.2f, 0.1f, d, 1.0f));
            v[i] = ana::bandDistance (q.L, x, at (6), 32768, FS); if (v[i] < prev - 0.3) mono = false; prev = v[i]; ++i;
        }
        check (mono && v[4] > v[1] + 2.0, "spectral distance from dry rises with DEPTH: %.1f / %.1f / %.1f / %.1f / %.1f dB", v[0], v[1], v[2], v[3], v[4]);
    }
    {   // reach: at low DEPTH only the upper partials are touched; the foundation keeps its identity
        const auto dry = render (x, make (0.5f, 0.8f, 0.0f, 0.1f, 0.0f, 1.0f));
        auto lowChange = [&] (const Buf& y) { double acc = 0; for (int n : { 2, 3, 4 }) acc += std::fabs (ana::peakLevelDb (y, at (6), 32768, n * f0, FS, 0.4 / n) - ana::peakLevelDb (dry.L, at (6), 32768, n * f0, FS, 0.4 / n)) / 3.0; return acc; };
        auto highShift = [&] (const Buf& y) { double acc = 0; for (int n : { 20, 24, 28 }) acc += std::fabs (ana::cents (ana::peakFreq (y, at (6), 32768, n * f0, FS, 0.4 / n), n * f0)) / 3.0; return acc; };
        const auto lo = render (x, make (0.5f, 0.8f, 0.0f, 0.1f, 0.3f, 1.0f)), full = render (x, make (0.5f, 0.8f, 0.0f, 0.1f, 1.0f, 1.0f));
        const double l3 = lowChange (lo.L), l10 = lowChange (full.L), h3 = highShift (lo.L), h10 = highShift (full.L);
        check (l3 < 1.0 && h3 > 6.0, "DEPTH 0.3: partials 2-4 change %.2f dB, partials 20-28 are detuned %.1f cents (only the upper register is reached)", l3, h3);
        check (l10 > l3 + 2.0 && h10 > h3, "DEPTH 1: the lowest partials are reached too (%.2f dB change) and the upper ones move further (%.1f cents)", l10, h10);
    }
    {   // DEPTH 0.5 / BLEND 100 vs DEPTH 1 / BLEND 50: a wet/dry knob could not tell them apart
        const auto a = render (x, make (0.5f, 0.8f, 0.0f, 0.1f, 0.5f, 1.0f)), b = render (x, make (0.5f, 0.8f, 0.0f, 0.1f, 1.0f, 0.5f));
        const double nominalA = ana::peakLevelDb (a.L, at (6), 32768, 2 * f0, FS, 0.01), nominalB = ana::peakLevelDb (b.L, at (6), 32768, 2 * f0, FS, 0.01);
        const double hiA = ana::peakLevelDb (a.L, at (6), 32768, 25 * f0, FS, 0.01), hiB = ana::peakLevelDb (b.L, at (6), 32768, 25 * f0, FS, 0.01);
        const double d = ana::bandDistance (a.L, b.L, at (6), 32768, FS);
        check (d > 1.5, "DEPTH 0.5/BLEND 100 vs DEPTH 1/BLEND 50 are different sounds (%.1f dB band distance; partial 2 %.0f vs %.0f dB, partial 25 %.0f vs %.0f dB)", d, nominalA, nominalB, hiA, hiB);
    }
}

static void testIndependenceMatrix()
{
    section ("T19  Every control is audible and has its own signature (low -> high, others at default)");
    const double f0 = 164.81;
    auto sus = sig::harmonicNote (f0, 22.0, FS, 28, 0.9, 1.0e6);
    for (float& v : sus) v *= 2.5f;   // about -12 dBFS rms: THRESHOLD 1 (-18 dB) is still open
    const auto arp = sig::arpeggio ({ 164.81, 207.65, 246.94, 329.63 }, 0.7, 8.0, FS, 3.0);
    const Params def = make (0.5f, 0.45f, 0.35f, 0.2f, 0.7f, 1.0f);
    struct Row { const char* name; float Params::*field; };
    const Row rows[] = { { "MEMORY", &Params::memory }, { "EROSION", &Params::erosion }, { "DRIFT", &Params::drift },
                         { "THRESHOLD", &Params::threshold }, { "DEPTH", &Params::depth }, { "BLEND", &Params::blend } };
    std::printf ("  %-10s | developed note: spectrum | partial detune | timbre motion (13-21 s) | arpeggio: spectrum | transformation at 0.6 s\n", "control");
    double steadyD[6], inhD[6], motD[6], arpD[6], earlyD[6];
    for (int r = 0; r < 6; ++r)
    {
        Params a = def, b = def; a.*(rows[r].field) = 0.0f; b.*(rows[r].field) = 1.0f;
        const auto sa = render (sus, a), sb = render (sus, b), aa = render (arp, a), ab = render (arp, b);
        steadyD[r] = ana::bandDistance (sa.L, sb.L, at (12), 32768, FS);
        inhD[r] = std::fabs (inharmonicityCents (sa.L, at (12), f0, 6, 16) - inharmonicityCents (sb.L, at (12), f0, 6, 16));
        motD[r] = std::fabs (timbreMotionDb (sa.L, at (13), at (21), 4800) - timbreMotionDb (sb.L, at (13), at (21), 4800));
        arpD[r] = 0.0; for (double on : { 2.1, 2.8, 3.5, 4.2, 4.9, 5.6 }) arpD[r] += ana::bandDistance (aa.L, ab.L, at (on) + at (0.05), at (0.35), FS, 80.0, 12000.0, 1.26) / 6.0;   // right after six note onsets
        earlyD[r] = std::fabs (sa.trace[at (0.6) / 480].transformation - sb.trace[at (0.6) / 480].transformation);
        std::printf ("  %-10s | %20.2f dB | %10.1f ct | %17.2f dB | %14.2f dB | %.3f\n", rows[r].name, steadyD[r], inhD[r], motD[r], arpD[r], earlyD[r]);
    }
    for (int r = 0; r < 6; ++r) check (std::max ({ steadyD[r], arpD[r], inhD[r] / 5.0 }) > 1.0, "%s changes the sound audibly (largest effect %.1f)", rows[r].name, std::max ({ steadyD[r], arpD[r], inhD[r] / 5.0 }));
    check (motD[2] > 2.0 * std::max ({ motD[0], motD[1], motD[3], motD[4], 0.15 }), "DRIFT is the control that moves a developed timbre in time (%.2f dB vs others <= %.2f)", motD[2], std::max ({ motD[0], motD[1], motD[3], motD[4] }));
    check (earlyD[3] > 0.05 && earlyD[3] > 2.0 * std::max ({ earlyD[0], earlyD[2], earlyD[5] }), "THRESHOLD is the control that sets how fast the transformation arrives (%.3f vs others <= %.3f)", earlyD[3], std::max ({ earlyD[0], earlyD[2], earlyD[5] }));
    check (steadyD[3] < 0.5 * std::max (steadyD[1], steadyD[4]), "...and it leaves the developed sound alone (%.2f dB vs %.2f dB for EROSION/DEPTH)", steadyD[3], std::max (steadyD[1], steadyD[4]));
    check (arpD[0] > 1.0 && arpD[0] > steadyD[0] * 0.8, "MEMORY matters in the arpeggio (%.2f dB) as much as it does little on a held note (%.2f dB)", arpD[0], steadyD[0]);
    check (inhD[1] > 2.0 * std::max ({ inhD[0], inhD[2], inhD[3], 1.0 }), "EROSION is the control that detunes the partials (%.1f ct vs others <= %.1f)", inhD[1], std::max ({ inhD[0], inhD[2], inhD[3] }));
}

// "ordinary" chain: low-pass -> tanh saturation -> modulated-delay chorus. Used only as a yardstick.
static Buf conventionalChain (const Buf& x)
{
    Buf y (x.size());
    double z1 = 0, z2 = 0;
    const double a = std::exp (-2 * 3.14159265 * 3500.0 / FS);
    std::vector<float> dl (4096, 0.0f); size_t wp = 0;
    for (size_t n = 0; n < x.size(); ++n)
    {
        z1 = (1 - a) * x[n] + a * z1; z2 = (1 - a) * z1 + a * z2;
        const float s = std::tanh (3.0f * (float) z2) / 3.0f * 1.5f;
        dl[wp] = s;
        const double d = 0.004 * FS * (1.0 + 0.5 * std::sin (2 * 3.14159265 * 0.4 * (double) n / FS));
        const double rp = (double) wp - d; const long i0 = (long) std::floor (rp); const double fr = rp - (double) i0;
        const float dly = (1 - (float) fr) * dl[(size_t) ((i0 % 4096 + 4096) % 4096)] + (float) fr * dl[(size_t) (((i0 + 1) % 4096 + 4096) % 4096)];
        y[n] = 0.5f * s + 0.5f * dly;
        wp = (wp + 1) % 4096;
    }
    return y;
}

static void testNotACommonChain()
{
    section ("T14  Compared with filter + saturation + modulation");
    const double f0 = 196.0;
    const auto x = sig::harmonicNote (f0, 14.0, FS, 28, 0.9, 1.0e6);
    const auto base = conventionalChain (x);
    const auto um = render (x, make (0.6f, 0.45f, 0.4f, 0.3f, 0.7f, 1.0f));
    auto early = [&] (const Buf& b) { return inharmonicityCents (b, at (0.05), f0, 6, 16); };   // 0.68 s window starting at 50 ms
    auto late = [&] (const Buf& b) { return inharmonicityCents (b, at (11), f0, 6, 16); };
    const double eB = early (base), lB = late (base), eU = early (um.L), lU = late (um.L);
    const double cB = harmonicContrastDb (base, at (11), f0) - harmonicContrastDb (base, at (0.05), f0);
    const double cU = harmonicContrastDb (um.L, at (11), f0) - harmonicContrastDb (um.L, at (0.05), f0);
    std::printf ("  baseline: partial deviation %.1f ct early -> %.1f ct late, harmonic contrast change %+.1f dB\n", eB, lB, cB);
    std::printf ("  UMBRAL  : partial deviation %.1f ct early -> %.1f ct late, harmonic contrast change %+.1f dB\n", eU, lU, cU);
    check (lB < 3.0, "a low-pass + saturation + chorus keeps the partials on the harmonic series (%.1f ct)", lB);
    check (lU > lB + 12.0, "UMBRAL pushes them off it (%.1f ct)", lU);
    check (std::fabs (lB - eB) < 3.0 && lU - eU > 8.0, "...progressively: the conventional chain does not evolve (%.1f -> %.1f ct), UMBRAL does (%.1f -> %.1f ct)", eB, lB, eU, lU);
}

static void testStereo()
{
    section ("T15  Mono, stereo, hard-panned material");
    const Params p = make (0.6f, 0.6f, 0.5f, 0.2f, 0.8f, 1.0f);
    const auto a = sig::chord ({ 98, 147, 196, 247 }, 5.0, FS), b = sig::pad ({ 110, 165 }, 5.0, FS);
    {
        const auto r = render (a, &a, p);
        const double lr = ana::errorDb (r.R, r.L, 0, r.L.size());
        check (lr < -110.0, "identical L/R in -> identical L/R out (mono stays mono: L-R is %.1f dB)", lr);
        const auto m = render (a, p);
        const double mm = ana::errorDb (m.L, r.L, 0, r.L.size());
        check (mm < -110.0, "a mono instance and a stereo instance with L=R agree (%.1f dB)", mm);
    }
    {
        const Buf z (a.size(), 0.0f);
        const auto r = render (a, &z, p);
        check (ana::rms (r.R, 0, r.R.size()) < 1e-6, "hard-left input leaves the right channel silent (rms %.2g)", ana::rms (r.R, 0, r.R.size()));
    }
    {
        const auto r = render (a, &b, p);
        auto corr = [] (const Buf& u, const Buf& v, size_t s, size_t n) { double uv = 0, uu = 0, vv = 0; for (size_t i = s; i < s + n; ++i) { uv += u[i] * v[i]; uu += u[i] * u[i]; vv += v[i] * v[i]; } return uv / std::sqrt (uu * vv + 1e-30); };
        const double cin = corr (a, b, at (2), at (2)), cout = corr (r.L, r.R, at (2), at (2));
        check (std::fabs (cin - cout) < 0.25, "different material in L and R stays different (inter-channel correlation %.2f in -> %.2f out)", cin, cout);
    }
}

static void testPresetsAndSources()
{
    section ("T16  Factory presets x the six kinds of source (finite, sensible level, not silent)");
    const double fsL = FS;
    struct Src { const char* name; Buf l, r; };
    std::vector<Src> srcs;
    srcs.push_back ({ "clean guitar, single notes", sig::concat (sig::pluck (82.4, 2.0, fsL), sig::concat (sig::pluck (196, 2.0, fsL), sig::pluck (329.6, 2.0, fsL))), {} });
    srcs.push_back ({ "arpeggio, sustained notes", sig::arpeggio ({ 110, 164.8, 196, 247, 329.6 }, 0.45, 6.0, fsL), {} });
    srcs.push_back ({ "dissonant chord", sig::chord ({ 130.8, 138.6, 185.0, 196.0, 277.2 }, 6.0, fsL), {} });
    srcs.push_back ({ "distorted guitar", sig::distorted (sig::chord ({ 82.4, 123.5, 164.8 }, 6.0, fsL)), {} });
    srcs.push_back ({ "pad / synth (stereo)", sig::pad ({ 110, 138.6, 165 }, 6.0, fsL), sig::pad ({ 110.3, 138.2, 165.4 }, 6.0, fsL) });
    srcs.push_back ({ "voice", sig::voice (140, 6.0, fsL), {} });
    srcs.push_back ({ "percussion", sig::drums (6.0, fsL), {} });
    bool allFinite = true, allLevel = true, allAlive = true;
    double worstLo = 0, worstHi = 0;
    const auto& presets = umbral::factoryPresets();
    for (const auto& s : srcs)
        for (const auto& pr : presets)
        {
            Params p = pr.params; p.blend = 1.0f; p.outputDb = 0.0f;
            const auto r = render (s.l, s.r.empty() ? nullptr : &s.r, p);
            allFinite = allFinite && finiteBounded (r.L, 8.0f) && finiteBounded (r.R, 8.0f);
            const double ratio = ana::db (ana::rms (r.L, at (1), at (4)) / std::max (1e-9, ana::rms (s.l, at (1), at (4))));
            worstLo = std::min (worstLo, ratio); worstHi = std::max (worstHi, ratio);
            if (ratio < -12.0 || ratio > 6.0) allLevel = false;
            if (ana::rms (r.L, at (1), at (4)) < 1e-4) allAlive = false;
        }
    check (allFinite, "%zu presets x %zu sources: always finite, peak < 8", presets.size(), srcs.size());
    check (allLevel, "output level stays within -12..+6 dB of the input at BLEND 100 (observed %.1f .. %+.1f dB)", worstLo, worstHi);
    check (allAlive, "no preset silences any source");
}

static void testIdentity()
{
    section ("T20  Musical usefulness: moderate settings keep the identity of the instrument");
    const double f0 = 146.83;
    const auto x = sig::pluck (f0, 6.0, FS, 5.0);
    const auto r = render (x, make (0.5f, 0.3f, 0.3f, 0.3f, 0.5f, 1.0f));
    double dev = 0; for (int n : { 1, 2, 3, 4 }) dev += std::fabs (ana::cents (ana::peakFreq (r.L, at (2), 32768, n * f0, FS, 0.4 / n), n * f0)) / 4.0;
    check (dev < 4.0, "moderate EROSION/DEPTH: the pitch-defining partials 1-4 stay within %.1f cents", dev);
    const double d = ana::bandDistance (r.L, x, at (2), 32768, FS);
    check (d < 5.0, "overall spectral balance stays within %.1f dB (1/3-octave rms) of the original", d);
    const double dd = ana::bandDistance (r.L, x, at (2), 32768, FS, 80, 640, 2.0);
    check (dd < 2.5, "the low register (80-640 Hz, octave bands) is preserved to %.1f dB", dd);
}

static void testAllocationsAndCpu()
{
    section ("T17  Real-time safety and CPU");
    umbral::Engine e; e.setParams (make (0.7f, 0.8f, 0.7f, 0.2f, 1.0f, 0.8f)); e.prepare (FS, 512);
    Buf l = sig::chord ({ 82.4, 123.5, 196, 233 }, 10.0, FS), r = sig::pad ({ 110, 165 }, 10.0, FS);
    g_allocs = 0; g_track = true;
    const auto t0 = std::chrono::steady_clock::now();
    for (size_t pos = 0; pos + 512 <= l.size(); pos += 512)
    {
        e.setParams (make (0.7f, 0.8f, 0.7f, 0.2f, 1.0f, 0.8f, (float) (pos % 7)));
        if (pos == 512 * 300) e.requestReset();
        float* ch[2] = { l.data() + pos, r.data() + pos };
        e.process (ch, 2, 512);
        (void) e.getDiagnostics();
    }
    const auto t1 = std::chrono::steady_clock::now();
    g_track = false;
    const double secs = std::chrono::duration<double> (t1 - t0).count();
    check (g_allocs.load() == 0, "no heap allocation while processing 10 s of stereo (%ld allocations)", g_allocs.load());
    std::printf ("  CPU: %.2f %% of one core for stereo 48 kHz (N=%d, 8x overlap, latency %d samples = %.1f ms)\n", 100.0 * secs / 10.0, e.getFftSize(), e.getLatencySamples(), 1000.0 * e.getLatencySamples() / FS);
    check (secs / 10.0 < 0.15, "CPU load below 15 %% of one core (%.1f %%)", 100.0 * secs / 10.0);
    {   // silence after loud, resonant material must not get more expensive (denormals in recursive state)
        Buf zl (at (10), 0.0f), zr (at (10), 0.0f);
        const auto s0 = std::chrono::steady_clock::now();
        for (size_t pos = 0; pos + 512 <= zl.size(); pos += 512) { float* ch[2] = { zl.data() + pos, zr.data() + pos }; e.process (ch, 2, 512); }
        const double silentSecs = std::chrono::duration<double> (std::chrono::steady_clock::now() - s0).count();
        check (silentSecs < 1.5 * secs + 0.01, "10 s of silence after the signal cost %.2f s vs %.2f s for the signal (no denormal slow-down)", silentSecs, secs);
    }
}

int main()
{
    std::setvbuf (stdout, nullptr, _IOLBF, 0);
    testReconstruction();
    testBlockAndDeterminism();
    testRobustness();
    testBlendOutput();
    testErosion();
    testDrift();
    testMemory();
    testThreshold();
    testDepth();
    testIndependenceMatrix();
    testNotACommonChain();
    testStereo();
    testPresetsAndSources();
    testIdentity();
    testAllocationsAndCpu();
    section ("Numerical health");
    check (harness::g_totalFaults == 0, "no frame in any of the renders above needed the non-finite safety net (%d faults)", harness::g_totalFaults);
    std::printf ("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail;
}
