# Backtrack

A single-header JUCE debugging tool (`include/backtrack.hpp`) that records audio
streams in memory and shows them in a floating waveform viewer you can scroll,
zoom and inspect down to individual samples.

Requires C++20 and JUCE 7.0.12+ or 8.

## Usage

```cmake
include(FetchContent)

# After JUCE is available, so Backtrack reuses it.
FetchContent_Declare(
  backtrack
  GIT_REPOSITORY https://github.com/lucaayscough/backtrack.git
  GIT_TAG master
  GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(backtrack)

target_link_libraries(MyPlugin PRIVATE la::backtrack)
```

```cpp
#include "backtrack.hpp"

// Processor
std::optional<la::Backtrack> backtrack;

void prepareToPlay(double sampleRate, int) override
{
    backtrack.emplace("Output", sampleRate, getTotalNumOutputChannels());
}

void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
{
    // ... your processing ...
    backtrack->record(buffer);
}

// Editor constructor
la::Backtrack::setEditor(this);
```

The viewer opens once an editor is set and at least one `Backtrack` exists.
Each `Backtrack` is one named buffer; create as many as you like and pick one
from the viewer's menu.

## API

```cpp
la::Backtrack(std::string name, double sampleRate, int channelCount);

void record(const juce::AudioBuffer<float>& buffer, int startSample = 0, int numSamples = -1) noexcept;
void record(std::span<const float> mono) noexcept;
void record(std::span<const std::span<const float>> channels) noexcept;

static void setEditor(juce::Component* editor); // message thread
static void show();
static void hide();
static void toggle();
```

- `record()` can be called from the audio thread; it holds a short lock while
  appending. Blocks with the wrong channel count or invalid ranges are dropped
  and counted rather than throwing.
- Recording is append-only until the `Backtrack` is destroyed. Don't destroy it
  while another thread is calling `record()`.
- One hour per channel is reserved up front; longer recordings keep growing.
  Define `LA_BACKTRACK_RESERVE_SECONDS` (consistently across the target) to
  change that.
- No need to call `setEditor(nullptr)`: Backtrack detaches when the editor is
  deleted. Calling it from one plugin instance's editor would detach another's.

## Building and testing

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

- **Demo app:** `la_backtrack_demo`. Turn it off with `-DLA_BACKTRACK_BUILD_DEMO=OFF`.
- **ThreadSanitizer:** add `-DLA_BACKTRACK_TSAN=ON` to also build and run the
  smoke test under TSan.
- **JUCE 7:** `examples/juce7_plugin` is a minimal plugin on JUCE 7.0.12, and
  configuring it also runs the tests against JUCE 7:
  `cmake -S examples/juce7_plugin -B build-juce7`. It targets macOS 11, because
  JUCE 7 doesn't build against macOS 15+ SDKs otherwise.
