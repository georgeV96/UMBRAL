#pragma once

// Measurement helpers for the tests: windowed spectra, partial tracking, band levels.
#include <algorithm>
#include <cmath>
#include <vector>

#include "DSP/Fft.h"

namespace ana
{
using Buf = std::vector<float>;

inline double rms (const Buf& x, size_t a, size_t n)
{
    double s = 0.0;
    n = std::min (n, x.size() > a ? x.size() - a : 0);
    for (size_t i = 0; i < n; ++i) s += (double) x[a + i] * x[a + i];
    return n ? std::sqrt (s / (double) n) : 0.0;
}
inline double db (double v) { return 20.0 * std::log10 (std::max (v, 1.0e-12)); }

// Magnitude spectrum of a Hann-windowed segment, zero padded to `fftSize`.
inline std::vector<double> spectrum (const Buf& x, size_t start, size_t len, int fftSize = 32768)
{
    umbral::Fft fft;
    fft.init (fftSize);
    std::vector<float> re ((size_t) fftSize, 0.0f), im ((size_t) fftSize, 0.0f);
    len = std::min ({ len, (size_t) fftSize, x.size() > start ? x.size() - start : 0 });
    for (size_t i = 0; i < len; ++i)
        re[i] = x[start + i] * (float) (0.5 - 0.5 * std::cos (2.0 * 3.14159265358979 * (double) i / (double) len));
    fft.forward (re.data(), im.data());
    std::vector<double> m ((size_t) fftSize / 2 + 1);
    for (size_t k = 0; k < m.size(); ++k) m[k] = std::hypot ((double) re[k], (double) im[k]);
    return m;
}

// Frequency of the strongest spectral peak within +-`tol` (ratio) of `expected`, parabolic interpolation.
inline double peakFreq (const Buf& x, size_t start, size_t len, double expected, double fs, double tol = 0.04, int fftSize = 32768)
{
    const auto m = spectrum (x, start, len, fftSize);
    const double bw = fs / fftSize;
    const int lo = std::max (2, (int) std::floor (expected * (1.0 - tol) / bw)), hi = std::min ((int) m.size() - 3, (int) std::ceil (expected * (1.0 + tol) / bw));
    int best = lo;
    for (int k = lo; k <= hi; ++k) if (m[(size_t) k] > m[(size_t) best]) best = k;
    const double a = std::log (m[(size_t) best - 1] + 1e-20), b = std::log (m[(size_t) best] + 1e-20), c = std::log (m[(size_t) best + 1] + 1e-20);
    const double den = a - 2 * b + c;
    const double d = std::fabs (den) > 1e-12 ? 0.5 * (a - c) / den : 0.0;
    return ((double) best + d) * bw;
}
inline double peakLevelDb (const Buf& x, size_t start, size_t len, double f, double fs, double tol = 0.03, int fftSize = 32768)
{
    const auto m = spectrum (x, start, len, fftSize);
    const double bw = fs / fftSize;
    double best = 0.0;
    for (int k = (int) (f * (1 - tol) / bw); k <= (int) (f * (1 + tol) / bw); ++k) best = std::max (best, m[(size_t) k]);
    return db (best);
}

inline double cents (double f, double ref) { return 1200.0 * std::log2 (f / ref); }

// Energy (dB) of the segment between fLo and fHi.
inline double bandDb (const Buf& x, size_t start, size_t len, double fLo, double fHi, double fs, int fftSize = 32768)
{
    const auto m = spectrum (x, start, len, fftSize);
    const double bw = fs / fftSize;
    double e = 0.0;
    for (int k = std::max (1, (int) (fLo / bw)); k <= std::min ((int) m.size() - 1, (int) (fHi / bw)); ++k) e += m[(size_t) k] * m[(size_t) k];
    return 10.0 * std::log10 (std::max (e, 1e-24));
}

// RMS difference (dB) of the band levels of two segments (1/3-octave by default): a rough "how different do they sound".
// Each band is floored 50 dB below the loudest band so that empty bands between sparse partials do not dominate.
inline double bandDistance (const Buf& a, const Buf& b, size_t start, size_t len, double fs, double fLo = 80.0, double fHi = 12000.0, double ratio = 1.26)
{
    std::vector<double> da, dbb;
    for (double f = fLo; f * ratio < fHi * 1.0001; f *= ratio)
    {
        da.push_back (bandDb (a, start, len, f, f * ratio, fs));
        dbb.push_back (bandDb (b, start, len, f, f * ratio, fs));
    }
    double ma = -1e9, mb = -1e9;
    for (double v : da) ma = std::max (ma, v);
    for (double v : dbb) mb = std::max (mb, v);
    const double fl = std::max (ma, mb) - 50.0;
    double acc = 0.0;
    for (size_t i = 0; i < da.size(); ++i) { const double d = std::max (da[i], fl) - std::max (dbb[i], fl); acc += d * d; }
    return std::sqrt (acc / std::max<size_t> (1, da.size()));
}

// Relative error (dB) of x against a reference over [start, start+len).
inline double errorDb (const Buf& x, const Buf& ref, size_t start, size_t len)
{
    double e = 0.0, r = 0.0;
    len = std::min ({ len, x.size() > start ? x.size() - start : 0, ref.size() > start ? ref.size() - start : 0 });
    for (size_t i = 0; i < len; ++i) { const double d = (double) x[start + i] - ref[start + i]; e += d * d; r += (double) ref[start + i] * ref[start + i]; }
    return 10.0 * std::log10 ((e + 1e-24) / (r + 1e-24));
}
} // namespace ana
