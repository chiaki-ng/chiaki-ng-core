#pragma once

#include "libchiaking/platform/runtime/clock_provider.hpp"
#include "libchiaking/play_types.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace kakusan_test {

class TrackedClock final : public libchiaking::platform::ClockProvider {
 public:
  explicit TrackedClock(const std::uint64_t& now) noexcept : now_(&now) {}

  std::uint64_t now_microseconds() const noexcept override { return *now_; }

 private:
  const std::uint64_t* now_;
};

class ForwardedMedia final : public libchiaking::media::MediaSink {
 public:
  void on_stream_description(
      const libchiaking::media::StreamDescription& description) override {
    descriptions.push_back(description);
  }

  auto on_video(const libchiaking::media::VideoUnit& unit)
      -> libchiaking::media::VideoResult override {
    const auto bytes = unit.frame().bytes();
    auto& kept = unit.frame().profile_header() ? headers : video;
    kept.emplace_back(bytes.begin(), bytes.end());
    return {};
  }

  void on_video_gap() override { ++gaps; }

  auto on_audio(const libchiaking::media::AudioUnit& unit)
      -> libchiaking::media::AudioResult override {
    if (unit.kind == libchiaking::media::AudioUnit::Kind::samples)
      audio.push_back(unit.bytes);
    return {};
  }

  void on_feedback(
      const libchiaking::controllers::ControllerFeedback& feedback) override {
    feedback_seen.push_back(feedback);
  }

  auto empty() const noexcept -> bool { return audio.empty() && video.empty(); }

  std::vector<libchiaking::media::StreamDescription> descriptions;
  std::vector<std::vector<std::byte>> audio;
  std::vector<std::vector<std::byte>> video;
  std::vector<std::vector<std::byte>> headers;
  std::vector<libchiaking::controllers::ControllerFeedback> feedback_seen;
  std::size_t gaps{};
};

}  // namespace kakusan_test
