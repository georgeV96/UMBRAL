#pragma once

// Synthetic test material for the UMBRAL tests and the render tool. Everything is deterministic.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace sig
{
using Buf = std::vector<float>;
constexpr double kPi = 3.14159265358979323846;

inline size_t samples (double seconds, double fs) { return (size_t) std::llround (seconds * fs); }

struct Lcg
{
    uint32_t s = 12345u;
    float next() { s = s * 1664525u + 1013904223u; return (float) (s >> 8) * (2.0f / 16777215.0f) - 1.0f; }
};

inline void normalisePeak (Buf& x, float peak)
{
    float m = 0.0f;
    for (float v : x) m = std::max (m, std::fabs (v));
    if (m > 0.0f) for (float& v : x) v *= peak / m;
}

inline void mixInto (Buf& dst, const Buf& src, size_t offset, float gain = 1.0f)
{
    if (dst.size() < offset + src.size()) dst.resize (offset + src.size(), 0.0f);
    for (size_t i = 0; i < src.size(); ++i) dst[offset + i] += gain * src[i];
}

// Additive harmonic note. amplitude of partial n ~ n^-rolloff, each partial decays with tau / (1 + decayTilt * n).
inline Buf harmonicNote (double f0, double dur, double fs, int maxHarm = 40, double rolloff = 1.1, double tau = 8.0,
                         double decayTilt = 0.0, double attack = 0.004, double phase0 = 0.3)
{
    Buf x (samples (dur, fs), 0.0f);
    // scale so that the steady-state level is about -20 dBFS rms, like a real guitar DI
    double power = 0.0;
    for (int n = 1; n <= maxHarm && f0 * n <= 0.45 * fs; ++n) power += 0.5 * std::pow ((double) n, -2.0 * rolloff);
    const double norm = 0.1 / std::sqrt (std::max (power, 1.0e-9));
    for (int n = 1; n <= maxHarm; ++n)
    {
        const double f = f0 * n;
        if (f > 0.45 * fs) break;
        const double a = norm * std::pow ((double) n, -rolloff), tn = tau / (1.0 + decayTilt * n);
        const double ph = phase0 * n * n;
        for (size_t i = 0; i < x.size(); ++i)
        {
            const double t = (double) i / fs;
            const double env = std::min (1.0, t / attack) * std::exp (-t / tn);
            x[i] += (float) (a * env * std::sin (2.0 * kPi * f * t + ph));
        }
    }
    return x;
}

// Plucked-guitar-like: bright attack, upper partials die first.
inline Buf pluck (double f0, double dur, double fs, double tau = 3.0) { return harmonicNote (f0, dur, fs, 36, 0.9, tau, 0.35, 0.002); }

inline Buf sine (double f, double dur, double fs, float amp = 0.3f)
{
    Buf x (samples (dur, fs));
    for (size_t i = 0; i < x.size(); ++i) x[i] = amp * (float) std::sin (2.0 * kPi * f * (double) i / fs);
    return x;
}

inline Buf noise (double dur, double fs, float amp, uint32_t seed = 1)
{
    Buf x (samples (dur, fs));
    Lcg r; r.s = seed;
    for (float& v : x) v = amp * r.next();
    return x;
}

inline Buf chord (const std::vector<double>& freqs, double dur, double fs, double tau = 4.0)
{
    Buf x;
    for (double f : freqs) mixInto (x, pluck (f, dur, fs, tau), 0);
    normalisePeak (x, 0.5f);
    return x;
}

// notes of `noteDur` seconds that keep ringing under the following ones
inline Buf arpeggio (const std::vector<double>& freqs, double noteDur, double total, double fs, double ring = 2.5)
{
    const size_t len = samples (total, fs);
    Buf x (len, 0.0f);
    size_t off = 0;
    for (size_t i = 0; off < len; ++i, off += samples (noteDur, fs))
        mixInto (x, pluck (freqs[i % freqs.size()], std::min (total - (double) off / fs, 3.0 * ring), fs, ring), off, 1.0f);
    x.resize (len);
    normalisePeak (x, 0.5f);
    return x;
}

inline Buf distorted (const Buf& clean, float drive = 12.0f)
{
    Buf y (clean.size());
    for (size_t i = 0; i < y.size(); ++i) y[i] = 0.4f * std::tanh (drive * clean[i]);
    return y;
}

// detuned saw-ish pad with slow amplitude breathing
inline Buf pad (const std::vector<double>& freqs, double dur, double fs)
{
    Buf x (samples (dur, fs), 0.0f);
    const double det[3] = { 0.9965, 1.0, 1.0042 };
    for (double f0 : freqs)
        for (double d : det)
        {
            const Buf n = harmonicNote (f0 * d, dur, fs, 24, 1.0, 1.0e6, 0.0, 0.8, 1.1 * d);
            mixInto (x, n, 0, 1.0f);
        }
    for (size_t i = 0; i < x.size(); ++i)
        x[i] *= 0.8f + 0.2f * (float) std::sin (2.0 * kPi * 0.17 * (double) i / fs);
    normalisePeak (x, 0.4f);
    return x;
}

// vowel-like: harmonic source with fixed formants and vibrato
inline Buf voice (double f0, double dur, double fs)
{
    Buf x (samples (dur, fs), 0.0f);
    const double fm[3] = { 700.0, 1220.0, 2600.0 }, bw[3] = { 110.0, 150.0, 250.0 }, fa[3] = { 1.0, 0.6, 0.25 };
    double phase = 0.0;
    std::vector<double> ph (60, 0.0);
    for (size_t i = 0; i < x.size(); ++i)
    {
        const double t = (double) i / fs;
        const double f = f0 * std::pow (2.0, 0.5 * std::sin (2.0 * kPi * 5.5 * t) / 12.0);
        phase += 2.0 * kPi * f / fs;
        double s = 0.0;
        for (int n = 1; n <= 50 && n * f < 0.45 * fs; ++n)
        {
            const double fn = n * f;
            double g = 0.02;
            for (int j = 0; j < 3; ++j) g += fa[j] * std::exp (-0.5 * std::pow ((fn - fm[j]) / bw[j], 2.0));
            s += g / (double) n * std::sin ((double) n * phase);
        }
        x[i] = (float) s * std::min (1.0f, (float) (t / 0.05));
    }
    normalisePeak (x, 0.4f);
    return x;
}

// kick + snare pattern
inline Buf drums (double dur, double fs)
{
    Buf x (samples (dur, fs), 0.0f);
    Lcg r;
    const double beat = 0.375;
    for (double t0 = 0.0; t0 < dur; t0 += beat)
    {
        const int idx = (int) std::llround (t0 / beat);
        const size_t off = samples (t0, fs);
        if (idx % 2 == 0)
        {
            double ph = 0.0;
            for (size_t i = 0; i < samples (0.3, fs) && off + i < x.size(); ++i)
            {
                const double t = (double) i / fs, f = 50.0 + 90.0 * std::exp (-t / 0.03);
                ph += 2.0 * kPi * f / fs;
                x[off + i] += 0.8f * (float) (std::sin (ph) * std::exp (-t / 0.1));
            }
        }
        else
            for (size_t i = 0; i < samples (0.25, fs) && off + i < x.size(); ++i)
            {
                const double t = (double) i / fs;
                x[off + i] += (float) (0.5 * r.next() * std::exp (-t / 0.06) + 0.3 * std::sin (2.0 * kPi * 190.0 * t) * std::exp (-t / 0.05));
            }
    }
    return x;
}

inline Buf concat (const Buf& a, const Buf& b) { Buf r = a; r.insert (r.end(), b.begin(), b.end()); return r; }
inline Buf silence (double dur, double fs) { return Buf (samples (dur, fs), 0.0f); }
} // namespace sig
