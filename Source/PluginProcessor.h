#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include "DSP/UmbralEngine.h"
#include "Presets.h"

namespace ParamID
{
inline constexpr const char* memory = "memory";
inline constexpr const char* erosion = "erosion";
inline constexpr const char* drift = "drift";
inline constexpr const char* threshold = "threshold";
inline constexpr const char* depth = "depth";
inline constexpr const char* blend = "blend";
inline constexpr const char* output = "output";
inline constexpr const char* seed = "seed";
} // namespace ParamID

class UmbralAudioProcessor : public juce::AudioProcessor
{
public:
    UmbralAudioProcessor();
    ~UmbralAudioProcessor() override = default;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout&) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using juce::AudioProcessor::processBlock;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "Umbral"; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return engine.getFftSize() > 0 ? (double) engine.getLatencySamples() / getSampleRate() : 0.0; }

    int getNumPrograms() override { return (int) umbral::factoryPresets().size(); }
    int getCurrentProgram() override { return currentProgram; }
    void setCurrentProgram (int) override;
    const juce::String getProgramName (int index) override;
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock&) override;
    void setStateInformation (const void*, int) override;

    juce::AudioProcessorValueTreeState apvts;

    // live read-outs for the editor (all thread-safe)
    umbral::Diagnostics getDiagnostics() const noexcept { return engine.getDiagnostics(); }
    float takeInputPeak() noexcept { return inPeak.exchange (0.0f); }
    float takeOutputPeak() noexcept { return outPeak.exchange (0.0f); }
    int getLatency() const noexcept { return engine.getLatencySamples(); }

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();
    void pullParameters (umbral::Params&) const;

    umbral::Engine engine;
    std::atomic<float>* pMemory; std::atomic<float>* pErosion; std::atomic<float>* pDrift; std::atomic<float>* pThreshold;
    std::atomic<float>* pDepth; std::atomic<float>* pBlend; std::atomic<float>* pOutput; std::atomic<float>* pSeed;
    int currentProgram = 0;
    int lastSeed = -1;
    bool wasPlaying = false;
    std::atomic<float> inPeak { 0.0f }, outPeak { 0.0f };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (UmbralAudioProcessor)
};
