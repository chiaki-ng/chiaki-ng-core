#pragma once

#include "libchiaking/play_types.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace libchiaking::testing {

class RecordingMedia final : public libchiaking::media::MediaSink {
 public:
  void on_stream_description(
      const libchiaking::media::StreamDescription& description) override {
    profiles = description.profiles.size();
    ++infos;
  }

  auto on_video(const libchiaking::media::VideoUnit& unit)
      -> libchiaking::media::VideoResult override {
    ++videos;
    headers.emplace_back(unit.profile_header().begin(),
                         unit.profile_header().end());
    hevc = unit.hevc();
    frames.push_back(unit.frame().frame_index());
    return verdict;
  }

  void on_video_gap() override { ++gaps; }
  auto on_audio(const libchiaking::media::AudioUnit& unit)
      -> libchiaking::media::AudioResult override {
    audio.push_back(unit.kind);
    return audio_verdict;
  }
  void on_feedback(
      const libchiaking::controllers::ControllerFeedback& fact) override {
    feedback.push_back(fact.kind());
  }

  libchiaking::media::VideoResult verdict{};
  libchiaking::media::AudioResult audio_verdict{};
  std::size_t profiles{};
  int infos{};
  int videos{};
  int gaps{};
  bool hevc{};
  std::vector<std::vector<std::byte>> headers;
  std::vector<std::uint16_t> frames;
  std::vector<libchiaking::media::AudioUnit::Kind> audio;
  std::vector<libchiaking::controllers::ControllerFeedback::Kind> feedback;
};

}  // namespace libchiaking::testing
