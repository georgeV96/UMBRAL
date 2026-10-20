// Minimal headless "host": instantiates the real UmbralAudioProcessor and checks the plugin layer end to end -
// parameters reaching the engine, reported latency, programs, state, layouts, editor construction.
#include <cstdio>

#include "../Source/PluginEditor.h"
#include "../Source/PluginProcessor.h"
#include "Harness.h"

static int g_fail = 0, g_pass = 0;
static void check (bool ok, const juce::String& what)
{
    std::printf ("  [%s] %s\n", ok ? "PASS" : "FAIL", what.toRawUTF8());
    ok ? ++g_pass : ++g_fail;
}

static std::vector<float> run (UmbralAudioProcessor& p, const std::vector<float>& in, int block, bool stereo = true)
{
    std::vector<float> out;
    juce::AudioBuffer<float> buf (stereo ? 2 : 1, block);
    juce::MidiBuffer midi;
    for (size_t pos = 0; pos < in.size(); pos += (size_t) block)
    {
        const int n = (int) std::min<size_t> ((size_t) block, in.size() - pos);
        buf.setSize (stereo ? 2 : 1, n, false, false, true);
        for (int c = 0; c < buf.getNumChannels(); ++c)
            buf.copyFrom (c, 0, in.data() + pos, n);
        p.processBlock (buf, midi);
        out.insert (out.end(), buf.getReadPointer (0), buf.getReadPointer (0) + n);
    }
    return out;
}
static double errDb (const std::vector<float>& a, const std::vector<float>& b, size_t from, size_t n)
{
    double e = 0, r = 0;
    for (size_t i = from; i < from + n && i < a.size() && i < b.size(); ++i) { e += (double) (a[i] - b[i]) * (a[i] - b[i]); r += (double) b[i] * b[i]; }
    return 10.0 * std::log10 ((e + 1e-24) / (r + 1e-24));
}
static void setParam (UmbralAudioProcessor& p, const char* id, float plainValue)
{
    auto* prm = p.apvts.getParameter (id);
    prm->setValueNotifyingHost (prm->convertTo0to1 (plainValue));
}

int main()
{
    juce::ScopedJuceInitialiser_GUI init;
    const double fs = 48000.0;
    const int block = 480;
    auto note = sig::pluck (196.0, 6.0, fs, 40.0);
    for (float& v : note) v *= 2.0f;

    std::printf ("\n== Plugin layer\n");
    {
        UmbralAudioProcessor p;
        p.setPlayConfigDetails (2, 2, fs, block);
        p.prepareToPlay (fs, block);
        check (p.getLatencySamples() == 2047, "reported latency " + juce::String (p.getLatencySamples()) + " samples (= FFT size 2048 - 1 at 48 kHz)");
        check (p.getNumPrograms() == (int) umbral::factoryPresets().size() && p.getProgramName (0).isNotEmpty(), juce::String (p.getNumPrograms()) + " factory programs, first: " + p.getProgramName (0));
        check (p.isBusesLayoutSupported (juce::AudioProcessor::BusesLayout { { juce::AudioChannelSet::mono() }, { juce::AudioChannelSet::mono() } }), "mono layout accepted");
        check (p.apvts.getParameter (ParamID::memory) != nullptr && p.apvts.getParameter (ParamID::erosion) != nullptr && p.apvts.getParameter (ParamID::drift) != nullptr
                   && p.apvts.getParameter (ParamID::threshold) != nullptr && p.apvts.getParameter (ParamID::depth) != nullptr && p.apvts.getParameter (ParamID::blend) != nullptr
                   && p.apvts.getParameter (ParamID::output) != nullptr, "all seven controls are host-automatable parameters");
        check (! p.apvts.getParameter (ParamID::seed)->isAutomatable(), "SEED is stored and recalled but not automatable (it restarts the pattern)");
    }
    {   // every control, moved through the real parameter, changes the real output - and the plugin equals the bare engine
        struct C { const char* id; float lo, hi; };
        const C controls[] = { { ParamID::memory, 0, 1 }, { ParamID::erosion, 0, 1 }, { ParamID::drift, 0, 1 }, { ParamID::threshold, 0, 1 },
                               { ParamID::depth, 0, 1 }, { ParamID::blend, 0, 1 }, { ParamID::output, -24.0f, 0.0f } };
        for (const auto& c : controls)
        {
            std::vector<float> o[2];
            for (int k = 0; k < 2; ++k)
            {
                UmbralAudioProcessor p;
                p.setPlayConfigDetails (2, 2, fs, block);
                setParam (p, ParamID::memory, 0.5f); setParam (p, ParamID::erosion, 0.5f); setParam (p, ParamID::drift, 0.5f);
                setParam (p, ParamID::threshold, 0.1f); setParam (p, ParamID::depth, 0.8f); setParam (p, ParamID::blend, 0.8f); setParam (p, ParamID::output, 0.0f);
                setParam (p, c.id, k == 0 ? c.lo : c.hi);
                p.prepareToPlay (fs, block);
                o[k] = run (p, note, block);
            }
            // compare the second half of the note, where the transformation has developed
            const double d = errDb (o[0], o[1], 144000, 48000);
            check (d > -60.0, juce::String (c.id) + ": moving the real host parameter changes the real output (error vs. other setting " + juce::String (d, 1) + " dB; wiring check - audibility is measured by the engine tests)");
        }
        // plugin == engine for preset 5
        UmbralAudioProcessor p;
        p.setPlayConfigDetails (2, 2, fs, block);
        p.setCurrentProgram (5);
        p.prepareToPlay (fs, block);
        const auto viaPlugin = run (p, note, block);
        const auto viaEngine = harness::render (note, &note, umbral::factoryPresets()[5].params);
        std::vector<float> aligned (viaEngine.L);
        std::vector<float> shifted (viaPlugin.begin() + p.getLatencySamples(), viaPlugin.end());
        check (errDb (shifted, aligned, 48000, 200000) < -90.0, "program '" + p.getProgramName (5) + "': plugin output == bare engine output (" + juce::String (errDb (shifted, aligned, 48000, 200000), 1) + " dB)");
    }
    {   // state round trip
        UmbralAudioProcessor a;
        a.setPlayConfigDetails (2, 2, fs, block);
        setParam (a, ParamID::erosion, 0.77f); setParam (a, ParamID::output, -6.5f);
        a.setCurrentProgram (3);
        setParam (a, ParamID::erosion, 0.77f);
        juce::MemoryBlock mb;
        a.getStateInformation (mb);
        UmbralAudioProcessor b;
        b.setStateInformation (mb.getData(), (int) mb.getSize());
        check (std::abs (b.apvts.getRawParameterValue (ParamID::erosion)->load() - 0.77f) < 1e-4f && b.getCurrentProgram() == 3, "state (parameters + current program) survives save/restore");
    }
    {   // mono instance
        UmbralAudioProcessor p;
        p.setPlayConfigDetails (1, 1, fs, block);
        p.prepareToPlay (fs, block);
        const auto out = run (p, note, block, false);
        bool fin = true; for (float v : out) if (! std::isfinite (v)) fin = false;
        check (fin && out.size() == note.size(), "mono processing is finite and length-preserving");
    }
    {   // editor can be built and painted
        UmbralAudioProcessor p;
        p.setPlayConfigDetails (2, 2, fs, block);
        p.prepareToPlay (fs, block);
        std::unique_ptr<juce::AudioProcessorEditor> ed (p.createEditor());
        juce::Image img (juce::Image::ARGB, ed->getWidth(), ed->getHeight(), true);
        juce::Graphics g (img);
        ed->paintEntireComponent (g, false);
        check (ed->getWidth() > 300 && ed->getHeight() > 200, "editor constructs and paints (" + juce::String (ed->getWidth()) + " x " + juce::String (ed->getHeight()) + ")");
    }
    std::printf ("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail;
}
