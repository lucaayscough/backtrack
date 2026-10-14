# JUCE Backtrack Utility

## Summary

This repository provides a C++20, single-header JUCE debugging utility in
`include/backtrack.hpp`. Each
`la::Backtrack` instance captures one named, append-only, multichannel audio
stream and registers itself with a shared floating waveform viewer.

The viewer opens automatically once both an editor and at least one Backtrack
instance exist. It supports live scrollback, buffer selection, stacked channel
lanes, navigation to the recording start, horizontal zoom, and exact sample
inspection.

## Public API

```cpp
namespace la {

using f32 = float;

class Backtrack final
{
public:
  static void setEditor(juce::Component* editor);
  static void show();
  static void hide();
  static void toggle();

  Backtrack(std::string name, double sampleRate, int channelCount);

  Backtrack(const Backtrack&) = delete;
  Backtrack& operator=(const Backtrack&) = delete;
  Backtrack(Backtrack&&) = delete;
  Backtrack& operator=(Backtrack&&) = delete;

  ~Backtrack();

  void record(const juce::AudioBuffer<f32>& buffer,
              int startSample = 0,
              int numSamples = -1) noexcept;

  void record(std::span<const f32> mono) noexcept;
  void record(std::span<const std::span<const f32>> channels) noexcept;
};

} // namespace la
```

- `setEditor()` is called on the JUCE message thread and stores a
  `juce::Component::SafePointer`, not an unsafe raw lifetime dependency.
- Construction requires a non-empty name, positive finite sample rate, and a
  positive channel count.
- Each channel owns a `std::vector<f32>` and reserves one hour of samples.
  Recording past the reserve grows storage geometrically, so appends stay
  amortized constant time.
- Recording remains append-only until destruction. There is no `start()`,
  `stop()`, or `clear()`; inspection begins naturally when the host stops
  supplying blocks.
- Invalid ranges, mismatched channel counts, unequal span lengths, or allocation
  failures drop the whole block, increment an error counter, and never throw
  through the audio callback.
- Callers must ensure destruction does not race with `record()`.

## Implementation and Viewer Behavior

- Keep the shared registry, editor pointer, window, and naming counters in an
  inline function-local singleton so the header is ODR-safe.
- Register and unregister instances automatically. Duplicate labels are stable
  and assigned in creation order: `Input`, `Input (2)`, `Input (3)`.
- Protect each instance with a small mutex. Recording appends under that lock;
  the UI copies only summaries or the currently visible exact-sample range,
  keeping lock duration bounded.
- Maintain an amortized-linear hierarchy of minimum/maximum waveform buckets,
  beginning at 64 samples per bucket, alongside full samples.
  When more than one sample maps to a pixel, painting draws one min/max column
  per pixel: from the bucket hierarchy at 64+ samples per pixel, and reduced
  from raw samples below that. Below two samples per pixel it draws the exact
  waveform, adding sample points at four or more pixels per sample.
  Columns always span at least one pixel, so flat signals remain visible.
- Display channels as vertically stacked lanes with a shared time axis and zero
  lines.
- Auto-follow the newest audio while recording. Manual dragging, scrollbar use,
  or zooming disengages follow mode; a `Live` control restores it.
- Provide mouse-wheel zoom centered on the cursor, drag/scroll navigation,
  `Fit All`, `Live`, and `Go to Start` controls.
- At sample-level zoom, draw individual points and show channel, sample index,
  time, and value on hover.
- The top menu selects the active Backtrack buffer. The status area shows
  duration, sample rate, channels, dropped blocks, retained bytes, reserved
  bytes, and aggregate Backtrack retained bytes as a percentage of physical
  RAM.
- Label memory reporting as `Backtrack memory estimate`: normal below 10% of
  physical RAM, amber from 10%, and red from 25%. If RAM size is unavailable,
  show byte counts without a percentage.
- Use one non-modal resizable `juce::DocumentWindow`, initially centered relative
  to the editor. Closing it (or calling `hide()`) hides it, and buffers being
  created or destroyed will not reopen it; only `show()` or `toggle()` does.
  Destroy or hide it safely when the editor disappears.
- Marshal registry-driven GUI changes onto the JUCE message thread and repaint
  on a roughly 30 Hz timer that only polls while the viewer is on screen.

## Build and Demonstration

- The root CMake project uses `FetchContent` with JUCE pinned to tag `8.0.10`,
  CMake 3.22+, and C++20. When added with `add_subdirectory()` to a project
  that already provides JUCE targets, it reuses them, and tests and the demo
  are off by default.
- `la_backtrack` is an `INTERFACE` target linking the required JUCE audio and GUI
  modules.
- The JUCE desktop demo under `test/`:
  - Calls `Backtrack::setEditor(this)`.
  - Creates several mono and stereo Backtrack instances, including duplicate
    names.
  - Generates or routes live audio through each `record()` overload.
  - Demonstrates automatic viewer opening, continued capture, and stopped-host
    inspection.
- A console test target covers storage and non-visual behavior and is registered
  with CTest.

Configure and verify the project with:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

The demo target is `la_backtrack_demo`. Set `LA_BACKTRACK_BUILD_DEMO=OFF` to
exclude it. Tests use a short reserve interval; consumers reserve one hour per
channel by default and may consistently define `LA_BACKTRACK_RESERVE_SECONDS`
to another positive value for all translation units in a target.

ThreadSanitizer coverage is opt-in, since it builds a second copy of the
smoke test (GUI tests are skipped there):

```sh
cmake -S . -B build-tsan -DCMAKE_BUILD_TYPE=Debug -DLA_BACKTRACK_TSAN=ON
cmake --build build-tsan --parallel
ctest --test-dir build-tsan --output-on-failure
```

## Test Plan

- Verify all record overloads preserve sample values, channel ordering, ranges,
  and cumulative length.
- Verify invalid blocks are discarded atomically and update the dropped-block
  count.
- Verify allocation failure handling leaves existing audio intact.
- Verify one-hour per-channel reservation and retained/reserved byte
  calculations.
- Verify minimum/maximum summaries across block boundaries and the transition to
  exact-sample rendering.
- Verify duplicate naming remains stable through instance creation and
  destruction.
- Exercise concurrent UI snapshots and recording under ThreadSanitizer where
  available.
- Verify editor deletion, instance deletion, window hiding/reopening, and
  application shutdown produce no dangling access.
- Manually validate live following, disengaging follow mode, return-to-start
  navigation, zoom anchoring, stacked channels, hover values, and memory-warning
  colors.

## Assumptions

- This is an in-process debugging tool, so brief audio-thread locking, large
  virtual reservations, and occasional allocation or page-fault costs are
  acceptable.
- A Backtrack instance has one producer format for its lifetime; sample rate and
  channel count never change.
- All GUI ownership calls occur in a JUCE application with a functioning message
  manager.
- The utility stores audio only in memory and performs no file export or session
  persistence.
