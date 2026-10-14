#include "backtrack.hpp"

#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <optional>
#include <random>
#include <stdexcept>
#include <thread>

std::size_t la_backtrack_odr_probe();
la::detail::Registry* la_backtrack_odr_registry();
std::atomic<int>* la_backtrack_odr_countdown();
la::detail::GuiController* la_backtrack_odr_controller();

namespace la::detail {

struct BacktrackTestAccess
{
  static std::shared_ptr<CaptureState> state(const Backtrack& backtrack)
  {
    return backtrack._state;
  }

  static bool has_editor(const GuiController& controller)
  {
    return controller._editor != nullptr;
  }

  static BacktrackWindow* window(GuiController& controller)
  {
    return controller._window ? &*controller._window : nullptr;
  }

  static ViewerComponent& viewer(BacktrackWindow& window) { return window._viewer; }

  static const WaveformSnapshot& snapshot(const ViewerComponent& viewer)
  {
    return viewer._snapshot;
  }

  static bool live(const ViewerComponent& viewer) { return viewer._live; }
  static double view_start(const ViewerComponent& viewer) { return viewer._view_start; }

  static double samples_per_pixel(const ViewerComponent& viewer)
  {
    return viewer._samples_per_pixel;
  }

  static std::uint64_t selected(const ViewerComponent& viewer)
  {
    return viewer._selected_id;
  }

  static int item_count(const ViewerComponent& viewer)
  {
    return viewer._capture_selector.getNumItems();
  }

  static juce::Rectangle<int> waveform_bounds(const ViewerComponent& viewer)
  {
    return viewer.waveform_bounds();
  }

  static void click_go_to_start(ViewerComponent& viewer) { viewer._go_start.onClick(); }
  static void click_fit_all(ViewerComponent& viewer) { viewer._fit.onClick(); }
  static void click_live(ViewerComponent& viewer) { viewer._live_button.onClick(); }

  static void scroll_to(ViewerComponent& viewer, double start)
  {
    viewer._scrollbar.setCurrentRangeStart(start, juce::sendNotificationSync);
  }

  static void select(ViewerComponent& viewer, std::uint64_t id)
  {
    for (std::size_t index = 0; index < viewer._captures.size(); ++index)
      if (viewer._captures[index].id == id)
        viewer._capture_selector.setSelectedItemIndex(
            int(index), juce::sendNotificationSync);
  }

  static void set_view(ViewerComponent& viewer, double start,
                       double samples_per_pixel)
  {
    viewer._live = false;
    viewer._view_initialised = true;
    viewer._view_start = start;
    viewer._samples_per_pixel = samples_per_pixel;
    viewer.refresh_snapshot();
  }

  // What the timer does, without requiring the viewer to be on screen.
  static void refresh(ViewerComponent& viewer)
  {
    viewer.refresh_captures();
    viewer.refresh_snapshot();
  }

  static void tick(ViewerComponent& viewer) { viewer.timerCallback(); }
};

} // namespace la::detail

namespace {

int failures = 0;

// Unlike assert(), CHECK stays active in Release builds.
#define CHECK(condition)                                                     \
  do                                                                         \
  {                                                                          \
    if (!(condition))                                                        \
    {                                                                        \
      ++failures;                                                            \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__,  \
                   #condition);                                              \
    }                                                                        \
  } while (false)

using Access = la::detail::BacktrackTestAccess;

auto state(const la::Backtrack& capture)
{
  return Access::state(capture);
}

bool near(double a, double b)
{
  return std::abs(a - b) <= 1.0e-12 * std::max(1.0, std::abs(b));
}

bool close(double a, double b)
{
  return std::abs(a - b) <= 1.0e-6 * std::max(1.0, std::abs(b));
}

bool same_peaks(const std::vector<la::detail::Peak>& a,
                const std::vector<la::detail::Peak>& b)
{
  return std::equal(a.begin(), a.end(), b.begin(), b.end(),
                    [](auto x, auto y) {
                      return x.minimum == y.minimum && x.maximum == y.maximum;
                    });
}

juce::MouseEvent mouse_event(juce::Component& component,
                             juce::Point<float> position,
                             juce::Point<float> down_position)
{
  const auto now = juce::Time::getCurrentTime();
  return {juce::Desktop::getInstance().getMainMouseSource(),
          position, {}, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, &component, &component,
          now, down_position, now, 1, position != down_position};
}

bool contains_colour(const juce::Image& image, juce::Colour colour)
{
  for (int y = 0; y < image.getHeight(); ++y)
    for (int x = 0; x < image.getWidth(); ++x)
    {
      const auto pixel = image.getPixelAt(x, y);
      if (std::abs(pixel.getRed() - colour.getRed()) <= 8 &&
          std::abs(pixel.getGreen() - colour.getGreen()) <= 8 &&
          std::abs(pixel.getBlue() - colour.getBlue()) <= 8)
        return true;
    }
  return false;
}

// Runs the message loop until everything posted so far has been delivered:
// the queue is FIFO, so a marker posted now arrives after all earlier messages.
void pump_messages()
{
  const auto delivered = std::make_shared<bool>(false);
  juce::MessageManager::callAsync([delivered] { *delivered = true; });
  for (int attempt = 0; attempt < 500 && !*delivered; ++attempt)
    juce::MessageManager::getInstance()->runDispatchLoopUntil(10);
  CHECK(*delivered);
}

void test_constructor()
{
  la::Backtrack capture {"Smoke", 48'000.0, 2};
  CHECK(state(capture)->channels[0].samples.capacity() >= 48'000);

  bool empty_name_rejected = false;
  try
  {
    la::Backtrack invalid {"", 48'000.0, 1};
  }
  catch (const std::invalid_argument&)
  {
    empty_name_rejected = true;
  }
  CHECK(empty_name_rejected);

  for (const auto invalid_rate :
       {0.0, -1.0, std::numeric_limits<double>::infinity(),
        std::numeric_limits<double>::quiet_NaN()})
  {
    bool rejected = false;
    try
    {
      la::Backtrack invalid {"invalid", invalid_rate, 1};
    }
    catch (const std::invalid_argument&)
    {
      rejected = true;
    }
    CHECK(rejected);
  }

  for (const auto invalid_channels : {0, -1})
  {
    bool rejected = false;
    try
    {
      la::Backtrack invalid {"invalid", 48'000.0, invalid_channels};
    }
    catch (const std::invalid_argument&)
    {
      rejected = true;
    }
    CHECK(rejected);
  }

  bool too_large_rejected = false;
  try
  {
    la::Backtrack invalid {"invalid", 1.0e300, 1};
  }
  catch (const std::length_error&)
  {
    too_large_rejected = true;
  }
  CHECK(too_large_rejected);

  // Failed constructions never register.
  CHECK(la::detail::registry().captures().size() == 1);
}

void test_record_overloads()
{
  la::Backtrack stereo {"Stereo", 128.0, 2};
  juce::AudioBuffer<float> buffer(2, 6);
  for (int sample = 0; sample < 6; ++sample)
  {
    buffer.setSample(0, sample, float(sample));
    buffer.setSample(1, sample, float(-sample));
  }
  stereo.record(buffer, 1, 4);

  const std::array<float, 2> left {10.0f, 11.0f};
  const std::array<float, 2> right {-10.0f, -11.0f};
  const std::array<std::span<const float>, 2> planar {left, right};
  stereo.record(planar);
  stereo.record(buffer, 5);

  const auto stereo_state = state(stereo);
  CHECK(stereo_state->channels[0].samples ==
        std::vector<float>({1, 2, 3, 4, 10, 11, 5}));
  CHECK(stereo_state->channels[1].samples ==
        std::vector<float>({-1, -2, -3, -4, -10, -11, -5}));

  la::Backtrack mono {"Mono", 128.0, 1};
  const std::array<float, 3> samples {0.25f, -0.5f, 0.75f};
  mono.record(samples);
  CHECK(state(mono)->channels[0].samples ==
        std::vector<float>(samples.begin(), samples.end()));
}

void test_invalid_and_atomic_failure()
{
  la::Backtrack capture {"Invalid", 128.0, 2};
  const std::array<float, 2> left {1, 2};
  const std::array<float, 1> short_right {3};
  const std::array<std::span<const float>, 2> unequal {left, short_right};
  capture.record(unequal);
  capture.record(std::span<const float>(left));

  const std::array<float, 2> right {3, 4};
  const std::array<std::span<const float>, 2> valid {left, right};
  la::detail::allocation_fail_countdown().store(0);
  capture.record(valid);

  juce::AudioBuffer<float> buffer(2, 4);
  capture.record(buffer, -1, 1);
  capture.record(buffer, 5, 0);
  capture.record(buffer, 0, -2);
  capture.record(buffer, 0, 5);
  capture.record(buffer, 4, -1);
  juce::AudioBuffer<float> wrong_channels(1, 4);
  capture.record(wrong_channels);

  const auto capture_state = state(capture);
  CHECK(capture_state->channels[0].samples.empty());
  CHECK(capture_state->channels[1].samples.empty());
  CHECK(capture_state->dropped_blocks == 8);
}

void test_allocation_failure_preserves_existing_audio()
{
  la::Backtrack capture {"Preserve", 128.0, 2};
  std::array<float, 100> left {};
  std::array<float, 100> right {};
  for (std::size_t i = 0; i < left.size(); ++i)
  {
    left[i] = float(i);
    right[i] = -float(i);
  }
  const std::array<std::span<const float>, 2> first {left, right};
  capture.record(first);

  // Countdown 1 lets channel 0 reserve, then fails on channel 1.
  const std::array<float, 10> extra {};
  const std::array<std::span<const float>, 2> second {extra, extra};
  la::detail::allocation_fail_countdown().store(1);
  capture.record(second);

  const auto capture_state = state(capture);
  CHECK(capture_state->dropped_blocks == 1);
  for (std::size_t channel = 0; channel < 2; ++channel)
  {
    const auto& storage = capture_state->channels[channel];
    CHECK(storage.samples.size() == 100);
    CHECK(storage.summaries[0].buckets.size() == 1);
    CHECK(storage.summaries[0].partial_count == 36);
  }
  CHECK(capture_state->channels[0].samples[99] == 99.0f);
  CHECK(capture_state->channels[1].samples[99] == -99.0f);

  capture.record(second);
  CHECK(capture_state->channels[0].samples.size() == 110);
  CHECK(capture_state->channels[0].summaries[0].buckets.size() == 1);
  CHECK(capture_state->channels[0].summaries[0].partial_count == 46);
}

void test_growth_beyond_reserve_is_geometric()
{
  // The smoke target reserves one second, so this records far past it.
  la::Backtrack capture {"Growth", 128.0, 1};
  const auto capture_state = state(capture);
  const auto& channel = capture_state->channels[0];

  constexpr std::size_t total = 100'000;
  constexpr std::size_t block = 16;
  std::array<float, block> samples {};
  std::size_t sample_reallocations = 0;
  std::size_t bucket_reallocations = 0;
  for (std::size_t written = 0; written < total; written += block)
  {
    for (std::size_t i = 0; i < block; ++i)
      samples[i] = float(written + i);
    const auto sample_capacity = channel.samples.capacity();
    const auto bucket_capacity = channel.summaries[0].buckets.capacity();
    capture.record(samples);
    sample_reallocations += channel.samples.capacity() != sample_capacity;
    bucket_reallocations +=
        channel.summaries[0].buckets.capacity() != bucket_capacity;
  }

  CHECK(sample_reallocations <= 16);
  CHECK(bucket_reallocations <= 16);
  CHECK(channel.samples.size() == total);
  CHECK(channel.summaries[0].buckets.size() == total / 64);
  CHECK(channel.samples[total - 1] == float(total - 1));
  CHECK(capture_state->dropped_blocks == 0);
}

void test_summaries()
{
  la::Backtrack capture {"Summary", 256.0, 1};
  std::array<float, 128> samples {};
  for (std::size_t i = 0; i < samples.size(); ++i)
    samples[i] = float(i) - 70.0f;
  capture.record(std::span<const float>(samples.data(), 63));
  capture.record(std::span<const float>(samples.data() + 63, 65));

  const auto capture_state = state(capture);
  const auto& channel = capture_state->channels[0];
  CHECK(channel.summaries[0].buckets.size() == 2);
  CHECK(channel.summaries[0].buckets[0].minimum == -70.0f);
  CHECK(channel.summaries[0].buckets[0].maximum == -7.0f);
  CHECK(channel.summaries[0].buckets[1].minimum == -6.0f);
  CHECK(channel.summaries[0].buckets[1].maximum == 57.0f);
  CHECK(channel.summaries[1].buckets.size() == 1);
  CHECK(channel.summaries[1].buckets[0].minimum == -70.0f);
  CHECK(channel.summaries[1].buckets[0].maximum == 57.0f);

  std::array<float, 200> growth {};
  capture.record(growth);
  CHECK(capture_state->channels[0].samples.size() == 328);
  CHECK(capture_state->channels[0].samples.capacity() >= 328);

  la::Backtrack partial_capture {"Partial summary", 256.0, 1};
  std::array<float, 100> partial_samples {};
  for (std::size_t i = 0; i < partial_samples.size(); ++i)
    partial_samples[i] = float(i);
  partial_capture.record(partial_samples);
  const auto partial = state(partial_capture)->snapshot(0, 100, 128);
  CHECK(partial.peaks[0].size() == 1);
  CHECK(partial.peaks[0][0].minimum == 0.0f);
  CHECK(partial.peaks[0][0].maximum == 99.0f);
}

void test_peak_level_selection()
{
  la::Backtrack capture {"Levels", 128.0, 1};
  std::array<float, 1024> samples {};
  capture.record(samples);
  const auto capture_state = state(capture);

  // The coarsest level not coarser than requested, so columns never leave gaps.
  CHECK(capture_state->snapshot(0, 1024, 64).bucket_samples == 64);
  CHECK(capture_state->snapshot(0, 1024, 100).bucket_samples == 64);
  CHECK(capture_state->snapshot(0, 1024, 128).bucket_samples == 128);
  CHECK(capture_state->snapshot(0, 1024, 255).bucket_samples == 128);
  CHECK(capture_state->snapshot(0, 1024, 256).bucket_samples == 256);
}

void test_in_progress_bucket_merges_lower_partials()
{
  // 1000 samples leave unfinished buckets on levels 0-3; the unfinished
  // 512-sample bucket must cover samples 512..999 exactly.
  la::Backtrack capture {"In progress", 128.0, 1};
  std::vector<float> samples(1000);
  for (std::size_t i = 0; i < samples.size(); ++i)
    samples[i] = float(i);
  capture.record(samples);

  const auto snapshot = state(capture)->snapshot(0, 1000, 512);
  CHECK(snapshot.bucket_samples == 512);
  CHECK(snapshot.peaks[0].size() == 2);
  CHECK(snapshot.peaks[0][0].minimum == 0.0f);
  CHECK(snapshot.peaks[0][0].maximum == 511.0f);
  CHECK(snapshot.peaks[0][1].minimum == 512.0f);
  CHECK(snapshot.peaks[0][1].maximum == 999.0f);

  const auto tail_only = state(capture)->snapshot(600, 400, 512);
  CHECK(tail_only.first_sample == 512);
  CHECK(tail_only.peaks[0].size() == 1);
  CHECK(tail_only.peaks[0][0].minimum == 512.0f);
}

void test_reduced_peaks_from_samples()
{
  la::Backtrack capture {"Reduced", 128.0, 2};
  std::array<float, 100> left {};
  std::array<float, 100> right {};
  for (std::size_t i = 0; i < left.size(); ++i)
  {
    left[i] = float(i);
    right[i] = -float(i);
  }
  const std::array<std::span<const float>, 2> planar {left, right};
  capture.record(planar);
  const auto capture_state = state(capture);

  const auto snapshot = capture_state->snapshot(10, 20, 4);
  CHECK(!snapshot.exact());
  CHECK(snapshot.bucket_samples == 4);
  CHECK(snapshot.first_sample == 8);
  CHECK(snapshot.peaks.size() == 2);
  CHECK(snapshot.peaks[0].size() == 6);
  CHECK(snapshot.peaks[0].front().minimum == 8.0f);
  CHECK(snapshot.peaks[0].front().maximum == 11.0f);
  // The last bucket spans its full extent, not just up to the range end.
  CHECK(snapshot.peaks[0].back().minimum == 28.0f);
  CHECK(snapshot.peaks[0].back().maximum == 31.0f);
  CHECK(snapshot.peaks[1].front().minimum == -11.0f);
  CHECK(snapshot.peaks[1].front().maximum == -8.0f);

  const auto clipped = capture_state->snapshot(96, 50, 8);
  CHECK(clipped.peaks[0].size() == 1);
  CHECK(clipped.peaks[0][0].minimum == 96.0f);
  CHECK(clipped.peaks[0][0].maximum == 99.0f);

  CHECK(capture_state->snapshot(200, 10, 4).peaks[0].empty());
}

void test_empty_block_is_noop()
{
  la::Backtrack capture {"Empty", 128.0, 1};
  la::detail::allocation_fail_countdown().store(0);
  capture.record(std::span<const float> {});
  CHECK(state(capture)->dropped_blocks == 0);
  la::detail::allocation_fail_countdown().store(-1);
}

void test_registry_and_names()
{
  std::optional<la::Backtrack> first;
  first.emplace("Input", 128.0, 1);
  la::Backtrack second {"Input", 128.0, 1};
  const auto first_state = state(*first);
  const auto second_state = state(second);
  CHECK(first_state->display_name == "Input");
  CHECK(second_state->display_name == "Input (2)");
  CHECK(la::detail::registry().find(first_state->id) != nullptr);

  first.reset();
  CHECK(la::detail::registry().find(first_state->id) == nullptr);
  CHECK(first_state->display_name == "Input");

  la::Backtrack third {"Input", 128.0, 1};
  CHECK(state(third)->display_name == "Input (3)");
}

void test_snapshots_and_concurrency()
{
  la::Backtrack capture {"Concurrent", 128.0, 1};
  const auto capture_state = state(capture);
  const std::array<float, 32> block {1.0f};

  std::thread writer([&] {
    for (int i = 0; i < 100; ++i)
      capture.record(block);
  });

  for (int i = 0; i < 100; ++i)
  {
    const auto metadata = capture_state->metadata();
    const auto first = metadata.sample_count > 16 ? metadata.sample_count - 16 : 0;
    const auto snapshot = capture_state->snapshot(first, 16, 1);
    CHECK(snapshot.exact());
    CHECK(snapshot.peaks.size() == 1);
    CHECK(snapshot.peaks[0].size() <= 16);
  }
  writer.join();

  const auto metadata = capture_state->metadata();
  CHECK(metadata.sample_count == 3200);
  CHECK(metadata.retained_bytes > 3200 * sizeof(float));
  CHECK(metadata.reserved_bytes >= metadata.retained_bytes);
  const auto peaks = capture_state->snapshot(0, metadata.sample_count, 128);
  CHECK(peaks.bucket_samples == 128);
  CHECK(!peaks.peaks[0].empty());
}

void test_snapshot_matches_brute_force()
{
  using la::detail::Peak;
  std::mt19937 random {20261014};
  const auto uniform = [&random](std::size_t low, std::size_t high) {
    return std::uniform_int_distribution<std::size_t>(low, high)(random);
  };
  std::uniform_real_distribution<float> value(-1.0f, 1.0f);
  constexpr std::array<std::size_t, 16> bucket_choices {
      0, 1, 2, 3, 7, 63, 64, 65, 100, 128, 129, 500, 1024, 4096,
      std::size_t(1) << 14, std::size_t(1) << 30};
  const auto largest_bucket =
      la::detail::bucket_samples_at(la::detail::summary_level_count - 1);

  for (int trial = 0; trial < 24; ++trial)
  {
    // Random block sizes so bucket boundaries fall anywhere within blocks.
    la::Backtrack capture {"Random", 128.0, 2};
    std::array<std::vector<float>, 2> reference;
    const auto total = trial == 0 ? 0 : trial == 1 ? 300'000 : uniform(1, 20'000);
    while (reference[0].size() < total)
    {
      const auto block = std::min(uniform(1, 700), total - reference[0].size());
      std::array<std::vector<float>, 2> data;
      for (std::size_t channel = 0; channel < 2; ++channel)
      {
        data[channel].resize(block);
        for (auto& sample : data[channel])
          sample = value(random);
        reference[channel].insert(reference[channel].end(), data[channel].begin(),
                                  data[channel].end());
      }
      const std::array<std::span<const float>, 2> planar {data[0], data[1]};
      capture.record(planar);
    }

    const auto capture_state = state(capture);
    CHECK(capture_state->channels[0].samples == reference[0]);
    CHECK(capture_state->channels[1].samples == reference[1]);

    for (int query = 0; query < 100; ++query)
    {
      const auto first = uniform(0, total + 64);
      const auto count = uniform(0, total + 64);
      const auto requested = bucket_choices[uniform(0, bucket_choices.size() - 1)];
      const auto snapshot = capture_state->snapshot(first, count, requested);

      auto size = std::max<std::size_t>(1, requested);
      if (requested >= la::detail::base_bucket_samples)
      {
        size = la::detail::base_bucket_samples;
        while (size * 2 <= requested && size < largest_bucket)
          size *= 2;
      }
      CHECK(snapshot.bucket_samples == size);
      CHECK(snapshot.first_sample == first / size * size);
      CHECK(snapshot.metadata.sample_count == total);

      // Every bucket overlapping [first, end), over its full recorded extent.
      const auto end = std::min(total, first + count);
      bool matches = snapshot.peaks.size() == 2;
      for (std::size_t channel = 0; matches && channel < 2; ++channel)
      {
        std::vector<Peak> expected;
        for (auto start = snapshot.first_sample; start < end; start += size)
        {
          const auto& samples = reference[channel];
          const auto [minimum, maximum] = std::minmax_element(
              samples.begin() + std::ptrdiff_t(start),
              samples.begin() + std::ptrdiff_t(std::min(total, start + size)));
          expected.push_back({*minimum, *maximum});
        }
        matches = same_peaks(snapshot.peaks[channel], expected);
      }
      CHECK(matches);
      if (!matches)
      {
        std::fprintf(stderr, "  total=%zu first=%zu count=%zu bucket=%zu\n",
                     total, first, count, requested);
        return;
      }
    }
  }
}

void test_byte_accounting()
{
  la::Backtrack capture {"Bytes", 128.0, 2};
  const std::array<float, 100> left {};
  const std::array<float, 100> right {};
  const std::array<std::span<const float>, 2> planar {left, right};
  capture.record(planar);

  const auto capture_state = state(capture);
  const auto metadata = capture_state->metadata();
  CHECK(metadata.display_name == "Bytes");
  CHECK(metadata.channel_count == 2);
  CHECK(metadata.sample_count == 100);
  // 100 samples and one finished 64-sample bucket per channel.
  CHECK(metadata.retained_bytes ==
        2 * (100 * sizeof(float) + sizeof(la::detail::Peak)));

  std::size_t reserved = 0;
  for (const auto& channel : capture_state->channels)
  {
    reserved += channel.samples.capacity() * sizeof(float);
    for (const auto& summary : channel.summaries)
      reserved += summary.buckets.capacity() * sizeof(la::detail::Peak);
  }
  CHECK(metadata.reserved_bytes == reserved);
  CHECK(metadata.reserved_bytes >= 2 * 128 * sizeof(float));
}

void test_concurrent_recording_lifecycle_and_snapshots()
{
  la::Backtrack shared {"Shared", 128.0, 2};
  constexpr int blocks = 500;
  const std::array<float, 64> block {};
  const std::array<std::span<const float>, 2> planar {block, block};
  std::atomic<bool> done {false};

  // A reader does what the viewer does while writers record and other
  // threads create and destroy buffers.
  std::thread reader([&] {
    while (!done.load())
      for (const auto& capture : la::detail::registry().captures())
      {
        const auto metadata = capture->metadata();
        for (const std::size_t bucket : {1, 4, 64, 256})
          (void)capture->snapshot(0, metadata.sample_count, bucket);
        (void)la::detail::registry().find(metadata.id);
      }
  });

  std::vector<std::thread> threads;
  for (int writer = 0; writer < 2; ++writer)
    threads.emplace_back([&] {
      for (int i = 0; i < blocks; ++i)
        shared.record(planar);
    });
  for (int lifecycle = 0; lifecycle < 2; ++lifecycle)
    threads.emplace_back([&] {
      for (int i = 0; i < 200; ++i)
      {
        la::Backtrack transient {"Transient", 128.0, 1};
        transient.record(std::span<const float>(block));
      }
    });
  for (auto& thread : threads)
    thread.join();
  done = true;
  reader.join();

  const auto metadata = state(shared)->metadata();
  CHECK(metadata.sample_count == std::size_t(2 * blocks * 64));
  CHECK(metadata.dropped_blocks == 0);
  CHECK(la::detail::registry().captures().size() == 1);
}

void test_viewport_math()
{
  CHECK(la::detail::clamp_view_start(-10.0, 1000.0, 100.0) == 0.0);
  CHECK(la::detail::clamp_view_start(950.0, 1000.0, 100.0) == 900.0);
  CHECK(la::detail::clamp_view_start(20.0, 50.0, 100.0) == 0.0);

  const auto start = la::detail::zoom_anchored_start(
      100.0, 2.0, 1.0, 50.0, 1000.0, 200.0);
  CHECK(start == 150.0);
  CHECK(start + 50.0 == 100.0 + 50.0 * 2.0);
}

void test_zoom_scale()
{
  using la::detail::zoomed_scale;
  // Short recording viewed at the 5-second default: zooming out must not
  // snap inwards to the recording-length limit.
  CHECK(zoomed_scale(500.0, -0.5, 1000.0, 1000.0) >= 500.0);
  CHECK(zoomed_scale(500.0, 0.5, 1000.0, 1000.0) < 500.0);
  CHECK(zoomed_scale(500.0, 0.5, 1000.0, 1000.0) > 2.0);

  // Long recording: zooming out is allowed up to twice the fit-all scale.
  CHECK(zoomed_scale(10.0, -0.5, 1.0e6, 1000.0) > 10.0);
  CHECK(zoomed_scale(1500.0, -10.0, 1.0e6, 1000.0) == 2000.0);
  CHECK(zoomed_scale(1.0, 10.0, 1.0e6, 1000.0) == 0.05);
}

void test_render_mode_selection()
{
  using la::detail::peak_bucket_request;
  CHECK(peak_bucket_request(0.05) == 1);
  CHECK(peak_bucket_request(1.0) == 1);
  CHECK(peak_bucket_request(1.9) == 1);
  CHECK(peak_bucket_request(2.0) == 2);
  CHECK(peak_bucket_request(4.0) == 4);
  CHECK(peak_bucket_request(64.5) == 64);
  CHECK(peak_bucket_request(0.3) == 1);
}

void test_ruler_labels()
{
  using la::detail::ruler_decimals;
  using la::detail::ruler_step;
  CHECK(near(ruler_step(0.00042), 0.0005));
  CHECK(near(ruler_step(0.0015), 0.002));
  CHECK(near(ruler_step(0.7), 1.0));
  CHECK(near(ruler_step(3.0), 5.0));
  CHECK(near(ruler_step(6.0), 10.0));

  CHECK(ruler_decimals(ruler_step(0.00004)) == 5);
  CHECK(ruler_decimals(ruler_step(0.00042)) == 4);
  CHECK(ruler_decimals(ruler_step(0.001)) == 3);
  CHECK(ruler_decimals(ruler_step(0.0015)) == 3);
  CHECK(ruler_decimals(ruler_step(0.3)) == 1);
  CHECK(ruler_decimals(ruler_step(1.0)) == 0);
  CHECK(ruler_decimals(ruler_step(3.0)) == 0);
  CHECK(ruler_decimals(ruler_step(40.0)) == 0);
}

void test_peak_columns_are_visible()
{
  const auto flat = la::detail::peak_column({0.5f, 0.5f}, 100.0f, 10.0f);
  CHECK(flat.first == 95.0f);
  CHECK(flat.second == 96.0f);

  const auto full = la::detail::peak_column({-1.0f, 1.0f}, 100.0f, 10.0f);
  CHECK(full.first == 90.0f);
  CHECK(full.second == 110.0f);
}

void test_memory_status_helpers()
{
  CHECK(la::detail::memory_pressure_level(99, 1000) == 0);
  CHECK(la::detail::memory_pressure_level(100, 1000) == 1);
  CHECK(la::detail::memory_pressure_level(249, 1000) == 1);
  CHECK(la::detail::memory_pressure_level(250, 1000) == 2);
  CHECK(la::detail::memory_pressure_level(500, 0) == 0);
  CHECK(la::detail::format_bytes(0) == "0 B");
  CHECK(la::detail::format_bytes(1023) == "1023 B");
  CHECK(la::detail::format_bytes(1024) == "1.00 KiB");
  CHECK(la::detail::format_bytes(1536) == "1.50 KiB");
  CHECK(la::detail::format_bytes(10 * 1024) == "10.0 KiB");
  CHECK(la::detail::format_bytes(std::uint64_t(5) << 20) == "5.00 MiB");
  CHECK(la::detail::format_bytes(std::uint64_t(3) << 30) == "3.00 GiB");
  CHECK(la::detail::format_bytes(std::uint64_t(1) << 50) == "1024.0 TiB");
}

void test_gui_editor_lifetime()
{
  {
    juce::Component editor;
    la::Backtrack::setEditor(&editor);
    CHECK(Access::has_editor(*la::detail::GuiController::getInstanceWithoutCreating()));
    // Both translation units share one controller.
    CHECK(la_backtrack_odr_controller() ==
          la::detail::GuiController::getInstanceWithoutCreating());
  }
  auto& controller = *la::detail::GuiController::getInstanceWithoutCreating();
  CHECK(!Access::has_editor(controller));
  CHECK(Access::window(controller) == nullptr);
  la::Backtrack::setEditor(nullptr);
}

void test_gui_window_respects_user_hide()
{
  std::optional<juce::Component> editor;
  editor.emplace().setSize(400, 300);
  la::Backtrack::setEditor(&*editor);
  auto& controller = *la::detail::GuiController::getInstanceWithoutCreating();
  CHECK(Access::window(controller) == nullptr);

  la::Backtrack first {"Window", 128.0, 1};
  pump_messages();
  auto* window = Access::window(controller);
  CHECK(window != nullptr);
  if (window == nullptr)
    return;
  CHECK(window->isVisible());

  la::Backtrack::hide();
  CHECK(!window->isVisible());
  {
    la::Backtrack second {"Window", 128.0, 1};
    pump_messages();
    CHECK(!window->isVisible());
  }
  pump_messages();
  CHECK(!window->isVisible());

  la::Backtrack::show();
  CHECK(window->isVisible());

  window->closeButtonPressed();
  CHECK(!window->isVisible());
  {
    la::Backtrack third {"Window", 128.0, 1};
    pump_messages();
    CHECK(!window->isVisible());
  }

  la::Backtrack::toggle();
  CHECK(window->isVisible());

  // The viewer only polls while it is on screen.
  auto& viewer = Access::viewer(*window);
  CHECK(window->getContentComponent() == &viewer);
  Access::select(viewer, state(first)->id);
  const std::array<float, 32> block {};
  first.record(block);
  Access::tick(viewer);
  CHECK(Access::snapshot(viewer).metadata.sample_count == 32);

  la::Backtrack::hide();
  first.record(block);
  Access::tick(viewer);
  CHECK(Access::snapshot(viewer).metadata.sample_count == 32);

  la::Backtrack::show();
  Access::tick(viewer);
  CHECK(Access::snapshot(viewer).metadata.sample_count == 64);

  editor.reset();
  CHECK(!Access::has_editor(controller));
  CHECK(Access::window(controller) == nullptr);
  la::Backtrack::setEditor(nullptr);
}

void test_gui_switching_to_shorter_capture_keeps_live()
{
  la::Backtrack long_capture {"Long", 128.0, 1};
  la::Backtrack short_capture {"Short", 128.0, 1};
  std::vector<float> samples(128 * 60);
  long_capture.record(samples);
  short_capture.record(std::span<const float>(samples.data(), 10));

  la::detail::ViewerComponent viewer;
  viewer.setSize(900, 520);
  Access::select(viewer, state(long_capture)->id);
  CHECK(Access::live(viewer));
  Access::select(viewer, state(short_capture)->id);
  CHECK(Access::live(viewer));
  pump_messages();
  CHECK(Access::live(viewer));
}

void test_gui_render_modes()
{
  la::Backtrack capture {"Render", 128.0, 2};
  std::vector<float> samples(128 * 60);
  const std::array<std::span<const float>, 2> planar {samples, samples};
  capture.record(planar);

  la::detail::ViewerComponent viewer;
  viewer.setSize(900, 520);
  Access::select(viewer, state(capture)->id);

  Access::set_view(viewer, 0.0, 0.5);
  CHECK(Access::snapshot(viewer).exact());
  CHECK(Access::snapshot(viewer).peaks.size() == 2);

  // Fewer than two samples per pixel still draws the exact waveform.
  Access::set_view(viewer, 0.0, 1.5);
  CHECK(Access::snapshot(viewer).exact());

  Access::set_view(viewer, 0.0, 4.0);
  CHECK(!Access::snapshot(viewer).exact());
  CHECK(Access::snapshot(viewer).bucket_samples == 4);
  // One column per pixel rather than every sample.
  CHECK(!Access::snapshot(viewer).peaks.empty() &&
        Access::snapshot(viewer).peaks[0].size() <= 900);

  Access::set_view(viewer, 0.0, 100.0);
  CHECK(!Access::snapshot(viewer).exact());
  CHECK(Access::snapshot(viewer).bucket_samples == 64);
}

void test_gui_window_follows_editor_and_buffers()
{
  auto& controller = *la::detail::GuiController::getInstance();

  // Without an editor, show() and toggle() have nothing to attach to.
  la::Backtrack::setEditor(nullptr);
  {
    la::Backtrack capture {"Lifecycle", 128.0, 1};
    la::Backtrack::show();
    CHECK(Access::window(controller) == nullptr);
    la::Backtrack::toggle();
    CHECK(Access::window(controller) == nullptr);
  }
  pump_messages();

  // An editor without buffers does not create a window.
  juce::Component first_editor;
  first_editor.setSize(400, 300);
  la::Backtrack::setEditor(&first_editor);
  la::Backtrack::show();
  CHECK(Access::window(controller) == nullptr);

  std::optional<la::Backtrack> capture;
  capture.emplace("Lifecycle", 128.0, 1);
  pump_messages();
  auto* window = Access::window(controller);
  CHECK(window != nullptr && window->isVisible());
  const juce::Component::SafePointer<juce::Component> first_window {window};

  // Switching editors replaces the window.
  juce::Component second_editor;
  second_editor.setSize(400, 300);
  la::Backtrack::setEditor(&second_editor);
  CHECK(first_window == nullptr);
  window = Access::window(controller);
  CHECK(window != nullptr && window->isVisible());

  // Losing the last buffer hides the window; a new one shows it again.
  capture.reset();
  pump_messages();
  window = Access::window(controller);
  CHECK(window != nullptr && !window->isVisible());
  capture.emplace("Lifecycle", 128.0, 1);
  pump_messages();
  CHECK(window->isVisible());

  capture.reset();
  pump_messages();
  la::Backtrack::setEditor(nullptr);
  CHECK(Access::window(controller) == nullptr);
}

void test_gui_viewer_controls()
{
  la::Backtrack capture {"Controls", 128.0, 1};
  std::vector<float> samples(128 * 60);
  capture.record(samples);
  std::size_t total = samples.size();
  const auto add_second = [&] {
    capture.record(std::span<const float>(samples.data(), 128));
    total += 128;
  };

  la::detail::ViewerComponent viewer;
  viewer.setSize(900, 520);
  Access::select(viewer, state(capture)->id);
  const auto bounds = Access::waveform_bounds(viewer);
  const auto width = double(bounds.getWidth());
  const auto view_end = [&] {
    return Access::view_start(viewer) + width * Access::samples_per_pixel(viewer);
  };

  // Live follows the newest audio.
  CHECK(Access::live(viewer));
  CHECK(close(view_end(), double(total)));
  add_second();
  Access::refresh(viewer);
  CHECK(close(view_end(), double(total)));

  // Go to Start leaves live mode and stays put as audio arrives.
  Access::click_go_to_start(viewer);
  CHECK(!Access::live(viewer));
  CHECK(Access::view_start(viewer) == 0.0);
  add_second();
  Access::refresh(viewer);
  CHECK(Access::view_start(viewer) == 0.0);

  Access::click_live(viewer);
  CHECK(Access::live(viewer));
  CHECK(close(view_end(), double(total)));

  // Dragging right moves back in time and leaves live mode.
  const auto drag_start = Access::view_start(viewer);
  const auto scale = Access::samples_per_pixel(viewer);
  const auto down = bounds.getCentre().toFloat();
  viewer.mouseDown(mouse_event(viewer, down, down));
  viewer.mouseDrag(mouse_event(viewer, down + juce::Point<float>(100.0f, 0.0f), down));
  CHECK(!Access::live(viewer));
  CHECK(close(Access::view_start(viewer), drag_start - 100.0 * scale));

  // A drag that starts outside the waveform is ignored.
  const auto after_drag = Access::view_start(viewer);
  const juce::Point<float> toolbar {20.0f, 20.0f};
  viewer.mouseDown(mouse_event(viewer, toolbar, toolbar));
  viewer.mouseDrag(mouse_event(viewer, toolbar + juce::Point<float>(200.0f, 0.0f), toolbar));
  CHECK(Access::view_start(viewer) == after_drag);

  // Wheel zoom keeps the sample under the cursor in place.
  const juce::Point<float> cursor {float(bounds.getX() + 300), float(bounds.getY() + 100)};
  const auto anchor = Access::view_start(viewer) + 300.0 * scale;
  juce::MouseWheelDetails wheel {};
  wheel.deltaY = 0.3f;
  viewer.mouseWheelMove(mouse_event(viewer, cursor, cursor), wheel);
  const auto zoomed = Access::samples_per_pixel(viewer);
  CHECK(zoomed < scale);
  CHECK(close(Access::view_start(viewer) + 300.0 * zoomed, anchor));

  // Fit All shows the whole recording.
  Access::click_fit_all(viewer);
  CHECK(!Access::live(viewer));
  CHECK(Access::view_start(viewer) == 0.0);
  CHECK(close(view_end(), double(total)));

  // A user scroll leaves live mode and moves the view.
  Access::set_view(viewer, 0.0, 1.0);
  Access::click_live(viewer);
  CHECK(Access::live(viewer));
  Access::scroll_to(viewer, 100.0);
  CHECK(!Access::live(viewer));
  CHECK(Access::view_start(viewer) == 100.0);
}

void test_gui_viewer_tracks_buffer_list()
{
  la::Backtrack first {"List A", 128.0, 1};
  std::optional<la::Backtrack> second;
  second.emplace("List B", 128.0, 1);

  la::detail::ViewerComponent viewer;
  viewer.setSize(900, 520);
  CHECK(Access::item_count(viewer) == 2);
  CHECK(Access::selected(viewer) == state(first)->id);

  const auto second_id = state(*second)->id;
  Access::select(viewer, second_id);
  CHECK(Access::selected(viewer) == second_id);

  la::Backtrack third {"List C", 128.0, 1};
  Access::refresh(viewer);
  CHECK(Access::item_count(viewer) == 3);
  CHECK(Access::selected(viewer) == second_id);

  // Destroying the selected buffer falls back to the first, in live mode.
  Access::set_view(viewer, 0.0, 1.0);
  second.reset();
  Access::refresh(viewer);
  CHECK(Access::item_count(viewer) == 2);
  CHECK(Access::selected(viewer) == state(first)->id);
  CHECK(Access::live(viewer));
}

void test_gui_paints_every_mode()
{
  const juce::Colour waveform {0xff7ed7a7};
  {
    la::detail::ViewerComponent empty;
    empty.setSize(900, 520);
    Access::refresh(empty);
    const auto image = empty.createComponentSnapshot(empty.getLocalBounds());
    CHECK(!contains_colour(image, waveform));
  }

  la::Backtrack capture {"Paint", 128.0, 2};
  std::vector<float> left(128 * 60);
  std::vector<float> right(left.size());
  for (std::size_t i = 0; i < left.size(); ++i)
  {
    left[i] = 0.8f * float(std::sin(double(i) * 0.05));
    right[i] = 0.5f * float(std::cos(double(i) * 0.02));
  }
  const std::array<std::span<const float>, 2> planar {left, right};
  capture.record(planar);

  la::detail::ViewerComponent viewer;
  viewer.setSize(900, 520);
  Access::select(viewer, state(capture)->id);
  for (const auto scale : {0.1, 0.5, 1.5, 4.0, 100.0, 1000.0})
  {
    Access::set_view(viewer, 1000.0, scale);
    const auto image = viewer.createComponentSnapshot(viewer.getLocalBounds());
    CHECK(contains_colour(image, waveform));
  }

  // Hovering at sample zoom draws a value tooltip next to the cursor.
  Access::set_view(viewer, 1000.0, 0.1);
  const auto bounds = Access::waveform_bounds(viewer);
  const juce::Point<float> cursor {float(bounds.getX() + 200), float(bounds.getY() + 70)};
  viewer.mouseExit(mouse_event(viewer, cursor, cursor));
  const auto without = viewer.createComponentSnapshot(viewer.getLocalBounds());
  viewer.mouseMove(mouse_event(viewer, cursor, cursor));
  const auto with = viewer.createComponentSnapshot(viewer.getLocalBounds());
  const auto box_x = int(cursor.x) + 14;
  const auto box_y = int(cursor.y) - 15;
  CHECK(with.getPixelAt(box_x, box_y) != without.getPixelAt(box_x, box_y));
}

void test_gui()
{
  juce::ScopedJuceInitialiser_GUI initialise_gui;
  test_gui_editor_lifetime();
  test_gui_window_respects_user_hide();
  test_gui_window_follows_editor_and_buffers();
  test_gui_switching_to_shorter_capture_keeps_live();
  test_gui_render_modes();
  test_gui_viewer_controls();
  test_gui_viewer_tracks_buffer_list();
  test_gui_paints_every_mode();
  juce::DeletedAtShutdown::deleteAll();
}

} // namespace

int main()
{
  CHECK(la_backtrack_odr_probe() == 64);
  CHECK(la_backtrack_odr_registry() == &la::detail::registry());
  CHECK(la_backtrack_odr_countdown() == &la::detail::allocation_fail_countdown());
  test_constructor();
  test_record_overloads();
  test_invalid_and_atomic_failure();
  test_allocation_failure_preserves_existing_audio();
  test_growth_beyond_reserve_is_geometric();
  test_summaries();
  test_peak_level_selection();
  test_in_progress_bucket_merges_lower_partials();
  test_reduced_peaks_from_samples();
  test_empty_block_is_noop();
  test_registry_and_names();
  test_snapshots_and_concurrency();
  test_snapshot_matches_brute_force();
  test_byte_accounting();
  test_concurrent_recording_lifecycle_and_snapshots();
  if (std::getenv("LA_BACKTRACK_SKIP_GUI_TEST") == nullptr)
    test_gui();
  test_viewport_math();
  test_zoom_scale();
  test_render_mode_selection();
  test_ruler_labels();
  test_peak_columns_are_visible();
  test_memory_status_helpers();

  if (failures != 0)
  {
    std::fprintf(stderr, "%d check(s) failed\n", failures);
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}
