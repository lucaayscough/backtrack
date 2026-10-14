#include "backtrack.hpp"

#include <juce_audio_utils/juce_audio_utils.h>

#include <array>
#include <atomic>
#include <cmath>
#include <exception>
#include <optional>
#include <span>

namespace {

class MainComponent final : public juce::AudioAppComponent
{
public:
  MainComponent()
  {
    setSize(720, 240);
    addAndMakeVisible(_pause);
    addAndMakeVisible(_viewer);
    addAndMakeVisible(_description);

    _pause.onClick = [this] {
      const auto paused = !_paused.load(std::memory_order_relaxed);
      _paused.store(paused, std::memory_order_relaxed);
      _pause.setButtonText(paused ? "Resume capture" : "Pause capture");
    };
    _viewer.onClick = [] { la::Backtrack::toggle(); };
    _description.setText(
        "Generated stereo audio is recorded into two duplicate-named buffers "
        "and one mono buffer. Pausing stops record() calls without changing "
        "Backtrack state.",
        juce::dontSendNotification);
    _description.setJustificationType(juce::Justification::centred);

    la::Backtrack::setEditor(this);
    setAudioChannels(0, 2);
  }

  ~MainComponent() override
  {
    shutdownAudio();
    la::Backtrack::setEditor(nullptr);
  }

  void prepareToPlay(int, double sample_rate) override
  {
    _phase = 0.0;
    // Keep recording across device restarts; a Backtrack has one sample rate
    // for its lifetime, so only a rate change starts new buffers.
    if (_stereo && juce::exactlyEqual(sample_rate, _device_sample_rate))
      return;

    _device_sample_rate = sample_rate;
    try
    {
      _stereo.emplace("Generated", sample_rate, 2);
      _duplicate.emplace("Generated", sample_rate, 2);
      _mono.emplace("Left channel", sample_rate, 1);
    }
    catch (const std::exception& error)
    {
      DBG("Backtrack allocation failed: " << error.what());
      _stereo.reset();
      _duplicate.reset();
      _mono.reset();
    }
  }

  void releaseResources() override {}

  void getNextAudioBlock(const juce::AudioSourceChannelInfo& info) override
  {
    auto& buffer = *info.buffer;
    const auto increment = juce::MathConstants<double>::twoPi * 220.0 /
                           std::max(1.0, _device_sample_rate);
    for (int sample = 0; sample < info.numSamples; ++sample)
    {
      const auto left = float(std::sin(_phase) * 0.65);
      const auto right = float(std::sin(_phase * 1.503) * 0.45);
      buffer.setSample(0, info.startSample + sample, left);
      buffer.setSample(1, info.startSample + sample, right);
      _phase += increment;
      if (_phase >= juce::MathConstants<double>::twoPi)
        _phase -= juce::MathConstants<double>::twoPi;
    }

    if (_paused.load(std::memory_order_relaxed))
      return;

    if (!_stereo || !_duplicate || !_mono)
      return;

    _stereo->record(buffer, info.startSample, info.numSamples);
    const std::array<std::span<const float>, 2> planar {
        std::span<const float>(buffer.getReadPointer(0, info.startSample),
                               std::size_t(info.numSamples)),
        std::span<const float>(buffer.getReadPointer(1, info.startSample),
                               std::size_t(info.numSamples))};
    _duplicate->record(planar);
    _mono->record(planar[0]);
  }

  void resized() override
  {
    auto bounds = getLocalBounds().reduced(24);
    _description.setBounds(bounds.removeFromTop(100));
    auto buttons = bounds.removeFromTop(40).withSizeKeepingCentre(330, 36);
    _pause.setBounds(buttons.removeFromLeft(155));
    buttons.removeFromLeft(20);
    _viewer.setBounds(buttons.removeFromLeft(155));
  }

private:
  std::optional<la::Backtrack> _stereo;
  std::optional<la::Backtrack> _duplicate;
  std::optional<la::Backtrack> _mono;
  juce::TextButton _pause {"Pause capture"};
  juce::TextButton _viewer {"Toggle viewer"};
  juce::Label _description;
  std::atomic<bool> _paused {false};
  double _device_sample_rate = 0.0;
  double _phase = 0.0;
};

class BacktrackDemoApplication final : public juce::JUCEApplication
{
public:
  const juce::String getApplicationName() override { return "Backtrack Demo"; }
  const juce::String getApplicationVersion() override { return "0.1.0"; }
  bool moreThanOneInstanceAllowed() override { return true; }

  void initialise(const juce::String&) override
  {
    _window.emplace();
  }

  void shutdown() override { _window.reset(); }
  void systemRequestedQuit() override { quit(); }

private:
  class MainWindow final : public juce::DocumentWindow
  {
  public:
    MainWindow()
        : DocumentWindow("Backtrack Demo", juce::Colour(0xff202328),
                         DocumentWindow::allButtons)
    {
      setUsingNativeTitleBar(true);
      setContentNonOwned(&_content, true);
      centreWithSize(getWidth(), getHeight());
      setResizable(true, false);
      setVisible(true);
    }

    void closeButtonPressed() override
    {
      juce::JUCEApplication::getInstance()->systemRequestedQuit();
    }

  private:
    // Destroyed before the window base; the window tracks it with a SafePointer.
    MainComponent _content;
  };

  std::optional<MainWindow> _window;
};

} // namespace

START_JUCE_APPLICATION(BacktrackDemoApplication)
