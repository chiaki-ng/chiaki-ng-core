#include <libchiaking/play.hpp>
#include <libchiaking/play_types.hpp>

#include "tests/support/enum_printing.hpp"

#include <gtest/gtest.h>

#include <cstddef>

namespace play_test {
namespace {

namespace controllers = libchiaking::controllers;
namespace media = libchiaking::media;

class MediaSink : public ::testing::Test {
 protected:
  class CountingMedia final : public media::MediaSink {
   public:
    void on_stream_description(
        const media::StreamDescription& description) override {
      profiles_ = description.profiles.size();
    }
    auto on_video(const media::VideoUnit&) -> media::VideoResult override {
      return {media::VideoVerdict::presented, 0};
    }
    void on_video_gap() override { ++gaps_; }
    auto on_audio(const media::AudioUnit& unit) -> media::AudioResult override {
      return {unit.raw_pcm ? media::AudioVerdict::failed
                           : media::AudioVerdict::accepted,
              0};
    }
    void on_feedback(const controllers::ControllerFeedback& feedback) override {
      last_seat_ = feedback.seat;
    }

    auto profiles() const -> std::size_t { return profiles_; }
    auto gaps() const -> int { return gaps_; }
    auto last_seat() const -> controllers::SeatNumber { return last_seat_; }

   private:
    std::size_t profiles_{};
    int gaps_{};
    controllers::SeatNumber last_seat_{};
  };

  static auto described() -> media::StreamDescription {
    media::StreamDescription description;
    description.profiles.push_back({1920, 1080, {std::byte{1}}});
    description.picture =
        media::GrantedPicture{media::VideoCodec::h265, 1920, 1080, 60, 15000};
    return description;
  }

  static auto rumbled() -> controllers::ControllerFeedback {
    return {*controllers::SeatNumber::create(2), controllers::Rumble{3, 4}};
  }

  CountingMedia counting;
  media::MediaSink& sink = counting;
};

TEST_F(MediaSink, CanBeImplementedByTheApp) {
  const auto description = described();
  sink.on_stream_description(description);
  sink.on_video_gap();
  EXPECT_EQ(counting.profiles(), 1u);
  EXPECT_EQ(counting.gaps(), 1);
  EXPECT_EQ(sink.on_audio(media::AudioUnit{}).verdict,
            media::AudioVerdict::accepted);
  EXPECT_EQ(sink.on_audio(media::AudioUnit{.raw_pcm = true}).verdict,
            media::AudioVerdict::failed);
  sink.on_feedback(rumbled());
  EXPECT_EQ(counting.last_seat().index(), 2u);
  EXPECT_EQ(description, description);
  EXPECT_EQ(media::AudioFormat{}, media::AudioFormat{});
}

TEST_F(MediaSink, ReceivesRumbleFeedback) {
  const auto feedback = rumbled();
  EXPECT_EQ(feedback.kind(), controllers::ControllerFeedback::Kind::rumble);
  ASSERT_TRUE(feedback.get<controllers::Rumble>());
  EXPECT_EQ(feedback.get<controllers::Rumble>()->right, 4);
  EXPECT_FALSE(feedback.get<controllers::HapticAudio>());
  EXPECT_EQ(controllers::ControllerFeedback{}.kind(),
            controllers::ControllerFeedback::Kind::rumble);
}

TEST_F(MediaSink, HoldsFeedbackInAnInbox) {
  libchiaking::platform::DefaultThreadingProvider threading;
  const libchiaking::platform::SteadyClock steady;
  controllers::FeedbackInbox inbox{threading, steady};
  EXPECT_EQ(inbox.accept(rumbled()),
            controllers::FeedbackInbox::Admission::held);
  EXPECT_EQ(controllers::HapticBlock::size, 256u);
  EXPECT_EQ(controllers::TriggerEffects::packed_size, 22u);
}

}  // namespace
}  // namespace play_test
