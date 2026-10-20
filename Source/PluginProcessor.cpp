#include "PluginProcessor.h"

#include "PluginEditor.h"

using APVTS = juce::AudioProcessorValueTreeState;

APVTS::ParameterLayout UmbralAudioProcessor::createLayout()
{
    using juce::NormalisableRange;
    const umbral::Params d;   // defaults live in one place
    auto percent = [] (const char* id, const char* name, float def)
    {
        return std::make_unique<juce::AudioParameterFloat> (
            juce::ParameterID { id, 1 }, name, NormalisableRange<float> (0.0f, 1.0f, 0.0f), def,
            juce::AudioParameterFloatAttributes()
                .withStringFromValueFunction ([] (float v, int) { return juce::String (juce::roundToInt (v * 100.0f)) + " %"; })
                .withValueFromStringFunction ([] (const juce::String& t) { return t.getFloatValue() / 100.0f; }));
    };

    std::vector<std::unique_ptr<juce::RangedAudioParameter>> p;
    p.push_back (percent (ParamID::memory, "Memory", d.memory));
    p.push_back (percent (ParamID::erosion, "Erosion", d.erosion));
    p.push_back (percent (ParamID::drift, "Drift", d.drift));
    p.push_back (percent (ParamID::threshold, "Threshold", d.threshold));
    p.push_back (percent (ParamID::depth, "Depth", d.depth));
    p.push_back (percent (ParamID::blend, "Blend", d.blend));
    p.push_back (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { ParamID::output, 1 }, "Output",
        NormalisableRange<float> (umbral::Engine::kMinOutDb, umbral::Engine::kMaxOutDb, 0.01f), d.outputDb,
        juce::AudioParameterFloatAttributes().withLabel ("dB")));
    // The seed picks which (reproducible) drift pattern is used; it is applied through a click-free reset.
    p.push_back (std::make_unique<juce::AudioParameterInt> (
        juce::ParameterID { ParamID::seed, 1 }, "Seed", 1, 99, 1, juce::AudioParameterIntAttributes().withAutomatable (false)));
    return { p.begin(), p.end() };
}

UmbralAudioProcessor::UmbralAudioProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput ("Input", juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "UMBRAL", createLayout())
{
    pMemory = apvts.getRawParameterValue (ParamID::memory);
    pErosion = apvts.getRawParameterValue (ParamID::erosion);
    pDrift = apvts.getRawParameterValue (ParamID::drift);
    pThreshold = apvts.getRawParameterValue (ParamID::threshold);
    pDepth = apvts.getRawParameterValue (ParamID::depth);
    pBlend = apvts.getRawParameterValue (ParamID::blend);
    pOutput = apvts.getRawParameterValue (ParamID::output);
    pSeed = apvts.getRawParameterValue (ParamID::seed);
}

bool UmbralAudioProcessor::isBusesLayoutSupported (const BusesLayout& l) const
{
    const auto in = l.getMainInputChannelSet(), out = l.getMainOutputChannelSet();
    return in == out && (out == juce::AudioChannelSet::mono() || out == juce::AudioChannelSet::stereo());
}

void UmbralAudioProcessor::pullParameters (umbral::Params& p) const
{
    p.memory = pMemory->load();
    p.erosion = pErosion->load();
    p.drift = pDrift->load();
    p.threshold = pThreshold->load();
    p.depth = pDepth->load();
    p.blend = pBlend->load();
    p.outputDb = pOutput->load();
}

void UmbralAudioProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    umbral::Params p;
    pullParameters (p);
    engine.setParams (p);
    lastSeed = juce::roundToInt (pSeed->load());
    engine.setSeed ((uint32_t) lastSeed);
    engine.prepare (sampleRate, samplesPerBlock);          // allocates here, never in processBlock
    setLatencySamples (engine.getLatencySamples());        // the STFT adds N-1 samples; the host compensates
    wasPlaying = false;
}

void UmbralAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;
    const int n = buffer.getNumSamples(), nCh = juce::jmin (buffer.getNumChannels(), 2);
    for (int c = getTotalNumInputChannels(); c < getTotalNumOutputChannels(); ++c)
        buffer.clear (c, 0, n);
    if (nCh < 1 || n < 1)
        return;

    // Every playback of the same material evolves the same way: the drift clock and the timbral memory restart with the transport.
    if (auto* ph = getPlayHead())
        if (auto pos = ph->getPosition())
        {
            const bool playing = pos->getIsPlaying();
            if (playing && ! wasPlaying)
                engine.requestReset();
            wasPlaying = playing;
        }

    const int seed = juce::roundToInt (pSeed->load());
    if (seed != lastSeed)
    {
        lastSeed = seed;
        engine.setSeed ((uint32_t) seed);
        engine.requestReset();
    }

    umbral::Params p;
    pullParameters (p);
    engine.setParams (p);

    float pk = 0.0f;
    for (int c = 0; c < nCh; ++c)
        pk = juce::jmax (pk, buffer.getMagnitude (c, 0, n));
    inPeak.store (juce::jmax (inPeak.load (std::memory_order_relaxed), pk), std::memory_order_relaxed);

    engine.process (buffer.getArrayOfWritePointers(), nCh, n);

    pk = 0.0f;
    for (int c = 0; c < nCh; ++c)
        pk = juce::jmax (pk, buffer.getMagnitude (c, 0, n));
    outPeak.store (juce::jmax (outPeak.load (std::memory_order_relaxed), pk), std::memory_order_relaxed);
}

void UmbralAudioProcessor::setCurrentProgram (int index)
{
    const auto& list = umbral::factoryPresets();
    if (index < 0 || index >= (int) list.size())
        return;
    currentProgram = index;
    const auto& q = list[(size_t) index].params;
    auto set = [this] (const char* id, float v) { if (auto* prm = apvts.getParameter (id)) prm->setValueNotifyingHost (prm->convertTo0to1 (v)); };
    set (ParamID::memory, q.memory);
    set (ParamID::erosion, q.erosion);
    set (ParamID::drift, q.drift);
    set (ParamID::threshold, q.threshold);
    set (ParamID::depth, q.depth);
    set (ParamID::blend, q.blend);
    set (ParamID::output, q.outputDb);
}

const juce::String UmbralAudioProcessor::getProgramName (int index)
{
    const auto& list = umbral::factoryPresets();
    return index >= 0 && index < (int) list.size() ? juce::String (list[(size_t) index].name) : juce::String();
}

void UmbralAudioProcessor::getStateInformation (juce::MemoryBlock& dest)
{
    auto state = apvts.copyState();
    state.setProperty ("program", currentProgram, nullptr);
    if (auto xml = state.createXml())
        copyXmlToBinary (*xml, dest);
}

void UmbralAudioProcessor::setStateInformation (const void* data, int size)
{
    if (auto xml = getXmlFromBinary (data, size))
        if (xml->hasTagName (apvts.state.getType()))
        {
            auto tree = juce::ValueTree::fromXml (*xml);
            currentProgram = juce::jlimit (0, juce::jmax (0, getNumPrograms() - 1), (int) tree.getProperty ("program", 0));
            apvts.replaceState (tree);
        }
}

juce::AudioProcessorEditor* UmbralAudioProcessor::createEditor() { return new UmbralEditor (*this); }

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new UmbralAudioProcessor(); }
