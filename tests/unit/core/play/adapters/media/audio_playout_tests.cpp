#include <libchiaking/play_types.hpp>

#include "tests/support/live_providers.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <numeric>
#include <optional>
#include <vector>

namespace play_test {
namespace {

using libchiaking::media::AudioPlayout;
using test_support::LiveProviders;

class AudioPlayoutTest : public ::testing::Test {
 protected:
  static constexpr std::uint32_t channels = 2;
  static constexpr std::uint32_t rate = 48000;

  static auto make(std::uint32_t target_ms = 40) -> AudioPlayout {
    auto playout = AudioPlayout::create(channels, rate, target_ms);
    EXPECT_TRUE(playout.has_value());
    return std::move(*playout);
  }

  static auto counting(std::size_t frames, std::int16_t first = 1)
      -> std::vector<std::int16_t> {
    std::vector<std::int16_t> samples(frames * channels);
    std::iota(samples.begin(), samples.end(), first);
    return samples;
  }

  static void push(AudioPlayout& playout, std::size_t frames) {
    const auto samples = counting(frames);
    playout.push(samples);
  }

  static auto pull(AudioPlayout& playout, std::size_t frames) -> std::size_t {
    std::vector<std::int16_t> samples(frames * channels, 7);
    return playout.pull(samples);
  }

  static auto pull_and_refill(AudioPlayout& playout, std::size_t frames,
                              int rounds) -> bool {
    for (int round = 0; round < rounds; ++round) {
      if (pull(playout, frames) != frames) {
        return false;
      }
      push(playout, frames);
    }
    return true;
  }
};

TEST_F(AudioPlayoutTest, RefusesUnusableFormats) {
  EXPECT_FALSE(AudioPlayout::create(0, rate, 40));
  EXPECT_FALSE(AudioPlayout::create(AudioPlayout::max_channels + 1, rate, 40));
  EXPECT_FALSE(AudioPlayout::create(channels, 0, 40));
  const auto clamped = AudioPlayout::create(channels, rate, 1);
  ASSERT_TRUE(clamped);
  EXPECT_EQ(clamped->target_frames(),
            rate * AudioPlayout::min_target_ms / 1000);
}

TEST_F(AudioPlayoutTest, PlaysPushedAudioInOrderInWholeBlocks) {
  auto playout = make();
  const auto samples = counting(1000);
  playout.push(samples);
  EXPECT_EQ(playout.buffered_frames(), 960U);
  std::vector<std::int16_t> pulled(700 * channels);
  EXPECT_EQ(playout.pull(pulled), 700U);
  EXPECT_TRUE(std::equal(pulled.begin(), pulled.end(), samples.begin()));
  EXPECT_EQ(playout.pull(pulled), 260U);
  EXPECT_TRUE(std::equal(pulled.begin(), pulled.begin() + 260 * channels,
                         samples.begin() + 700 * channels));
  EXPECT_TRUE(std::all_of(pulled.begin() + 260 * channels, pulled.end(),
                          [](std::int16_t sample) { return sample == 0; }));
  const auto rest = counting(200, 9000);
  playout.push(rest);
  EXPECT_EQ(playout.pull(pulled), 240U);
  EXPECT_TRUE(std::equal(pulled.begin(), pulled.begin() + 40 * channels,
                         samples.begin() + 960 * channels));
  EXPECT_TRUE(std::equal(pulled.begin() + 40 * channels,
                         pulled.begin() + 240 * channels, rest.begin()));
  const auto stats = playout.statistics();
  EXPECT_EQ(stats.pushed_frames, 1200U);
  EXPECT_EQ(stats.underruns, 2U);
  EXPECT_EQ(stats.silent_frames, 440U + 460U);
  EXPECT_EQ(stats.buffered_frames, 0U);
  EXPECT_EQ(stats.maximum_buffered_frames, 960U);
}

TEST_F(AudioPlayoutTest, ShortPacketsFillTheWholeBufferAndStillTrim) {
  auto playout = make();
  for (int packet = 0; packet < 48; ++packet) {
    push(playout, 120);
  }
  EXPECT_EQ(playout.buffered_frames(), 5760U);
  EXPECT_GE(playout.buffered_frames(), playout.target_frames());
  for (int round = 0; round < 200; ++round) {
    ASSERT_EQ(pull(playout, 120), 120U);
    push(playout, 120);
  }
  EXPECT_EQ(playout.statistics().trims, 1U);
  EXPECT_EQ(playout.statistics().overflows, 0U);
}

TEST_F(AudioPlayoutTest, DropsTheNewestAudioWhenFull) {
  auto playout = make();
  push(playout, 5760);
  push(playout, 480);
  const auto stats = playout.statistics();
  EXPECT_EQ(stats.buffered_frames, 5760U);
  EXPECT_EQ(stats.overflows, 1U);
  EXPECT_EQ(stats.dropped_frames, 480U);
  EXPECT_EQ(stats.pushed_frames, 6240U);
  std::vector<std::int16_t> pulled(channels);
  EXPECT_EQ(playout.pull(pulled), 1U);
  EXPECT_EQ(pulled[0], 1);
}

TEST_F(AudioPlayoutTest,
       SkipsToTheTargetOnceTheAudioLeftStaysHighForHalfASecond) {
  auto playout = make();
  push(playout, 5760);
  ASSERT_TRUE(pull_and_refill(playout, 480, 49));
  EXPECT_EQ(playout.statistics().trims, 0U);
  EXPECT_EQ(playout.buffered_frames(), 5760U);
  EXPECT_EQ(pull(playout, 480), 480U);
  const auto stats = playout.statistics();
  EXPECT_EQ(stats.trims, 1U);
  EXPECT_EQ(stats.trimmed_frames, 3360U);
  EXPECT_EQ(stats.buffered_frames, 1920U);
  EXPECT_EQ(stats.underruns, 0U);
}

TEST_F(AudioPlayoutTest, ADipBelowTheLineHoldsTheTrimOff) {
  auto playout = make();
  push(playout, 5760);
  ASSERT_TRUE(pull_and_refill(playout, 480, 24));
  EXPECT_EQ(pull(playout, 2000), 2000U);
  push(playout, 2000);
  ASSERT_TRUE(pull_and_refill(playout, 480, 22));
  EXPECT_EQ(playout.statistics().trims, 0U);
  ASSERT_TRUE(pull_and_refill(playout, 480, 50));
  EXPECT_EQ(playout.statistics().trims, 1U);
}

TEST_F(AudioPlayoutTest, LargeRenderQuantaNeverTrim) {
  auto playout = make();
  push(playout, 5096);
  ASSERT_TRUE(pull_and_refill(playout, 4096, 12));
  const auto stats = playout.statistics();
  EXPECT_EQ(stats.trims, 0U);
  EXPECT_EQ(stats.underruns, 0U);
}

TEST_F(AudioPlayoutTest, APartlyPulledBlockLeavesRoomForTheNextAtASmallTarget) {
  auto made = AudioPlayout::create(channels, 8000, 10);
  ASSERT_TRUE(made);
  auto playout = std::move(*made);
  push(playout, 240);
  EXPECT_EQ(pull(playout, 80), 80U);
  push(playout, 240);
  EXPECT_EQ(playout.statistics().overflows, 0U);
  EXPECT_EQ(playout.buffered_frames(), 400U);
}

TEST_F(AudioPlayoutTest, AConcurrentProducerAndConsumerKeepTheAudioInOrder) {
  auto playout = make(100);
  constexpr std::size_t packets = 50;
  constexpr std::size_t packet_frames = 480;
  std::atomic<bool> done{false};
  auto producer = LiveProviders::spawn("producer", [&] {
    for (std::size_t packet = 0; packet < packets; ++packet) {
      std::vector<std::int16_t> samples(packet_frames * channels);
      for (std::size_t frame = 0; frame < packet_frames; ++frame) {
        const auto id =
            static_cast<std::int16_t>(packet * packet_frames + frame + 1);
        samples[frame * channels] = id;
        samples[frame * channels + 1] = id;
      }
      playout.push(samples);
      LiveProviders::nap(std::chrono::microseconds{500});
    }
    done.store(true, std::memory_order_release);
  });
  std::vector<std::int16_t> pulled(256 * channels);
  std::int16_t last = 0;
  bool ordered = true;
  std::uint64_t received = 0;
  int quiet = 0;
  while (quiet < 3) {
    const auto frames = playout.pull(pulled);
    for (std::size_t frame = 0; frame < frames; ++frame) {
      const auto id = pulled[frame * channels];
      ordered = ordered && id > last && pulled[frame * channels + 1] == id;
      last = id;
    }
    received += frames;
    if (frames == 0 && done.load(std::memory_order_acquire)) {
      ++quiet;
    }
    LiveProviders::nap(std::chrono::microseconds{200});
  }
  static_cast<void>(producer.join());
  EXPECT_TRUE(ordered);
  const auto stats = playout.statistics();
  const auto accounted = received + stats.dropped_frames +
                         stats.trimmed_frames + stats.buffered_frames;
  EXPECT_EQ(stats.pushed_frames, packets * packet_frames);
  EXPECT_LE(accounted, stats.pushed_frames);
  EXPECT_LT(stats.pushed_frames - accounted, AudioPlayout::block_frames);
  EXPECT_LE(stats.maximum_buffered_frames, playout.target_frames() * 3);
}

}  // namespace
}  // namespace play_test
