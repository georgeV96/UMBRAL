#include "PluginEditor.h"

namespace
{
const juce::Colour kBg (0xff0d0d10), kPanel (0xff17171c), kInk (0xffd9d4cc), kDim (0xff7d7a76), kAccent (0xffc7503a), kCool (0xff4f8f98);
constexpr int kW = 780, kH = 430;
} // namespace

UmbralEditor::UmbralEditor (UmbralAudioProcessor& p) : AudioProcessorEditor (&p), proc (p)
{
    setSize (kW, kH);

    struct Def { const char* id; const char* name; };
    static const Def defs[7] = { { ParamID::memory, "MEMORY" }, { ParamID::erosion, "EROSION" }, { ParamID::drift, "DRIFT" },
                                 { ParamID::threshold, "THRESHOLD" }, { ParamID::depth, "DEPTH" }, { ParamID::blend, "BLEND" },
                                 { ParamID::output, "OUTPUT" } };
    for (size_t i = 0; i < knobs.size(); ++i)
    {
        auto& k = knobs[i];
        k.slider.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
        k.slider.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 80, 18);
        k.slider.setColour (juce::Slider::rotarySliderFillColourId, i < 5 ? kAccent : kCool);
        k.slider.setColour (juce::Slider::rotarySliderOutlineColourId, kPanel.brighter (0.2f));
        k.slider.setColour (juce::Slider::thumbColourId, kInk);
        k.slider.setColour (juce::Slider::textBoxTextColourId, kInk);
        k.slider.setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
        k.label.setText (defs[i].name, juce::dontSendNotification);
        k.label.setJustificationType (juce::Justification::centred);
        k.label.setColour (juce::Label::textColourId, kInk);
        k.label.setFont (juce::FontOptions (13.0f, juce::Font::bold));
        addAndMakeVisible (k.slider);
        addAndMakeVisible (k.label);
        k.attachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (proc.apvts, defs[i].id, k.slider);
    }

    const auto& list = umbral::factoryPresets();
    for (size_t i = 0; i < list.size(); ++i)
        presets.addItem (list[i].name, (int) i + 1);
    presets.setSelectedId (proc.getCurrentProgram() + 1, juce::dontSendNotification);
    presets.onChange = [this] { proc.setCurrentProgram (presets.getSelectedId() - 1); };
    presets.setColour (juce::ComboBox::backgroundColourId, kPanel);
    presets.setColour (juce::ComboBox::textColourId, kInk);
    presets.setColour (juce::ComboBox::outlineColourId, kDim);
    addAndMakeVisible (presets);

    seedSlider.setSliderStyle (juce::Slider::IncDecButtons);
    seedSlider.setTextBoxStyle (juce::Slider::TextBoxLeft, false, 36, 20);
    seedSlider.setColour (juce::Slider::textBoxTextColourId, kInk);
    seedSlider.setColour (juce::Slider::textBoxOutlineColourId, kDim);
    seedLabel.setText ("SEED", juce::dontSendNotification);
    seedLabel.setColour (juce::Label::textColourId, kDim);
    seedLabel.setFont (juce::FontOptions (11.0f));
    addAndMakeVisible (seedSlider);
    addAndMakeVisible (seedLabel);
    seedAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (proc.apvts, ParamID::seed, seedSlider);

    startTimerHz (30);
}

UmbralEditor::~UmbralEditor() { stopTimer(); }

void UmbralEditor::timerCallback()
{
    const float dIn = proc.takeInputPeak(), dOut = proc.takeOutputPeak();
    inLevel = juce::jmax (dIn, inLevel * 0.85f);
    outLevel = juce::jmax (dOut, outLevel * 0.85f);
    diag = proc.getDiagnostics();
    if (presets.getSelectedId() != proc.getCurrentProgram() + 1)
        presets.setSelectedId (proc.getCurrentProgram() + 1, juce::dontSendNotification);
    repaint();
}

void UmbralEditor::drawBar (juce::Graphics& g, juce::Rectangle<int> r, const juce::String& name, float v, juce::Colour c)
{
    g.setColour (kPanel);
    g.fillRect (r);
    g.setColour (c);
    g.fillRect (r.withWidth (juce::roundToInt ((float) r.getWidth() * juce::jlimit (0.0f, 1.0f, v))));
    g.setColour (kDim);
    g.setFont (juce::FontOptions (10.0f));
    g.drawText (name, r.translated (0, -14).withHeight (13), juce::Justification::left);
}

void UmbralEditor::paint (juce::Graphics& g)
{
    g.fillAll (kBg);
    g.setColour (kInk);
    g.setFont (juce::FontOptions (30.0f, juce::Font::bold));
    g.drawText ("UMBRAL", 24, 14, 260, 36, juce::Justification::left);
    g.setColour (kDim);
    g.setFont (juce::FontOptions (11.0f));
    g.drawText ("evolving spectral transformation  -  Lazarus Noise", 26, 48, 400, 16, juce::Justification::left);

    // live engine state
    const int bx = 24, by = 360, bw = 150, bh = 8, gap = 175;
    drawBar (g, { bx, by, bw, bh }, "ENGAGEMENT", diag.engagement, kAccent);
    drawBar (g, { bx + gap, by, bw, bh }, "TIMBRAL MEMORY", diag.memory, kCool);
    drawBar (g, { bx + 2 * gap, by, bw, bh }, "PARTIAL EROSION", diag.transformation, kAccent);
    drawBar (g, { bx + 3 * gap, by, bw, bh }, "TRANSIENT GUARD", diag.transient, kDim);

    auto meter = [&] (int x, float v, const char* t)
    {
        const float db = juce::Decibels::gainToDecibels (v, -60.0f);
        drawBar (g, { x, 395, 110, 6 }, t, (db + 60.0f) / 60.0f, kInk);
    };
    meter (24, inLevel, "IN");
    meter (150, outLevel, "OUT");

    g.setColour (kDim);
    g.setFont (juce::FontOptions (10.0f));
    const double fs = proc.getSampleRate();
    g.drawText (juce::String::formatted ("latency %d samples (%.1f ms)   partials tracked: %d", proc.getLatency(),
                                         fs > 0 ? 1000.0 * proc.getLatency() / fs : 0.0, diag.partials),
                kW - 330, 388, 306, 14, juce::Justification::right);
}

void UmbralEditor::resized()
{
    presets.setBounds (kW - 24 - 220, 22, 220, 28);
    seedLabel.setBounds (kW - 24 - 220, 56, 40, 20);
    seedSlider.setBounds (kW - 24 - 175, 56, 110, 20);

    const int top = 100, w = (kW - 48) / 7, h = 220;
    for (size_t i = 0; i < knobs.size(); ++i)
    {
        auto r = juce::Rectangle<int> (24 + (int) i * w, top, w, h);
        knobs[i].label.setBounds (r.removeFromTop (20));
        knobs[i].slider.setBounds (r.reduced (4));
    }
}
