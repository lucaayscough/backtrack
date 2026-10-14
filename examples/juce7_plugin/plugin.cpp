#include "backtrack.hpp"

#include <juce_audio_processors/juce_audio_processors.h>

#include <exception>
#include <optional>

namespace {

class Processor final : public juce::AudioProcessor
{
public:
  Processor()
      : AudioProcessor(BusesProperties()
                           .withInput("Input", juce::AudioChannelSet::stereo())
                           .withOutput("Output", juce::AudioChannelSet::stereo()))
  {
  }

  void prepareToPlay(double sample_rate, int) override
  {
    // A Backtrack keeps one format for its lifetime, so recording continues
    // across re-preparation unless the sample rate or channel count changes.
    const auto channels = getTotalNumInputChannels();
    if (_input && juce::exactlyEqual(sample_rate, _sample_rate) &&
        channels == _channels)
      return;

    _sample_rate = sample_rate;
    _channels = channels;
    _input.reset();
    _output.reset();
    try
    {
      _input.emplace("Plugin input", sample_rate, channels);
      _output.emplace("Plugin output", sample_rate, channels);
    }
    catch (const std::exception& error)
    {
      DBG("Backtrack allocation failed: " << error.what());
      _input.reset();
      _output.reset();
    }
  }

  void releaseResources() override {}

  bool isBusesLayoutSupported(const BusesLayout& layouts) const override
  {
    const auto output = layouts.getMainOutputChannelSet();
    return (output == juce::AudioChannelSet::mono() ||
            output == juce::AudioChannelSet::stereo()) &&
           layouts.getMainInputChannelSet() == output;
  }

  void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
  {
    juce::ScopedNoDenormals no_denormals;
    if (_input)
      _input->record(buffer);
    buffer.applyGain(juce::Decibels::decibelsToGain(-6.0f));
    if (_output)
      _output->record(buffer);
  }

  juce::AudioProcessorEditor* createEditor() override;
  bool hasEditor() const override { return true; }

  const juce::String getName() const override { return JucePlugin_Name; }
  bool acceptsMidi() const override { return false; }
  bool producesMidi() const override { return false; }
  bool isMidiEffect() const override { return false; }
  double getTailLengthSeconds() const override { return 0.0; }

  int getNumPrograms() override { return 1; }
  int getCurrentProgram() override { return 0; }
  void setCurrentProgram(int) override {}
  const juce::String getProgramName(int) override { return {}; }
  void changeProgramName(int, const juce::String&) override {}

  void getStateInformation(juce::MemoryBlock&) override {}
  void setStateInformation(const void*, int) override {}

private:
  std::optional<la::Backtrack> _input;
  std::optional<la::Backtrack> _output;
  double _sample_rate = 0.0;
  int _channels = 0;
};

class Editor final : public juce::AudioProcessorEditor
{
public:
  explicit Editor(Processor& owner) : AudioProcessorEditor(owner)
  {
    addAndMakeVisible(_toggle);
    _toggle.onClick = [] { la::Backtrack::toggle(); };
    setSize(320, 120);
    // No matching setEditor(nullptr) in the destructor: Backtrack detaches
    // when the editor is deleted, and clearing it explicitly could detach
    // another plugin instance's editor that registered since.
    la::Backtrack::setEditor(this);
  }

  void paint(juce::Graphics& graphics) override
  {
    graphics.fillAll(juce::Colour(0xff202328));
  }

  void resized() override { _toggle.setBounds(getLocalBounds().reduced(40)); }

private:
  juce::TextButton _toggle {"Toggle Backtrack viewer"};
};

juce::AudioProcessorEditor* Processor::createEditor()
{
  return new Editor(*this);
}

} // namespace

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
  return new Processor;
}
