#include "UmbralEngine.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace umbral
{
namespace
{
constexpr double kTwoPi = 6.283185307179586476925286766559;
constexpr int kPhaseTable = 4096;

inline float clamp01 (float v) noexcept { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }
inline float smoothstep01 (float t) noexcept { t = clamp01 (t); return t * t * (3.0f - 2.0f * t); }
inline float coef (float seconds, float tau) noexcept { return 1.0f - std::exp (-seconds / std::max (tau, 1.0e-4f)); }
inline float flush (float v) noexcept { return std::fabs (v) < 1.0e-20f ? 0.0f : v; }
} // namespace

// ================================================================================================================
// setup
// ================================================================================================================
void Engine::prepare (double sampleRate, int)
{
    fs = std::max (sampleRate, 8000.0);

    // ~40 ms analysis window: fine enough to resolve the low strings (82 Hz harmonics are 3.5 bins apart at 48 kHz)
    // without making the latency worse than it has to be.
    fftSize = 512;
    while ((double) fftSize < fs * 0.04 && fftSize < 16384)
        fftSize <<= 1;
    mask = fftSize - 1;
    hop = fftSize / kOverlap;
    bins = fftSize / 2 + 1;
    binHz = (float) (fs / fftSize);
    hopSeconds = (float) (hop / fs);
    fft.init (fftSize);

    window.assign ((size_t) fftSize, 0.0f);
    double sumSq = 0.0;
    for (int i = 0; i < fftSize; ++i)
    {
        const double w = 0.5 - 0.5 * std::cos (kTwoPi * i / fftSize);
        window[(size_t) i] = (float) w;
        sumSq += w * w;
    }
    // Hann analysis x Hann synthesis: the overlap-added w^2 sums to sumSq / hop. Includes the 1/N of the inverse FFT.
    olaGain = (float) ((double) hop / sumSq / (double) fftSize);

    auto sized = [] (std::vector<float>& v, size_t n) { v.assign (n, 0.0f); };
    sized (inL, (size_t) fftSize);  sized (inR, (size_t) fftSize);
    sized (olaL, (size_t) fftSize); sized (olaR, (size_t) fftSize);
    sized (re, (size_t) fftSize);   sized (im, (size_t) fftSize);
    for (auto* v : { &xlr, &xli, &xrr, &xri, &ylr, &yli, &yrr, &yri, &nmL, &nmR, &nsL, &nsR, &tmpA, &tmpB, &pw, &mag,
                     &plr, &pli, &prr, &pri,
                     &ageSm, &specGain, &log2Hz, &bandPos, &memShape, &age })
        sized (*v, (size_t) bins + 2);
    thetaCur.assign ((size_t) bins + 2, 0.0);
    thetaPrev.assign ((size_t) bins + 2, 0.0);
    regionMask.assign ((size_t) bins + 2, 0);
    isPeak.assign ((size_t) bins + 2, 0);
    wasPeak.assign ((size_t) bins + 2, 0);
    peakHold.assign ((size_t) bins + 2, 0);
    partials.clear();
    partials.reserve ((size_t) bins);
    bandCur.assign (kBands, 0.0f);
    bandMem.assign (kBands, 0.0f);
    bandGain.assign (kBands, 1.0f);
    levelHist.assign (kHistory, -120.0f);

    cosTab.assign (kPhaseTable, 0.0f);
    sinTab.assign (kPhaseTable, 0.0f);
    for (int i = 0; i < kPhaseTable; ++i)
    {
        cosTab[(size_t) i] = (float) std::cos (kTwoPi * i / kPhaseTable);
        sinTab[(size_t) i] = (float) std::sin (kTwoPi * i / kPhaseTable);
    }

    // log-spaced analysis bands for the timbre imprint (80 Hz .. ~16 kHz)
    const double fTop = std::min (16000.0, 0.45 * fs), fLow = 80.0;
    for (int k = 0; k < bins; ++k)
    {
        const double f = std::max (0.5, (double) k) * binHz;
        log2Hz[(size_t) k] = (float) std::log2 (f);
        const double pos = (double) (kBands - 1) * std::log2 (std::max (f, fLow) / fLow) / std::log2 (fTop / fLow);
        bandPos[(size_t) k] = (float) std::clamp (pos, 0.0, (double) (kBands - 1));
    }

    hardReset();
}

void Engine::hardReset() noexcept
{
    for (auto* v : { &inL, &inR, &olaL, &olaR })
        std::fill (v->begin(), v->end(), 0.0f);
    writePos = 0;
    hopCount = 0;
    smoothed = target;
    blendS = target.blend;
    gainS = std::pow (10.0f, target.outputDb / 20.0f);
    compS = 1.0f;
    pD = pW = pDW = 0.0f;
    wetFade = 1.0f;
    resetPhase = ResetPhase::none;
    resetRequested.store (false);
    clearState();
}

void Engine::clearState() noexcept
{
    for (auto* v : { &memShape, &age, &plr, &pli, &prr, &pri })
        std::fill (v->begin(), v->end(), 0.0f);
    std::fill (thetaCur.begin(), thetaCur.end(), 0.0);
    std::fill (thetaPrev.begin(), thetaPrev.end(), 0.0);
    std::fill (isPeak.begin(), isPeak.end(), (uint8_t) 0);
    std::fill (wasPeak.begin(), wasPeak.end(), (uint8_t) 0);
    std::fill (peakHold.begin(), peakHold.end(), (uint8_t) 0);
    levelS = -120.0f;
    std::fill (levelHist.begin(), levelHist.end(), -120.0f);
    histPos = 0;
    engage = duck = fluxS = 0.0f;
    clockT = 0.0;
    seed = pendingSeed;
    rng.s = 2463534242u ^ hash32 (seed);
    if (rng.s == 0)
        rng.s = 1;
    sumTransformation = sumInharm = sumAge = 0.0f;
}

void Engine::setParams (const Params& p) noexcept
{
    target.memory = clamp01 (p.memory);
    target.erosion = clamp01 (p.erosion);
    target.drift = clamp01 (p.drift);
    target.threshold = clamp01 (p.threshold);
    target.depth = clamp01 (p.depth);
    target.blend = clamp01 (p.blend);
    target.outputDb = std::clamp (p.outputDb, kMinOutDb, kMaxOutDb);
}

void Engine::setSeed (uint32_t s) noexcept { pendingSeed = s; }
void Engine::restart() noexcept { hardReset(); }
void Engine::requestReset() noexcept { resetRequested.store (true, std::memory_order_relaxed); }

Diagnostics Engine::getDiagnostics() const noexcept
{
    Diagnostics d;
    d.levelDb = dLevel.load (std::memory_order_relaxed);
    d.engagement = dEngage.load (std::memory_order_relaxed);
    d.transient = dTransient.load (std::memory_order_relaxed);
    d.age = dAge.load (std::memory_order_relaxed);
    d.memory = dMemory.load (std::memory_order_relaxed);
    d.transformation = dTransformation.load (std::memory_order_relaxed);
    d.inharmonicity = dInharm.load (std::memory_order_relaxed);
    d.partials = dPartials.load (std::memory_order_relaxed);
    d.numericalFaults = faults.load (std::memory_order_relaxed);
    return d;
}

// ================================================================================================================
// streaming
// ================================================================================================================
void Engine::process (float* const* ch, int numCh, int numSamples) noexcept
{
    if (fftSize == 0 || numCh < 1 || numSamples <= 0)
        return;
    activeCh = numCh >= 2 ? 2 : 1;

    if (resetRequested.exchange (false, std::memory_order_relaxed) && resetPhase == ResetPhase::none)
        resetPhase = ResetPhase::fadeOut;

    const float aSmooth = coef (1.0f / (float) fs, 0.02f);       // blend / output: 20 ms
    const float aComp = coef (1.0f / (float) fs, 0.15f);         // power trackers: 150 ms
    const float aCompS = coef (1.0f / (float) fs, 0.05f);        // compensation gain: 50 ms
    const float stepOut = 1.0f / (float) (0.006 * fs), stepIn = 1.0f / (float) (0.04 * fs);
    const float tgtGain = std::pow (10.0f, target.outputDb / 20.0f);
    float* const chL = ch[0];
    float* const chR = activeCh == 2 ? ch[1] : nullptr;

    for (int n = 0; n < numSamples; ++n)
    {
        float xl = chL[n], xr = chR ? chR[n] : 0.0f;
        if (!std::isfinite (xl)) xl = 0.0f;
        if (!std::isfinite (xr)) xr = 0.0f;
        xl = std::clamp (xl, -1.0e4f, 1.0e4f);
        xr = std::clamp (xr, -1.0e4f, 1.0e4f);

        inL[(size_t) writePos] = xl;
        inR[(size_t) writePos] = xr;
        writePos = (writePos + 1) & mask;
        if (++hopCount == hop)
        {
            hopCount = 0;
            processFrame();
        }

        // writePos now indexes the oldest sample of the ring: x[n-N+1] (the dry path) and the first sample of the
        // frame just added (the wet path). Both are latency N-1 behind the input.
        const size_t o = (size_t) writePos;
        const float wl = olaL[o], wr = olaR[o];
        olaL[o] = 0.0f;
        olaR[o] = 0.0f;
        const float dl = inL[o], dr = inR[o];

        if (resetPhase == ResetPhase::fadeOut)
        {
            wetFade -= stepOut;
            if (wetFade <= 0.0f)
            {
                wetFade = 0.0f;
                clearState();
                resetPhase = ResetPhase::fadeIn;
            }
        }
        else if (resetPhase == ResetPhase::fadeIn)
        {
            wetFade += stepIn;
            if (wetFade >= 1.0f)
            {
                wetFade = 1.0f;
                resetPhase = ResetPhase::none;
            }
        }

        blendS += aSmooth * (target.blend - blendS);
        gainS += aSmooth * (tgtGain - gainS);
        const float b = blendS * wetFade;

        float ol, orr;
        if (b <= 1.0e-6f)
        {
            ol = dl * gainS;
            orr = dr * gainS;
        }
        else
        {
            // Level compensation: keep the power of the mix at the power-weighted average of dry and wet. Correlated
            // signals add coherently (comp ~ 1); decorrelated ones dip by up to 3 dB at 50 % (comp -> 1.41). The
            // trackers are slow (150 ms) so dynamic differences between dry and wet are left alone.
            pD += aComp * (0.5f * (dl * dl + dr * dr) - pD);
            pW += aComp * (0.5f * (wl * wl + wr * wr) - pW);
            pDW += aComp * (0.5f * (dl * wl + dr * wr) - pDW);
            const float actual = (1.0f - b) * (1.0f - b) * pD + b * b * pW + 2.0f * b * (1.0f - b) * pDW;
            const float want = (1.0f - b) * pD + b * pW;
            const float c = std::clamp (std::sqrt ((want + 1.0e-12f) / (std::max (actual, 0.0f) + 1.0e-12f)), 0.7f, 1.4f);
            compS += aCompS * (c - compS);
            const float g = compS * gainS;
            ol = ((1.0f - b) * dl + b * wl) * g;
            orr = ((1.0f - b) * dr + b * wr) * g;
        }
        pD = flush (pD); pW = flush (pW); pDW = flush (pDW);

        chL[n] = std::isfinite (ol) ? ol : 0.0f;
        if (chR)
            chR[n] = std::isfinite (orr) ? orr : 0.0f;
    }
}

// ================================================================================================================
// one STFT frame
// ================================================================================================================
void Engine::processFrame() noexcept
{
    const int N = fftSize, K = N / 2;
    const float hs = hopSeconds;

    // ---- smooth the spectral controls at frame rate (50 ms) ---------------------------------------------------
    {
        const float a = coef (hs, 0.05f);
        smoothed.memory += a * (target.memory - smoothed.memory);
        smoothed.erosion += a * (target.erosion - smoothed.erosion);
        smoothed.drift += a * (target.drift - smoothed.drift);
        smoothed.threshold += a * (target.threshold - smoothed.threshold);
        smoothed.depth += a * (target.depth - smoothed.depth);
    }
    const float mem = smoothed.memory, ero = smoothed.erosion, dri = smoothed.drift, thr = smoothed.threshold,
                dep = smoothed.depth;

    // ---- analysis: both channels with one complex FFT ----------------------------------------------------------
    for (int i = 0; i < N; ++i)
    {
        const size_t idx = (size_t) ((writePos + i) & mask);
        re[(size_t) i] = inL[idx] * window[(size_t) i];
        im[(size_t) i] = inR[idx] * window[(size_t) i];
    }
    fft.forward (re.data(), im.data());
    xlr[0] = re[0]; xli[0] = 0.0f; xrr[0] = im[0]; xri[0] = 0.0f;
    xlr[(size_t) K] = re[(size_t) K]; xli[(size_t) K] = 0.0f; xrr[(size_t) K] = im[(size_t) K]; xri[(size_t) K] = 0.0f;
    for (int k = 1; k < K; ++k)
    {
        const size_t j = (size_t) (N - k), s = (size_t) k;
        xlr[s] = 0.5f * (re[s] + re[j]);
        xli[s] = 0.5f * (im[s] - im[j]);
        xrr[s] = 0.5f * (im[s] + im[j]);
        xri[s] = -0.5f * (re[s] - re[j]);
    }

    // linked power spectrum (the same plan is applied to both channels, which keeps the stereo image intact)
    const float invCh = 1.0f / (float) activeCh;
    double fullSum = 0.0, halfSum = 0.0;
    float maxP = 0.0f;
    for (int k = 0; k <= K; ++k)
    {
        const size_t s = (size_t) k;
        const float p = (xlr[s] * xlr[s] + xli[s] * xli[s] + xrr[s] * xrr[s] + xri[s] * xri[s]) * invCh;
        pw[s] = p;
        mag[s] = std::sqrt (p);
        maxP = std::max (maxP, p);
        fullSum += (k == 0 || k == K) ? p : 2.0f * p;
        if (k > 0 && k < K)
            halfSum += p;
    }
    // Parseval with the Hann window (sum w^2 = 3N/8): mean-square of the frame
    const double rms2 = fullSum / (0.375 * (double) N * (double) N);
    const float levelDb = 10.0f * std::log10 (std::max ((float) rms2, 1.0e-14f));

    // ---- THRESHOLD: level gate, persistence build-up, transient protection --------------------------------------
    levelS += coef (hs, levelDb > levelS ? 0.02f : 0.2f) * (levelDb - levelS);
    // onset = the frame level jumped well above what it was ~50 ms ago (a rise, not a sustained level)
    const float levelBefore = levelHist[(size_t) histPos];
    levelHist[(size_t) histPos] = levelDb;
    histPos = (histPos + 1) % kHistory;
    const float trans = clamp01 ((levelDb - levelBefore - 3.0f) / 9.0f);
    {   // transient protection: 12 ms attack, release 80 ms .. 530 ms with THRESHOLD (no instantaneous jumps -> no pumping)
        float d = duck * std::exp (-hs / (0.08f + 0.45f * thr));
        if (trans > d)
            d += coef (hs, 0.012f) * (trans - d);
        duck = d;
    }
    const float thrDb = -66.0f + 48.0f * thr;      // -66 dBFS (sensitive) .. -18 dBFS (only strong playing)
    const float gate = smoothstep01 ((levelS - thrDb) / 12.0f + 0.5f);
    engage += coef (hs, gate > engage ? 0.05f : 0.35f) * (gate - engage);
    const float Eg = engage * (1.0f - 0.9f * duck);

    // ---- partial detection ---------------------------------------------------------------------------------------
    // A spectral peak counts as a partial when (1) it drops clearly on both sides and (2) it is a stable sinusoid:
    // the instantaneous frequency measured from the phase advance since the previous frame agrees in the bins
    // p-1, p, p+1 (noise lobes and attack frames do not). This resolves dense low-pitched harmonics (82 Hz is only
    // 3.5 bins apart at 2048/48k) where a "stands out from the average" criterion fails.
    partials.clear();
    std::fill (regionMask.begin(), regionMask.end(), (uint8_t) 0);
    std::fill (isPeak.begin(), isPeak.end(), (uint8_t) 0);
    const float floorP = std::max (maxP * 1.0e-7f, 1.0e-20f);
    const float phasePerBin = (float) (kTwoPi / (double) kOverlap);   // expected phase advance per bin and hop
    auto binFreq = [&] (const std::vector<float>& cr, const std::vector<float>& ci, const std::vector<float>& qr,
                        const std::vector<float>& qi, int k, float& out) noexcept
    {
        const size_t s = (size_t) k;
        const float re0 = cr[s] * qr[s] + ci[s] * qi[s], im0 = ci[s] * qr[s] - cr[s] * qi[s];
        if (re0 * re0 + im0 * im0 < 1.0e-30f)
            return false;
        float d = std::atan2 (im0, re0) - phasePerBin * (float) (k % kOverlap);
        d -= (float) kTwoPi * std::floor (d / (float) kTwoPi + 0.5f);
        out = (float) k + d * (float) kOverlap / (float) kTwoPi;
        return true;
    };
    for (int k = 3; k <= K - 4; ++k)
    {
        const size_t s = (size_t) k;
        if (!(mag[s] > mag[s - 1] && mag[s] >= mag[s + 1] && pw[s] > floorP))
            continue;
        const bool hyst = wasPeak[s] || wasPeak[s - 1] || wasPeak[s + 1];
        const float vl = std::min (std::min (pw[s - 1], pw[s - 2]), std::min (pw[s - 3], pw[s - 4]));
        const float vr = std::min (std::min (pw[s + 1], pw[s + 2]), std::min (pw[s + 3], pw[s + 4]));
        if (pw[s] < (hyst ? 2.0f : 3.0f) * std::max (vl, vr))
            continue;
        const float tol = hyst ? 0.5f : 0.35f;
        bool stable = false;
        for (int c = 0; c < activeCh && !stable; ++c)
        {
            const auto& cr = c == 0 ? xlr : xrr; const auto& ci = c == 0 ? xli : xri;
            const auto& qr = c == 0 ? plr : prr; const auto& qi = c == 0 ? pli : pri;
            float f0 = 0, fm = 0, fp = 0;
            stable = binFreq (cr, ci, qr, qi, k, f0) && binFreq (cr, ci, qr, qi, k - 1, fm) && binFreq (cr, ci, qr, qi, k + 1, fp)
                     && std::fabs (fm - f0) < tol && std::fabs (fp - f0) < tol;
        }
        if (!stable)
            continue;
        Partial pt {};
        pt.memGain = 1.0f;
        pt.p = k;
        pt.lo = std::max (1, k - 3);
        pt.hi = std::min (K - 1, k + 3);
        // parabolic interpolation on the log magnitude
        const float la = std::log (mag[s - 1] + 1.0e-20f), lb = std::log (mag[s] + 1.0e-20f), lc = std::log (mag[s + 1] + 1.0e-20f);
        const float den = la - 2.0f * lb + lc;
        const float delta = std::fabs (den) > 1.0e-9f ? std::clamp (0.5f * (la - lc) / den, -0.5f, 0.5f) : 0.0f;
        pt.freq = ((float) k + delta) * binHz;
        partials.push_back (pt);
        isPeak[s] = 1;
    }
    // remember this frame's spectra for the next frame's phase comparison
    plr = xlr; pli = xli; prr = xrr; pri = xri;
    for (size_t i = 0; i + 1 < partials.size(); ++i)
    {
        Partial& a = partials[i];
        Partial& b = partials[i + 1];
        if (b.p - a.p <= 6)
        {
            int v = a.p + 1;
            for (int k = a.p + 1; k < b.p; ++k)
                if (mag[(size_t) k] < mag[(size_t) v])
                    v = k;
            a.hi = std::min (a.hi, v);
            b.lo = std::max (b.lo, v + 1);
        }
    }
    for (const auto& pt : partials)
        for (int k = pt.lo; k <= pt.hi; ++k)
            regionMask[(size_t) k] = 1;

    // ---- MEMORY: spectral-shape memory, per-bin persistence ("age"), drift clock --------------------------------
    const float tauForget = 0.04f * std::pow (250.0f, mem);          // 0.04 s .. 10 s
    const float tauUp = 0.08f + 4.5f * thr * thr;                    // persistence needed before a partial blooms
    const float aUp = coef (hs, tauUp), aDnPartial = coef (hs, std::max (tauForget, 0.12f));   // 120 ms floor: detection flicker must not reset a bloom
    const float rho = std::exp (-hs / tauForget);
    const float invTot = halfSum > 1.0e-30 ? (float) (1.0 / halfSum) : 0.0f;

    // shape of the memory *before* this frame: used for the timbre imprint and for the spectral-change measure
    float conf = 0.0f;
    std::fill (bandCur.begin(), bandCur.end(), 0.0f);
    std::fill (bandMem.begin(), bandMem.end(), 0.0f);
    for (int k = 1; k < K; ++k)
        conf += memShape[(size_t) k];
    float flux = 0.0f;
    for (int k = 1; k < K; ++k)
    {
        const size_t s = (size_t) k;
        const float cur = pw[s] * invTot;
        const int bnd = std::clamp ((int) (bandPos[s] + 0.5f), 0, kBands - 1);
        bandCur[(size_t) bnd] += cur;
        bandMem[(size_t) bnd] += memShape[s];
        if (conf > 1.0e-4f)
            flux += std::fabs (cur - memShape[s] / conf);
    }
    flux = (conf > 1.0e-4f && gate > 0.05f) ? clamp01 (0.5f * flux) : 0.0f;
    fluxS += coef (hs, 0.1f) * (flux - fluxS);

    // persistence smeared across frequency (from the previous frame's map): a new note near an aged region inherits part of its state
    {
        const int R = 16;
        double run = 0.0;
        int cnt = 0;
        for (int k = 1; k <= std::min (R + 1, K - 1); ++k, ++cnt)   // window of bin 1 is [1-R, 1+R] clipped to the spectrum
            run += (double) age[(size_t) k];
        for (int k = 1; k < K; ++k)
        {
            ageSm[(size_t) k] = cnt > 0 ? std::max (0.0f, (float) (run / cnt)) : 0.0f;
            const int add = k + R + 1, sub = k - R;
            if (add < K) { run += (double) age[(size_t) add]; ++cnt; }
            if (sub >= 1) { run -= (double) age[(size_t) sub]; --cnt; }
        }
    }


    // A partial that is born here (no partial within +-1 bin during the last ~130 ms) starts from what MEMORY lets it inherit,
    // not from the age the bin had under the previous note: a new note begins natural unless the memory says otherwise.
    for (const auto& pt : partials)
    {
        const size_t ps = (size_t) pt.p;
        if (peakHold[ps] || peakHold[ps - 1] || peakHold[ps + 1])
            continue;
        const float inh = 0.7f * mem * ageSm[ps];
        for (int k = pt.lo; k <= pt.hi; ++k)
            age[(size_t) k] = std::min (age[(size_t) k], inh);
    }
    for (int k = 1; k < K; ++k)
        if (peakHold[(size_t) k])
            --peakHold[(size_t) k];
    for (const auto& pt : partials)
        peakHold[(size_t) pt.p] = 24;

    for (int k = 1; k < K; ++k)
    {
        const size_t s = (size_t) k;
        memShape[s] = flush (rho * memShape[s] + (1.0f - rho) * gate * pw[s] * invTot);
        const float tgt = regionMask[s] ? gate : 0.0f;   // only bins that belong to a sustained partial accumulate persistence
        age[s] = flush (age[s] + (tgt > age[s] ? aUp : aDnPartial) * (tgt - age[s]));
    }
    // ---- DRIFT: deterministic, signal-modulated clock driving smooth lattice-noise fields ----------------------
    const bool driftOn = dri > 1.0e-4f;
    const float wD = driftOn ? std::pow (dri, 0.8f) : 0.0f;
    if (driftOn)
        clockT += (double) ((0.015f + 0.9f * std::pow (dri, 1.5f)) * (1.0f + 1.5f * fluxS)) * (double) hs;
    const float clockF = (float) clockT;
    const uint32_t sd = seed;
    auto field = [&] (float cell, uint32_t key) noexcept
    {
        const float base = valueNoise2 (cell, 0.0f, sd + key);
        if (wD <= 0.0f)
            return base;
        return base + wD * (valueNoise2 (cell, clockF, sd + key) - base);
    };
    const float betaMod = 1.0f + wD * 0.7f * valueNoise2 (3.7f, clockF * 0.7f, sd ^ 0xB37Au);
    const float fcShift = 0.9f * wD * valueNoise2 (9.1f, clockF * 0.5f, sd ^ 0xFC11u);

    // ---- DEPTH / EROSION amounts ----------------------------------------------------------------------------------
    const float sD = std::pow (dep, 1.3f);
    const float log2fc = std::log2 (12000.0f * std::pow (80.0f / 12000.0f, dep)) + fcShift;
    const float eLoss = std::pow (ero, 1.1f), eStretch = std::pow (ero, 1.3f), eWob = std::pow (ero, 1.2f);
    const float cellScale = 1.0f / 2.5f;   // lattice cell = 2.5 bins (~60 Hz at 48 kHz): neighbouring partials differ
    auto omega = [&] (int k) noexcept { return smoothstep01 (0.5f + 0.5f * (log2Hz[(size_t) k] - log2fc)); };
    auto ageEff = [&] (int k, float inherit) noexcept
    {
        const size_t s = (size_t) k;
        return std::max ({ age[s - 1], age[s], age[s + 1], inherit * ageSm[s] });
    };
    const float inheritW = 0.7f * mem;
    auto pow075 = [] (float a) noexcept { const float r = std::sqrt (std::max (a, 0.0f)); return r * std::sqrt (r); };

    // ---- build the output spectrum ---------------------------------------------------------------------------------
    // 1. bins outside any partial: identity, plus a slow, signal-dependent erosion of the spectral floor
    const bool colour = Eg * sD * eLoss > 1.0e-3f;
    for (int k = 1; k < K; ++k)
    {
        const size_t s = (size_t) k;
        nmL[s] = nmR[s] = 0.0f;
        if (regionMask[s])
        {
            ylr[s] = yli[s] = yrr[s] = yri[s] = 0.0f;
            continue;
        }
        float g = 1.0f;
        if (colour)
        {
            const float q = Eg * sD * omega (k) * pow075 (ageSm[s]);
            if (q > 1.0e-4f)
            {
                const float h = 0.25f + 0.75f * clamp01 (0.5f + 0.8f * field ((float) k * cellScale, 1u));
                g = 1.0f - 0.6f * std::min (1.0f, q * eLoss * 1.2f * h);
            }
        }
        ylr[s] = xlr[s] * g; yli[s] = xli[s] * g; yrr[s] = xrr[s] * g; yri[s] = xri[s] * g;
    }
    ylr[0] = xlr[0]; yli[0] = 0.0f; yrr[0] = xrr[0]; yri[0] = 0.0f;
    ylr[(size_t) K] = xlr[(size_t) K]; yli[(size_t) K] = 0.0f; yrr[(size_t) K] = xrr[(size_t) K]; yri[(size_t) K] = 0.0f;
    nmL[0] = nmR[0] = nmL[(size_t) K] = nmR[(size_t) K] = 0.0f;

    // 2. partials: displace (rigid lobe copy + phase rotation), erode, dissolve into noise
    // 2a. MEMORY fingerprint: partials that coincide with remembered partials are favoured, the others are held back
    //     (energy-neutral over the partials). A held note converges to its own memory, so this only acts at changes.
    const float muPartial = 0.85f * mem * std::sqrt (sD) * clamp01 (conf) * Eg;
    float fingerprintNorm = 1.0f;
    if (muPartial > 1.0e-3f && conf > 1.0e-4f && !partials.empty())
    {
        double e0 = 0.0, e1 = 0.0;
        for (auto& pt : partials)
        {
            const size_t ps = (size_t) pt.p;
            const float remembered = std::max ({ memShape[ps - 1], memShape[ps], memShape[ps + 1] }) / conf;
            const float current = pw[ps] * invTot;
            const float ratio = std::clamp ((remembered + 1.0e-4f) / (current + 1.0e-4f), 0.05f, 20.0f);
            pt.memGain = std::clamp (std::pow (ratio, 0.5f * muPartial), 0.35f, 2.0f);
            e0 += (double) current;
            e1 += (double) current * pt.memGain * pt.memGain;
        }
        fingerprintNorm = e1 > 1.0e-30 ? (float) std::sqrt (e0 / e1) : 1.0f;
    }
    float sumLam = 0.0f, sumDev = 0.0f, sumAgeP = 0.0f;
    const float betaCoef = 0.010f * eStretch * betaMod;
    const float maxShiftHz = 40.0f * binHz;
    for (auto& pt : partials)
    {
        const float cell = (pt.freq / binHz) * cellScale;
        const float Aj = ageEff (pt.p, inheritW);
        const float q = Eg * sD * omega (pt.p) * pow075 (Aj);
        const float h = 0.25f + 0.75f * clamp01 (0.5f + 0.8f * field (cell, 1u));
        pt.lambda = std::clamp (q * eLoss * 1.5f * h, 0.0f, 0.97f);

        const float f = pt.freq, fk = f * (1.0f / 1000.0f);
        const float wob = field (cell, 2u);
        const float ratio = betaCoef * fk * fk + 0.010f * eWob * wob * std::min(2.0f, std::sqrt (f * (1.0f / 500.0f)));
        pt.deltaHz = std::clamp (f * ratio * q, -std::min (0.12f * f, maxShiftHz), std::min (0.12f * f, maxShiftHz));
        pt.shift = (int) std::lround (pt.deltaHz / binHz);

        // rotation accumulator, carried over from the matching partial of the previous frame
        int m = -1;
        const size_t ps = (size_t) pt.p;
        if (wasPeak[ps]) m = pt.p;
        else if (wasPeak[ps - 1]) m = pt.p - 1;
        else if (wasPeak[ps + 1]) m = pt.p + 1;
        double th = (m >= 0 ? thetaPrev[(size_t) m] : 0.0) + kTwoPi * (double) pt.deltaHz * (double) hs;
        th -= kTwoPi * std::floor (th / kTwoPi + 0.5);
        thetaCur[ps] = th;

        const float gc = std::sqrt (1.0f - pt.lambda) * pt.memGain * fingerprintNorm;
        const float c = gc * (float) std::cos (th), sn = gc * (float) std::sin (th);
        for (int k = pt.lo; k <= pt.hi; ++k)
        {
            const int kk = k + pt.shift;
            if (kk < 1 || kk >= K)
                continue;
            const size_t s = (size_t) k, d = (size_t) kk;
            ylr[d] += xlr[s] * c - xli[s] * sn;
            yli[d] += xlr[s] * sn + xli[s] * c;
            yrr[d] += xrr[s] * c - xri[s] * sn;
            yri[d] += xrr[s] * sn + xri[s] * c;
            nmL[d] += pt.lambda * pt.memGain * fingerprintNorm * std::sqrt (xlr[s] * xlr[s] + xli[s] * xli[s]);
            nmR[d] += pt.lambda * pt.memGain * fingerprintNorm * std::sqrt (xrr[s] * xrr[s] + xri[s] * xri[s]);
        }
        sumLam += pt.lambda;
        sumDev += std::fabs (pt.deltaHz) / std::max (f, 1.0f);
        sumAgeP += Aj;
    }
    const int nP = (int) partials.size();

    // 3. the lost coherent energy re-appears as noise bands around the (displaced) partials; EROSION x DEPTH widens them
    if (nP > 0)
    {
        double pre = 0.0;
        for (int k = 1; k < K; ++k)
            pre += (double) nmL[(size_t) k] * nmL[(size_t) k] + (double) nmR[(size_t) k] * nmR[(size_t) k];
        if (pre > 1.0e-30)
        {
            const float sp = 3.0f * eLoss * sD;
            const int whole = (int) sp;
            const float frac = sp - (float) whole;
            auto pass = [&] (std::vector<float>& v) noexcept
            {
                float prev = 0.0f;
                for (int k = 1; k < K; ++k)
                {
                    const float cur = v[(size_t) k];
                    tmpA[(size_t) k] = 0.25f * prev + 0.5f * cur + 0.25f * v[(size_t) k + 1];
                    prev = cur;
                }
                for (int k = 1; k < K; ++k)
                    v[(size_t) k] = tmpA[(size_t) k];
            };
            nsL = nmL;
            nsR = nmR;
            nsL[(size_t) K] = nsR[(size_t) K] = 0.0f;
            for (int i = 0; i < whole; ++i) { pass (nsL); pass (nsR); }
            if (frac > 1.0e-3f)
            {
                tmpB = nsL; pass (nsL); for (int k = 1; k < K; ++k) nsL[(size_t) k] = tmpB[(size_t) k] + frac * (nsL[(size_t) k] - tmpB[(size_t) k]);
                tmpB = nsR; pass (nsR); for (int k = 1; k < K; ++k) nsR[(size_t) k] = tmpB[(size_t) k] + frac * (nsR[(size_t) k] - tmpB[(size_t) k]);
            }
            double post = 0.0;
            for (int k = 1; k < K; ++k)
                post += (double) nsL[(size_t) k] * nsL[(size_t) k] + (double) nsR[(size_t) k] * nsR[(size_t) k];
            const float scale = post > 1.0e-30 ? (float) std::sqrt (pre / post) : 0.0f;
            for (int k = 1; k < K; ++k)
            {
                const uint32_t r = rng.next() >> 20;   // 12 bits -> phase table index
                const float cp = cosTab[r], sp2 = sinTab[r];
                const float al = nsL[(size_t) k] * scale, ar = nsR[(size_t) k] * scale;
                ylr[(size_t) k] += al * cp; yli[(size_t) k] += al * sp2;
                yrr[(size_t) k] += ar * cp; yri[(size_t) k] += ar * sp2;
            }
        }
    }

    // 4. MEMORY imprint: the new sound is nudged towards the recent spectral balance (energy-neutral, +-9 dB max)
    const float mu = 0.85f * mem * std::sqrt (sD) * clamp01 (conf) * Eg;
    if (mu > 1.0e-3f && conf > 1.0e-4f)
    {
        const float eps = 1.0e-4f / (float) kBands;
        float curTot = 0.0f, wTot = 0.0f;
        for (int b = 0; b < kBands; ++b)
        {
            const float ratio = std::clamp ((bandMem[(size_t) b] / conf + eps) / (bandCur[(size_t) b] + eps), 1.0e-4f, 1.0e4f);
            const float g = std::clamp (std::pow (ratio, 0.5f * mu), 0.355f, 2.8f);
            bandGain[(size_t) b] = g;
            curTot += bandCur[(size_t) b];
            wTot += bandCur[(size_t) b] * g * g;
        }
        const float norm = wTot > 1.0e-12f ? std::sqrt (curTot / wTot) : 1.0f;
        for (int k = 1; k < K; ++k)
        {
            const float pos = bandPos[(size_t) k];
            const int b0 = std::min ((int) pos, kBands - 2);
            const float t = pos - (float) b0;
            const float g = norm * (bandGain[(size_t) b0] + t * (bandGain[(size_t) b0 + 1] - bandGain[(size_t) b0]));
            const size_t s = (size_t) k;
            ylr[s] *= g; yli[s] *= g; yrr[s] *= g; yri[s] *= g;
        }
    }

    // 4b. DRIFT: slow wander of the spectral distribution (a smooth, signal-gated gain field over log-frequency, energy-neutral)
    if (wD > 0.0f && Eg * sD > 1.0e-3f)
    {
        const float amp = 1.25f * wD * std::sqrt (sD) * Eg;
        double cur = 0.0, wtd = 0.0;
        for (int k = 1; k < K; ++k)
        {
            const size_t s = (size_t) k;
            const float g = std::exp2 (amp * valueNoise2 (log2Hz[s] * 1.5f, clockF * 0.6f, sd ^ 0x7117u));
            specGain[s] = g;
            cur += (double) pw[s];
            wtd += (double) pw[s] * g * g;
        }
        const float norm = wtd > 1.0e-30 ? (float) std::sqrt (cur / wtd) : 1.0f;
        for (int k = 1; k < K; ++k)
        {
            const size_t s = (size_t) k;
            const float g = specGain[s] * norm;
            ylr[s] *= g; yli[s] *= g; yrr[s] *= g; yri[s] *= g;
        }
    }

    // ---- synthesis -------------------------------------------------------------------------------------------------
    re[0] = ylr[0]; im[0] = yrr[0];
    re[(size_t) K] = ylr[(size_t) K]; im[(size_t) K] = yrr[(size_t) K];
    for (int k = 1; k < K; ++k)
    {
        const size_t s = (size_t) k, j = (size_t) (N - k);
        re[s] = ylr[s] - yri[s];
        im[s] = yli[s] + yrr[s];
        re[j] = ylr[s] + yri[s];
        im[j] = -yli[s] + yrr[s];
    }
    fft.inverse (re.data(), im.data());
    {   // last line of defence: a non-finite frame must never poison the state (the fault counter is checked by the tests)
        float chk = 0.0f;
        for (int i = 0; i < N; ++i)
            chk += re[(size_t) i] * 0.0f + im[(size_t) i] * 0.0f;
        if (!std::isfinite (chk))
        {
            clearState();
            faults.fetch_add (1, std::memory_order_relaxed);
            std::fill (re.begin(), re.end(), 0.0f);
            std::fill (im.begin(), im.end(), 0.0f);
        }
    }
    for (int i = 0; i < N; ++i)
    {
        const size_t idx = (size_t) ((writePos + i) & mask);
        const float w = window[(size_t) i] * olaGain;
        olaL[idx] += re[(size_t) i] * w;
        olaR[idx] += im[(size_t) i] * w;
    }

    // carry the partial-tracking state to the next frame
    std::swap (thetaCur, thetaPrev);
    std::fill (thetaCur.begin(), thetaCur.end(), 0.0);
    std::swap (isPeak, wasPeak);

    dLevel.store (levelS, std::memory_order_relaxed);
    dEngage.store (Eg, std::memory_order_relaxed);
    dTransient.store (duck, std::memory_order_relaxed);
    dMemory.store (clamp01 (conf), std::memory_order_relaxed);
    dAge.store (nP ? sumAgeP / (float) nP : 0.0f, std::memory_order_relaxed);
    dTransformation.store (nP ? sumLam / (float) nP : 0.0f, std::memory_order_relaxed);
    dInharm.store (nP ? sumDev / (float) nP : 0.0f, std::memory_order_relaxed);
    dPartials.store (nP, std::memory_order_relaxed);
}
} // namespace umbral
