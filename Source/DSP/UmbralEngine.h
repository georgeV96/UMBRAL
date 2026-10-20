#pragma once

#include <atomic>
#include <cstdint>
#include <vector>

#include "Fft.h"
#include "Noise.h"

// UMBRAL spectral engine: STFT (Hann, 8x overlap) + partial-aware transformation + timbral memory.
// No JUCE dependency. Everything is allocated in prepare(); process() never allocates, locks or does I/O.
namespace umbral
{
struct Params
{
    float memory = 0.5f;     // 0..1  how long earlier sound keeps influencing the transformation
    float erosion = 0.35f;   // 0..1  partial loss / detuning / dissolution into noise
    float drift = 0.3f;      // 0..1  0 = fully static; >0 slow evolution of the spectral fields
    float threshold = 0.3f;  // 0..1  level + persistence needed before the transformation blooms
    float depth = 0.5f;      // 0..1  intensity and spectral reach of the transformation
    float blend = 0.6f;      // 0..1  dry/processed (0 = dry, 1 = processed)
    float outputDb = 0.0f;   // -24..+12 dB
};

// Read-only view of the internal state (UI meters, tests). Written by the audio thread, relaxed atomics.
struct Diagnostics
{
    float levelDb = -120.0f;     // smoothed input level (RMS, dBFS)
    float engagement = 0.0f;     // 0..1 THRESHOLD gate after smoothing and transient protection
    float transient = 0.0f;      // 0..1 transient protection currently active
    float age = 0.0f;            // 0..1 mean persistence of the partials being transformed
    float memory = 0.0f;         // 0..1 how much spectral memory is currently stored
    float transformation = 0.0f; // 0..1 mean power fraction moved out of the coherent partials
    float inharmonicity = 0.0f;  // mean |partial displacement| / frequency
    int partials = 0;
    int numericalFaults = 0;     // frames that came out non-finite and were discarded (must stay 0)
};

class Engine
{
public:
    static constexpr int kOverlap = 8;
    static constexpr float kMinOutDb = -24.0f, kMaxOutDb = 12.0f;

    void prepare (double sampleRate, int maxBlockSize);   // allocates; also a hard reset
    void setParams (const Params&) noexcept;
    void setSeed (uint32_t) noexcept;                      // takes effect at the next reset
    void requestReset() noexcept;                          // thread-safe, click-free (wet fades to dry, state cleared, wet fades back)
    void restart() noexcept;                               // audio thread only, immediate: back to the state of a fresh instance
                                                           // (rings emptied, frame grid realigned) - use it when the transport starts
    void process (float* const* channels, int numChannels, int numSamples) noexcept;   // in place, 1 or 2 channels

    int getLatencySamples() const noexcept { return fftSize - 1; }
    int getFftSize() const noexcept { return fftSize; }
    Diagnostics getDiagnostics() const noexcept;

private:
    void hardReset() noexcept;
    void clearState() noexcept;
    void processFrame() noexcept;

    // ---- configuration ------------------------------------------------------------------------------------
    double fs = 48000.0;
    int fftSize = 0, mask = 0, hop = 0, bins = 0;   // bins = N/2 + 1
    float binHz = 0.0f, hopSeconds = 0.0f;
    float olaGain = 1.0f;
    Fft fft;
    Params target {}, smoothed {};
    uint32_t seed = 1, pendingSeed = 1;

    // ---- streaming ----------------------------------------------------------------------------------------
    std::vector<float> inL, inR, olaL, olaR, window;
    int writePos = 0, hopCount = 0, activeCh = 1;

    // ---- per-frame scratch (all sized in prepare) ----------------------------------------------------------
    std::vector<float> re, im;
    std::vector<float> xlr, xli, xrr, xri;     // analysis spectra
    std::vector<float> ylr, yli, yrr, yri;     // coherent output (identity bins + shifted partials)
    std::vector<float> nmL, nmR, nsL, nsR;     // dissolved-noise magnitudes (raw / spread)
    std::vector<float> pw, mag, ageSm, bandGain, specGain, tmpA, tmpB;
    std::vector<float> plr, pli, prr, pri;     // previous-frame analysis spectra (phase-coherence test)
    std::vector<float> log2Hz, bandPos;        // log2 of bin centre frequency; fractional band position
    std::vector<float> cosTab, sinTab;         // phase table for the dissolved-noise components
    std::vector<uint8_t> regionMask, isPeak, wasPeak, peakHold;
    std::vector<double> thetaCur, thetaPrev;
    struct Partial { int p, lo, hi, shift; float freq, lambda, deltaHz, memGain; };
    std::vector<Partial> partials;
    static constexpr int kBands = 28;
    std::vector<float> bandCur, bandMem;

    // ---- timbral state -------------------------------------------------------------------------------------
    std::vector<float> memShape;   // energy-weighted, leaky average of the normalised power spectrum
    std::vector<float> age;        // per-bin persistence 0..1
    static constexpr int kHistory = 10;   // hops (~53 ms at N=2048, 8x overlap)
    std::vector<float> levelHist;
    int histPos = 0;
    float levelS = -120.0f, engage = 0.0f, duck = 0.0f, fluxS = 0.0f;
    double clockT = 0.0;           // drift clock (lattice cells)
    XorShift32 rng;
    float sumTransformation = 0.0f, sumInharm = 0.0f, sumAge = 0.0f;

    // ---- output stage ---------------------------------------------------------------------------------------
    float blendS = 0.0f, gainS = 1.0f, compS = 1.0f, wetFade = 1.0f;
    float pD = 0.0f, pW = 0.0f, pDW = 0.0f;
    enum class ResetPhase { none, fadeOut, fadeIn } resetPhase = ResetPhase::none;
    std::atomic<bool> resetRequested { false };

    std::atomic<float> dLevel { -120.0f }, dEngage { 0.0f }, dTransient { 0.0f }, dAge { 0.0f }, dMemory { 0.0f },
        dTransformation { 0.0f }, dInharm { 0.0f };
    std::atomic<int> dPartials { 0 }, faults { 0 };
};
} // namespace umbral
