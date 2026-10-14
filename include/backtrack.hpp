#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_gui_extra/juce_gui_extra.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#ifndef LA_BACKTRACK_RESERVE_SECONDS
#define LA_BACKTRACK_RESERVE_SECONDS 3600
#endif

static_assert(LA_BACKTRACK_RESERVE_SECONDS >= 1,
              "LA_BACKTRACK_RESERVE_SECONDS must be a positive whole number");

namespace la {

using f32 = float;

namespace detail {

constexpr std::size_t base_bucket_samples = 64;
constexpr std::size_t summary_level_count = 24;

constexpr std::size_t bucket_samples_at(std::size_t level) noexcept
{
  return base_bucket_samples << level;
}

struct Peak
{
  f32 minimum = 0.0f;
  f32 maximum = 0.0f;
};

inline Peak merge(Peak a, Peak b) noexcept
{
  return {std::min(a.minimum, b.minimum), std::max(a.maximum, b.maximum)};
}

struct SummaryLevel
{
  std::vector<Peak> buckets;
  Peak partial;
  std::size_t partial_count = 0;
};

struct ChannelStorage
{
  std::vector<f32> samples;
  std::array<SummaryLevel, summary_level_count> summaries;
};

struct CaptureMetadata
{
  std::uint64_t id = 0;
  std::string display_name;
  double sample_rate = 0.0;
  int channel_count = 0;
  std::size_t sample_count = 0;
  std::size_t dropped_blocks = 0;
  std::size_t retained_bytes = 0;
  std::size_t reserved_bytes = 0;
};

struct WaveformSnapshot
{
  CaptureMetadata metadata;
  std::size_t first_sample = 0;
  std::size_t bucket_samples = 1;
  // One min/max per bucket and channel. With one-sample buckets each entry is
  // an exact sample, so minimum == maximum.
  std::vector<std::vector<Peak>> peaks;

  bool exact() const noexcept { return bucket_samples == 1; }
};

// Test hook: when non-negative, the append that brings it to zero throws.
inline std::atomic<int>& allocation_fail_countdown() noexcept
{
  static std::atomic<int> value {-1};
  return value;
}

inline void allocation_checkpoint()
{
  auto& countdown = allocation_fail_countdown();
  if (countdown.load(std::memory_order_relaxed) >= 0 &&
      countdown.fetch_sub(1, std::memory_order_relaxed) == 0)
    throw std::bad_alloc();
}

template <typename T>
void reserve_for_append(std::vector<T>& values, std::size_t additional)
{
  if (additional > values.max_size() - values.size())
    throw std::length_error("Backtrack recording is too large");
  const auto required = values.size() + additional;
  if (required <= values.capacity())
    return;
  const auto doubled = values.capacity() > values.max_size() / 2
                           ? values.max_size()
                           : values.capacity() * 2;
  values.reserve(std::max(required, doubled));
}

inline void append_peak(ChannelStorage& channel, Peak peak)
{
  for (std::size_t level_index = 0; level_index < summary_level_count;
       ++level_index)
  {
    auto& level = channel.summaries[level_index];
    level.partial = level.partial_count == 0 ? peak : merge(level.partial, peak);
    const std::size_t target = level_index == 0 ? base_bucket_samples : 2;
    if (++level.partial_count != target)
      return;
    peak = level.partial;
    level.buckets.push_back(peak);
    level.partial_count = 0;
  }
}

struct CaptureState
{
  CaptureState(std::string capture_name, double capture_sample_rate,
               int channel_count)
      : name(std::move(capture_name)), sample_rate(capture_sample_rate),
        channels(std::size_t(channel_count))
  {
    const auto reserve = static_cast<long double>(sample_rate) *
                         static_cast<long double>(LA_BACKTRACK_RESERVE_SECONDS);
    if (reserve > static_cast<long double>(std::numeric_limits<std::size_t>::max()))
      throw std::length_error("Backtrack reserve size is too large");
    const auto reserve_samples = static_cast<std::size_t>(std::ceil(reserve));

    // Matches the per-block bound in append(), so recording inside the reserve
    // never allocates.
    for (auto& channel : channels)
    {
      channel.samples.reserve(reserve_samples);
      for (std::size_t level = 0; level < summary_level_count; ++level)
        channel.summaries[level].buckets.reserve(
            reserve_samples / bucket_samples_at(level) + 1);
    }
  }

  void drop_block() noexcept
  {
    std::scoped_lock lock(mutex);
    ++dropped_blocks;
  }

  // `channel_at(index)` returns a pointer to `count` samples for that channel.
  template <typename ChannelAt>
  void append(std::size_t count, ChannelAt channel_at) noexcept
  {
    if (count == 0)
      return;

    try
    {
      std::scoped_lock lock(mutex);
      // Reserve everything first so the appends below cannot throw and a
      // failed block leaves no partial data behind. A level gains at most
      // count / bucket + 1 buckets per block.
      for (auto& channel : channels)
      {
        allocation_checkpoint();
        reserve_for_append(channel.samples, count);
        for (std::size_t level = 0; level < summary_level_count; ++level)
          reserve_for_append(channel.summaries[level].buckets,
                             count / bucket_samples_at(level) + 1);
      }

      for (std::size_t index = 0; index < channels.size(); ++index)
      {
        auto& channel = channels[index];
        const f32* input = channel_at(index);
        channel.samples.insert(channel.samples.end(), input, input + count);
        for (std::size_t sample = 0; sample < count; ++sample)
          append_peak(channel, {input[sample], input[sample]});
      }
    }
    catch (...)
    {
      drop_block();
    }
  }

  CaptureMetadata metadata() const
  {
    std::scoped_lock lock(mutex);
    return metadata_unlocked();
  }

  // Min/max of every bucket overlapping [first_sample, first_sample +
  // sample_count), each over its full extent within the recording. Buckets
  // below the summary size are reduced from raw samples; larger ones come from
  // the coarsest summary level not coarser than requested.
  WaveformSnapshot snapshot(std::size_t first_sample, std::size_t sample_count,
                            std::size_t bucket_samples) const
  {
    std::scoped_lock lock(mutex);
    WaveformSnapshot result;
    result.metadata = metadata_unlocked();
    result.peaks.resize(channels.size());
    const auto total = result.metadata.sample_count;
    const auto end = first_sample >= total
                         ? total
                         : first_sample + std::min(sample_count, total - first_sample);

    if (bucket_samples < base_bucket_samples)
    {
      const auto size = std::max<std::size_t>(1, bucket_samples);
      result.bucket_samples = size;
      result.first_sample = first_sample / size * size;
      for (std::size_t index = 0; index < channels.size(); ++index)
      {
        const auto& samples = channels[index].samples;
        for (auto start = result.first_sample; start < end; start += size)
        {
          const auto [minimum, maximum] = std::minmax_element(
              samples.begin() + static_cast<std::ptrdiff_t>(start),
              samples.begin() + static_cast<std::ptrdiff_t>(std::min(total, start + size)));
          result.peaks[index].push_back({*minimum, *maximum});
        }
      }
      return result;
    }

    std::size_t level_index = 0;
    while (level_index + 1 < summary_level_count &&
           bucket_samples_at(level_index + 1) <= bucket_samples)
      ++level_index;

    const auto size = bucket_samples_at(level_index);
    const auto first_bucket = first_sample / size;
    const auto end_bucket = (end + size - 1) / size;
    result.bucket_samples = size;
    result.first_sample = first_bucket * size;
    if (first_bucket >= end_bucket)
      return result;

    for (std::size_t index = 0; index < channels.size(); ++index)
    {
      const auto& buckets = channels[index].summaries[level_index].buckets;
      const auto complete_end = std::min(end_bucket, buckets.size());
      result.peaks[index].assign(
          buckets.begin() + static_cast<std::ptrdiff_t>(first_bucket),
          buckets.begin() + static_cast<std::ptrdiff_t>(complete_end));
      if (end_bucket > buckets.size())
        result.peaks[index].push_back(
            in_progress_peak(channels[index], level_index));
    }
    return result;
  }

  const std::string name;
  const double sample_rate;
  std::uint64_t id = 0;     // Assigned by Registry::add before publication.
  std::string display_name; // Likewise.
  mutable std::mutex mutex;
  std::vector<ChannelStorage> channels; // Never resized after construction.
  std::size_t dropped_blocks = 0;

private:
  // The unfinished bucket at a level spans the unfinished buckets of every
  // level below it, so its peak is the merge of their partials.
  static Peak in_progress_peak(const ChannelStorage& channel,
                               std::size_t level_index) noexcept
  {
    std::optional<Peak> result;
    for (std::size_t level = 0; level <= level_index; ++level)
      if (const auto& summary = channel.summaries[level]; summary.partial_count != 0)
        result = result ? merge(*result, summary.partial) : summary.partial;
    return result.value_or(Peak {});
  }

  CaptureMetadata metadata_unlocked() const
  {
    CaptureMetadata result;
    result.id = id;
    result.display_name = display_name;
    result.sample_rate = sample_rate;
    result.channel_count = int(channels.size());
    result.sample_count = channels.front().samples.size();
    result.dropped_blocks = dropped_blocks;
    for (const auto& channel : channels)
    {
      result.retained_bytes += channel.samples.size() * sizeof(f32);
      result.reserved_bytes += channel.samples.capacity() * sizeof(f32);
      for (const auto& summary : channel.summaries)
      {
        result.retained_bytes += summary.buckets.size() * sizeof(Peak);
        result.reserved_bytes += summary.buckets.capacity() * sizeof(Peak);
      }
    }
    return result;
  }
};

class Registry
{
public:
  void add(const std::shared_ptr<CaptureState>& state)
  {
    std::scoped_lock lock(_mutex);
    state->id = _next_id++;
    const auto duplicate = ++_name_counts[state->name];
    state->display_name = state->name;
    if (duplicate > 1)
      state->display_name += " (" + std::to_string(duplicate) + ")";
    _captures.push_back(state);
  }

  void remove(const CaptureState* state)
  {
    std::scoped_lock lock(_mutex);
    std::erase_if(_captures,
                  [state](const auto& capture) { return capture.get() == state; });
  }

  std::vector<std::shared_ptr<CaptureState>> captures() const
  {
    std::scoped_lock lock(_mutex);
    return _captures;
  }

  std::shared_ptr<CaptureState> find(std::uint64_t id) const
  {
    std::scoped_lock lock(_mutex);
    for (const auto& capture : _captures)
      if (capture->id == id)
        return capture;
    return {};
  }

private:
  mutable std::mutex _mutex;
  std::uint64_t _next_id = 1;
  std::unordered_map<std::string, std::size_t> _name_counts;
  std::vector<std::shared_ptr<CaptureState>> _captures;
};

// Intentionally leaked so Backtracks with static storage duration can still
// unregister during static destruction.
inline Registry& registry()
{
  static auto* instance = new Registry;
  return *instance;
}

// Defined by the tests; befriended by classes whose internals they inspect.
struct BacktrackTestAccess;

} // namespace detail

class Backtrack final
{
public:
  static void setEditor(juce::Component* editor);
  static void show();
  static void hide();
  static void toggle();

  Backtrack(std::string name, double sample_rate, int channel_count);
  ~Backtrack();

  Backtrack(const Backtrack&) = delete;
  Backtrack& operator=(const Backtrack&) = delete;
  Backtrack(Backtrack&&) = delete;
  Backtrack& operator=(Backtrack&&) = delete;

  void record(const juce::AudioBuffer<f32>& buffer, int start_sample = 0,
              int num_samples = -1) noexcept
  {
    const auto total = buffer.getNumSamples();
    if (std::size_t(buffer.getNumChannels()) != _state->channels.size() ||
        start_sample < 0 || start_sample > total)
      return _state->drop_block();

    const auto count = num_samples == -1 ? total - start_sample : num_samples;
    if (count < 0 || count > total - start_sample)
      return _state->drop_block();

    _state->append(std::size_t(count), [&buffer, start_sample](std::size_t channel) {
      return buffer.getReadPointer(int(channel), start_sample);
    });
  }

  void record(std::span<const f32> mono) noexcept
  {
    record(std::span<const std::span<const f32>>(&mono, 1));
  }

  void record(std::span<const std::span<const f32>> channels) noexcept
  {
    const auto count = channels.empty() ? 0 : channels.front().size();
    if (channels.size() != _state->channels.size() ||
        std::any_of(channels.begin(), channels.end(),
                    [count](auto channel) { return channel.size() != count; }))
      return _state->drop_block();

    _state->append(count, [channels](std::size_t channel) {
      return channels[channel].data();
    });
  }

private:
  friend struct detail::BacktrackTestAccess;
  std::shared_ptr<detail::CaptureState> _state;
};

namespace detail {

inline double clamp_view_start(double start, double total_samples,
                               double visible_samples) noexcept
{
  return std::clamp(start, 0.0, std::max(0.0, total_samples - visible_samples));
}

inline double zoom_anchored_start(double old_start, double old_scale,
                                  double new_scale, double anchor_x,
                                  double total_samples,
                                  double viewport_width) noexcept
{
  const auto anchored_sample = old_start + anchor_x * old_scale;
  return clamp_view_start(
      anchored_sample - anchor_x * new_scale, total_samples,
      viewport_width * new_scale);
}

inline double zoomed_scale(double current_scale, double wheel_delta,
                           double total_samples, double viewport_width) noexcept
{
  // Never let the upper bound fall below the current scale, otherwise a
  // zoom-out gesture on a short recording would snap the view inwards.
  const auto maximum_scale = std::max(
      {1.0, total_samples / viewport_width * 2.0, current_scale});
  return std::clamp(current_scale * std::exp(-wheel_delta * 1.5), 0.05,
                    maximum_scale);
}

// Samples per min/max column; 1 means the exact waveform is drawn.
inline std::size_t peak_bucket_request(double samples_per_pixel) noexcept
{
  return std::max<std::size_t>(1, std::size_t(std::floor(samples_per_pixel)));
}

inline double ruler_step(double seconds_per_100_pixels) noexcept
{
  const auto exponent =
      std::floor(std::log10(std::max(1.0e-9, seconds_per_100_pixels)));
  const auto base = std::pow(10.0, exponent);
  const auto normalised = seconds_per_100_pixels / base;
  const auto multiplier = normalised <= 1.0   ? 1.0
                          : normalised <= 2.0 ? 2.0
                          : normalised <= 5.0 ? 5.0
                                              : 10.0;
  return multiplier * base;
}

inline int ruler_decimals(double step) noexcept
{
  return std::clamp(int(std::ceil(-std::log10(step) - 1.0e-9)), 0, 9);
}

inline std::pair<float, float> peak_column(Peak peak, float centre_y,
                                           float scale_y) noexcept
{
  const auto top = centre_y - peak.maximum * scale_y;
  const auto bottom = centre_y - peak.minimum * scale_y;
  return {top, std::max(bottom, top + 1.0f)};
}

inline int memory_pressure_level(std::uint64_t retained_bytes,
                                 std::uint64_t physical_bytes) noexcept
{
  if (physical_bytes == 0)
    return 0;
  const auto ratio = double(retained_bytes) / double(physical_bytes);
  return ratio >= 0.25 ? 2 : ratio >= 0.10 ? 1 : 0;
}

inline juce::String format_bytes(std::uint64_t bytes)
{
  constexpr const char* suffixes[] = {"B", "KiB", "MiB", "GiB", "TiB"};
  auto value = double(bytes);
  std::size_t suffix = 0;
  while (value >= 1024.0 && suffix + 1 < std::size(suffixes))
  {
    value /= 1024.0;
    ++suffix;
  }
  const auto decimals = suffix == 0 ? 0 : value < 10.0 ? 2 : 1;
  return juce::String(value, decimals) + " " + suffixes[suffix];
}

class ViewerComponent final : public juce::Component,
                              private juce::Timer,
                              private juce::ScrollBar::Listener
{
public:
  friend struct BacktrackTestAccess;

  ViewerComponent()
  {
    addAndMakeVisible(_capture_selector);
    addAndMakeVisible(_go_start);
    addAndMakeVisible(_fit);
    addAndMakeVisible(_live_button);
    addAndMakeVisible(_scrollbar);
    addAndMakeVisible(_status);
    _status.setJustificationType(juce::Justification::centredLeft);
#if JUCE_MAJOR_VERSION >= 8
    _status.setFont(juce::FontOptions(12.0f));
#else
    _status.setFont(juce::Font(12.0f));
#endif
    _capture_selector.onChange = [this] {
      const auto index = _capture_selector.getSelectedItemIndex();
      if (juce::isPositiveAndBelow(index, int(_captures.size())))
      {
        _selected_id = _captures[std::size_t(index)].id;
        _view_initialised = false;
        _live = true;
      }
      refresh_snapshot();
    };
    _go_start.onClick = [this] {
      _live = false;
      _view_start = 0.0;
      refresh_snapshot();
    };
    _fit.onClick = [this] {
      const auto width = double(std::max(1, waveform_bounds().getWidth()));
      _samples_per_pixel =
          std::max(0.05, double(_snapshot.metadata.sample_count) / width);
      _view_start = 0.0;
      _view_initialised = true;
      _live = false;
      refresh_snapshot();
    };
    _live_button.onClick = [this] {
      _live = true;
      refresh_snapshot();
    };
    _scrollbar.addListener(this);
    startTimerHz(30);
    refresh_captures();
  }

  void resized() override
  {
    auto bounds = getLocalBounds().reduced(8);
    auto toolbar = bounds.removeFromTop(30);
    _capture_selector.setBounds(toolbar.removeFromLeft(230));
    toolbar.removeFromLeft(8);
    _go_start.setBounds(toolbar.removeFromLeft(96));
    toolbar.removeFromLeft(5);
    _fit.setBounds(toolbar.removeFromLeft(72));
    toolbar.removeFromLeft(5);
    _live_button.setBounds(toolbar.removeFromLeft(64));
    _scrollbar.setBounds(bounds.removeFromBottom(16));
    bounds.removeFromBottom(4);
    _status.setBounds(bounds.removeFromBottom(24));
    refresh_snapshot();
  }

  void paint(juce::Graphics& graphics) override
  {
    graphics.fillAll(juce::Colour(0xff15171a));
    auto area = waveform_bounds();
    if (_snapshot.metadata.channel_count == 0)
    {
      graphics.setColour(juce::Colour(0xffb7bec8));
      graphics.setFont(15.0f);
      graphics.drawFittedText("No Backtrack buffers", area,
                              juce::Justification::centred, 1);
      return;
    }

    paint_time_ruler(graphics, area.removeFromTop(20));
    const auto lane_height = float(area.getHeight()) /
                             float(_snapshot.metadata.channel_count);
    for (int channel = 0; channel < _snapshot.metadata.channel_count; ++channel)
    {
      auto lane = juce::Rectangle<float>(
          float(area.getX()), float(area.getY()) + lane_height * float(channel),
          float(area.getWidth()), lane_height);
      paint_channel(graphics, lane, std::size_t(channel));
    }
    paint_hover(graphics, area, lane_height);
  }

  void mouseDown(const juce::MouseEvent& event) override
  {
    _dragging = waveform_bounds().contains(event.getPosition());
    _drag_origin_start = _view_start;
  }

  void mouseDrag(const juce::MouseEvent& event) override
  {
    if (!_dragging)
      return;
    _live = false;
    const auto width = double(std::max(1, waveform_bounds().getWidth()));
    _view_start = clamp_view_start(
        _drag_origin_start -
            double(event.getDistanceFromDragStartX()) * _samples_per_pixel,
        double(_snapshot.metadata.sample_count), width * _samples_per_pixel);
    refresh_snapshot();
  }

  void mouseMove(const juce::MouseEvent& event) override
  {
    _mouse_position = event.getPosition();
    repaint();
  }

  void mouseExit(const juce::MouseEvent&) override
  {
    _mouse_position.reset();
    repaint();
  }

  void mouseWheelMove(const juce::MouseEvent& event,
                      const juce::MouseWheelDetails& wheel) override
  {
    auto area = waveform_bounds();
    area.removeFromTop(20);
    if (!area.contains(event.getPosition()) || wheel.deltaY == 0.0f)
      return;
    _live = false;
    const auto local_x = double(event.position.x - float(area.getX()));
    const auto width = double(std::max(1, area.getWidth()));
    const auto total = double(_snapshot.metadata.sample_count);
    const auto new_scale =
        zoomed_scale(_samples_per_pixel, double(wheel.deltaY), total, width);
    _view_start = zoom_anchored_start(
        _view_start, _samples_per_pixel, new_scale, local_x, total, width);
    _samples_per_pixel = new_scale;
    _view_initialised = true;
    refresh_snapshot();
  }

private:
  void scrollBarMoved(juce::ScrollBar*, double start) override
  {
    _live = false;
    _view_start = start;
    refresh_snapshot();
  }

  void timerCallback() override
  {
    if (!isShowing())
      return;
    try
    {
      refresh_captures();
      refresh_snapshot();
    }
    catch (...)
    {
    }
  }

  void refresh_captures()
  {
    std::vector<CaptureMetadata> latest;
    for (const auto& capture : registry().captures())
      latest.push_back(capture->metadata());
    // Display names are fixed per id, so comparing ids is enough.
    const auto unchanged = std::equal(
        latest.begin(), latest.end(), _captures.begin(), _captures.end(),
        [](const auto& a, const auto& b) { return a.id == b.id; });
    _captures = std::move(latest);
    if (unchanged)
      return;

    _capture_selector.clear(juce::dontSendNotification);
    int selected_index = -1;
    for (std::size_t index = 0; index < _captures.size(); ++index)
    {
      _capture_selector.addItem(
          _captures[index].display_name, int(index) + 1);
      if (_captures[index].id == _selected_id)
        selected_index = int(index);
    }
    if (selected_index < 0 && !_captures.empty())
    {
      selected_index = 0;
      _selected_id = _captures.front().id;
      _view_initialised = false;
      _live = true;
    }
    if (selected_index >= 0)
      _capture_selector.setSelectedItemIndex(
          selected_index, juce::dontSendNotification);
  }

  juce::Rectangle<int> waveform_bounds() const
  {
    auto bounds = getLocalBounds().reduced(8);
    bounds.removeFromTop(38);
    bounds.removeFromBottom(48);
    return bounds;
  }

  void refresh_snapshot()
  {
    const auto capture = registry().find(_selected_id);
    if (!capture)
    {
      _snapshot = {};
      _status.setText("No Backtrack buffer selected", juce::dontSendNotification);
      _status.setColour(
          juce::Label::textColourId, juce::Colour(0xffaab2bd));
      repaint();
      return;
    }

    const auto metadata = capture->metadata();
    const auto width = double(std::max(1, waveform_bounds().getWidth()));
    if (!_view_initialised)
    {
      _samples_per_pixel =
          std::max(0.05, metadata.sample_rate * 5.0 / width);
      _view_initialised = true;
    }
    const auto visible_samples = width * _samples_per_pixel;
    if (_live)
      _view_start = std::max(0.0, double(metadata.sample_count) - visible_samples);
    _view_start = clamp_view_start(
        _view_start, double(metadata.sample_count), visible_samples);

    _snapshot = capture->snapshot(std::size_t(std::floor(_view_start)),
                                  std::size_t(std::ceil(visible_samples)) + 2,
                                  peak_bucket_request(_samples_per_pixel));

    // Both calls must stay silent: a queued notification would arrive after
    // this returns and look like a user scroll, turning off Live.
    const auto range_end = std::max(double(metadata.sample_count), visible_samples);
    _scrollbar.setRangeLimits(0.0, range_end, juce::dontSendNotification);
    _scrollbar.setCurrentRange(
        _view_start, std::min(visible_samples, range_end),
        juce::dontSendNotification);
    _live_button.setColour(
        juce::TextButton::buttonColourId,
        _live ? juce::Colour(0xff236b4b) : juce::Colour(0xff343941));
    update_status(metadata);
    repaint();
  }

  void update_status(const CaptureMetadata& metadata)
  {
    std::uint64_t aggregate_retained = 0;
    for (const auto& capture : _captures)
      aggregate_retained += capture.retained_bytes;

    const auto memory_mb = juce::SystemStats::getMemorySizeInMegabytes();
    const auto physical_bytes =
        memory_mb > 0 ? std::uint64_t(memory_mb) << 20 : std::uint64_t(0);

    auto text = juce::String(double(metadata.sample_count) / metadata.sample_rate, 3) +
                " s  |  " + juce::String(metadata.sample_rate, 1) + " Hz  |  " +
                juce::String(metadata.channel_count) + " ch  |  dropped " +
                juce::String(std::uint64_t(metadata.dropped_blocks)) +
                "  |  retained " + format_bytes(metadata.retained_bytes) +
                "  |  reserved " + format_bytes(metadata.reserved_bytes) +
                "  |  Backtrack memory estimate " +
                format_bytes(aggregate_retained);
    if (physical_bytes != 0)
      text += " (" +
              juce::String(100.0 * double(aggregate_retained) /
                               double(physical_bytes),
                           2) +
              "% RAM)";
    _status.setText(text, juce::dontSendNotification);

    const auto level = memory_pressure_level(aggregate_retained, physical_bytes);
    _status.setColour(juce::Label::textColourId,
                      level == 2   ? juce::Colour(0xffff6868)
                      : level == 1 ? juce::Colour(0xffffbd5b)
                                   : juce::Colour(0xffaab2bd));
  }

  float x_for_sample(float origin_x, double sample) const noexcept
  {
    return origin_x + float((sample - _view_start) / _samples_per_pixel);
  }

  void paint_time_ruler(juce::Graphics& graphics, juce::Rectangle<int> ruler)
  {
    graphics.setColour(juce::Colour(0xff89919c));
    graphics.setFont(11.0f);
    const auto sample_rate = _snapshot.metadata.sample_rate;
    const auto step = ruler_step(_samples_per_pixel * 100.0 / sample_rate);
    const auto decimals = ruler_decimals(step);
    const auto first_seconds = _view_start / sample_rate;
    const auto end_seconds =
        (_view_start + double(ruler.getWidth()) * _samples_per_pixel) / sample_rate;
    // Index ticks by integer so long sessions do not accumulate rounding drift.
    for (auto tick = std::ceil(first_seconds / step); tick * step <= end_seconds;
         tick += 1.0)
    {
      const auto time = tick * step;
      const auto x =
          int(std::lround(x_for_sample(float(ruler.getX()), time * sample_rate)));
      graphics.drawVerticalLine(x, float(ruler.getBottom() - 4),
                                float(ruler.getBottom()));
      graphics.drawText(juce::String(time, decimals) + " s",
                        x + 3, ruler.getY(), 80, ruler.getHeight() - 2,
                        juce::Justification::centredLeft, false);
    }
  }

  void paint_channel(juce::Graphics& graphics, juce::Rectangle<float> lane,
                     std::size_t channel)
  {
    juce::Graphics::ScopedSaveState save_state(graphics);
    graphics.reduceClipRegion(lane.getSmallestIntegerContainer());
    const auto centre_y = lane.getCentreY();
    const auto scale_y = lane.getHeight() * 0.44f;
    graphics.setColour(juce::Colour(0xff30353d));
    graphics.drawHorizontalLine(int(centre_y), lane.getX(), lane.getRight());
    graphics.drawHorizontalLine(int(lane.getBottom()), lane.getX(), lane.getRight());
    graphics.setColour(juce::Colour(0xff7ed7a7));

    const auto& peaks = _snapshot.peaks[channel];
    if (_snapshot.exact())
    {
      juce::Path path;
      for (std::size_t index = 0; index < peaks.size(); ++index)
      {
        const auto x = x_for_sample(
            lane.getX(), double(_snapshot.first_sample + index));
        const auto y = centre_y - peaks[index].minimum * scale_y;
        if (index == 0)
          path.startNewSubPath(x, y);
        else
          path.lineTo(x, y);
        if (_samples_per_pixel <= 0.25)
          graphics.fillEllipse(x - 2.5f, y - 2.5f, 5.0f, 5.0f);
      }
      graphics.strokePath(path, juce::PathStrokeType(1.25f));
    }
    else
    {
      for (std::size_t index = 0; index < peaks.size(); ++index)
      {
        const auto x = x_for_sample(
            lane.getX(),
            double(_snapshot.first_sample + index * _snapshot.bucket_samples));
        const auto [top, bottom] = peak_column(peaks[index], centre_y, scale_y);
        graphics.drawVerticalLine(int(std::lround(x)), top, bottom);
      }
    }

    graphics.setColour(juce::Colour(0xff89919c));
    graphics.setFont(11.0f);
    graphics.drawText("ch " + juce::String(int(channel) + 1),
                      int(lane.getX()) + 4, int(lane.getY()) + 2, 42, 16,
                      juce::Justification::centredLeft, false);
  }

  void paint_hover(juce::Graphics& graphics, juce::Rectangle<int> area,
                   float lane_height)
  {
    if (!_mouse_position || !_snapshot.exact() || _samples_per_pixel > 0.25 ||
        !area.contains(*_mouse_position))
      return;
    const auto channel = std::size_t(std::clamp(
        int((float(_mouse_position->y - area.getY())) / lane_height), 0,
        _snapshot.metadata.channel_count - 1));
    const auto index = std::size_t(std::max(
        0.0, std::round(_view_start +
                        double(_mouse_position->x - area.getX()) *
                            _samples_per_pixel)));
    const auto& peaks = _snapshot.peaks[channel];
    if (index < _snapshot.first_sample ||
        index - _snapshot.first_sample >= peaks.size())
      return;
    const auto value = peaks[index - _snapshot.first_sample].minimum;
    const auto text = "ch " + juce::String(channel + 1) + "  sample " +
                      juce::String(std::uint64_t(index)) + "  " +
                      juce::String(double(index) / _snapshot.metadata.sample_rate, 6) +
                      " s  value " + juce::String(value, 7);
    auto box = juce::Rectangle<int>(
        _mouse_position->x + 12, _mouse_position->y - 26, 350, 22);
    box = box.constrainedWithin(getLocalBounds().reduced(4));
    graphics.setColour(juce::Colour(0xe62a2e34));
    graphics.fillRoundedRectangle(box.toFloat(), 4.0f);
    graphics.setColour(juce::Colours::white);
    graphics.setFont(12.0f);
    graphics.drawText(text, box.reduced(6, 1), juce::Justification::centredLeft,
                      false);
  }

  juce::ComboBox _capture_selector;
  juce::TextButton _go_start {"Go to Start"};
  juce::TextButton _fit {"Fit All"};
  juce::TextButton _live_button {"Live"};
  juce::ScrollBar _scrollbar {false};
  juce::Label _status;
  std::vector<CaptureMetadata> _captures;
  WaveformSnapshot _snapshot;
  std::uint64_t _selected_id = 0;
  double _view_start = 0.0;
  double _samples_per_pixel = 1.0;
  double _drag_origin_start = 0.0;
  bool _view_initialised = false;
  bool _live = true;
  bool _dragging = false;
  std::optional<juce::Point<int>> _mouse_position;
};

class BacktrackWindow final : public juce::DocumentWindow
{
public:
  explicit BacktrackWindow(juce::Component& editor)
      : DocumentWindow("Backtrack", juce::Colour(0xff202328),
                       DocumentWindow::allButtons)
  {
    setUsingNativeTitleBar(true);
    setResizable(true, false);
    setResizeLimits(500, 300, 4096, 2160);
    setContentNonOwned(&_viewer, true);
    centreAroundComponent(&editor, 900, 520);
  }

  void closeButtonPressed() override { Backtrack::hide(); }

private:
  friend struct BacktrackTestAccess;
  // Destroyed before the window base; the window tracks it with a SafePointer.
  ViewerComponent _viewer;
};

class GuiController final : public juce::DeletedAtShutdown,
                            private juce::ComponentListener
{
public:
  // Hand-written rather than JUCE_DECLARE_SINGLETON_INLINE, which JUCE 7 lacks.
  static GuiController* getInstance()
  {
    JUCE_ASSERT_MESSAGE_THREAD
    auto*& instance = slot();
    if (instance == nullptr)
      instance = new GuiController;
    return instance;
  }

  static GuiController* getInstanceWithoutCreating() noexcept { return slot(); }

  ~GuiController() override
  {
    detach_editor();
    slot() = nullptr;
  }

  void set_editor(juce::Component* editor)
  {
    JUCE_ASSERT_MESSAGE_THREAD
    if (_editor.getComponent() != editor)
    {
      detach_editor();
      _editor = editor;
      if (editor != nullptr)
        editor->addComponentListener(this);
    }
    captures_changed();
  }

  void captures_changed()
  {
    JUCE_ASSERT_MESSAGE_THREAD
    if (_editor == nullptr || registry().captures().empty())
    {
      if (_window)
        _window->setVisible(false);
      return;
    }
    // Respect an explicit close or hide(); only show() or toggle() reopens.
    if (!_hidden_by_user && !(_window && _window->isVisible()))
      show();
  }

  void show()
  {
    JUCE_ASSERT_MESSAGE_THREAD
    _hidden_by_user = false;
    if (!_window && _editor != nullptr && !registry().captures().empty())
      _window.emplace(*_editor.getComponent());
    if (_window)
    {
      _window->setVisible(true);
      _window->toFront(false);
    }
  }

  void hide()
  {
    JUCE_ASSERT_MESSAGE_THREAD
    _hidden_by_user = true;
    if (_window)
      _window->setVisible(false);
  }

  void toggle()
  {
    if (_window && _window->isVisible())
      hide();
    else
      show();
  }

private:
  friend struct BacktrackTestAccess;

  GuiController() = default;

  static GuiController*& slot() noexcept
  {
    static GuiController* instance = nullptr;
    return instance;
  }

  void detach_editor()
  {
    if (_editor != nullptr)
      _editor->removeComponentListener(this);
    _editor = nullptr;
    _window.reset();
  }

  void componentBeingDeleted(juce::Component& component) override
  {
    if (_editor.getComponent() == &component)
      detach_editor();
  }

  juce::Component::SafePointer<juce::Component> _editor;
  std::optional<BacktrackWindow> _window;
  bool _hidden_by_user = false;
};

inline void request_gui_refresh() noexcept
{
  if (juce::MessageManager::getInstanceWithoutCreating() == nullptr)
    return;
  try
  {
    juce::MessageManager::callAsync([] {
      if (auto* controller = GuiController::getInstanceWithoutCreating())
        controller->captures_changed();
    });
  }
  catch (...)
  {
  }
}

} // namespace detail

inline Backtrack::Backtrack(std::string name, double sample_rate,
                            int channel_count)
{
  if (name.empty())
    throw std::invalid_argument("Backtrack name must not be empty");
  if (!std::isfinite(sample_rate) || sample_rate <= 0.0)
    throw std::invalid_argument("Backtrack sample rate must be positive and finite");
  if (channel_count <= 0)
    throw std::invalid_argument("Backtrack channel count must be positive");

  _state = std::make_shared<detail::CaptureState>(
      std::move(name), sample_rate, channel_count);
  detail::registry().add(_state);
  detail::request_gui_refresh();
}

inline Backtrack::~Backtrack()
{
  detail::registry().remove(_state.get());
  detail::request_gui_refresh();
}

inline void Backtrack::setEditor(juce::Component* editor)
{
  detail::GuiController::getInstance()->set_editor(editor);
}

inline void Backtrack::show()
{
  detail::GuiController::getInstance()->show();
}

inline void Backtrack::hide()
{
  if (auto* controller = detail::GuiController::getInstanceWithoutCreating())
    controller->hide();
}

inline void Backtrack::toggle()
{
  detail::GuiController::getInstance()->toggle();
}

} // namespace la
