#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "PluginProcessor.h"

// Seven real controls, a preset menu and read-outs of the engine's own state (nothing here is decorative:
// the three bars are the engagement gate, the stored timbral memory and the mean partial loss).
class UmbralEditor : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    explicit UmbralEditor (UmbralAudioProcessor&);
    ~UmbralEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override;
    void drawBar (juce::Graphics&, juce::Rectangle<int>, const juce::String&, float value, juce::Colour);

    struct Knob
    {
        juce::Slider slider;
        juce::Label label;
        std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> attachment;
    };

    UmbralAudioProcessor& proc;
    std::array<Knob, 7> knobs;
    juce::ComboBox presets;
    juce::Slider seedSlider;
    juce::Label seedLabel;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> seedAttachment;

    float inLevel = 0.0f, outLevel = 0.0f;
    umbral::Diagnostics diag;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (UmbralEditor)
};
