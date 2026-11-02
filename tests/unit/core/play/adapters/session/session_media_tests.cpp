#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/play/adapters/session/session_case.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <vector>

namespace play_test {

using test_support::play::SessionRig;
namespace {

namespace play = libchiaking::play;
namespace controllers = libchiaking::controllers;
using libchiaking::media::MediaFailure;
using play::Outcome;
using FeedbackKind = controllers::ControllerFeedback::Kind;

class PlaySessionMedia : public SessionCase {
 protected:
  void SetUp() override {
    rig_.link.report_protocol_version(12);
    rig_.start_playing();
    rig_.link.describe(description());
    libchiaking::media::VideoFrame::Facts facts;
    facts.bytes = api_test::bytes_of({4, 5, 6, 7});
    facts.frame_index = 9;
    facts.frames_lost = 2;
    facts.recovered = true;
    rig_.link.show(libchiaking::media::VideoFrame{facts});
    rig_.link.lose_video();
    libchiaking::media::AudioUnit samples;
    samples.bytes = api_test::bytes_of({8, 9});
    rig_.link.sound(samples);
    libchiaking::media::AudioUnit format;
    format.kind = libchiaking::media::AudioUnit::Kind::format;
    format.format.channels = 6;
    format.format.rate = 48000;
    rig_.link.sound(format);
    rig_.link.feed_back({seat(2), controllers::Rumble{200, 100}});
    rig_.link.feed_back({seat(1), controllers::SeatConfirmed{{0, 0, 255}}});
    effects_.left.mode = 0x21;
    effects_.right.parameters[9] = 0x08;
    rig_.link.feed_back({seat(3), effects_});
    rig_.link.feed_back(
        {controllers::SeatNumber::first(), controllers::Lightbar{{1, 2, 3}}});
    rig_.link.feed_back(
        {controllers::SeatNumber::first(), controllers::PlayerLeds{2}});
    rig_.link.feed_back(
        {controllers::SeatNumber::first(),
         controllers::HapticIntensity{controllers::FeedbackIntensity::weak}});
    rig_.link.feed_back({controllers::SeatNumber::first(),
                         controllers::TriggerIntensity{
                             controllers::FeedbackIntensity::medium}});
    rig_.link.feed_back(
        {controllers::SeatNumber::first(), controllers::MotionReset{}});
    rig_.link.feed_back({seat(1), controllers::HapticAudio{pcm_}});
    SessionRig::eventually(
        [this] { return rig_.media.feedback().size() == 9; });
  }

  auto rig() -> SessionRig& { return rig_; }
  auto effects() const -> const controllers::TriggerEffects& {
    return effects_;
  }
  auto pcm() const -> const std::vector<std::byte>& { return pcm_; }

 private:
  SessionRig rig_;
  controllers::TriggerEffects effects_;
  std::vector<std::byte> pcm_ = api_test::bytes_of({1, 2, 3, 4, 5, 6, 7, 8});
};

TEST_F(PlaySessionMedia, DescriptionReachesTheApp) {
  const auto descriptions = rig().media.descriptions();
  ASSERT_EQ(descriptions.size(), 1U);
  EXPECT_EQ(descriptions.front(), description());
}

TEST_F(PlaySessionMedia, FramesReachTheAppPaddedAndProfiled) {
  const auto shown = rig().media.shown();
  ASSERT_EQ(shown.size(), 1U);
  EXPECT_EQ(shown.front().frame_index, 9U);
  EXPECT_EQ(shown.front().bytes, api_test::bytes_of({4, 5, 6, 7}));
  EXPECT_EQ(shown.front().padded,
            4 + libchiaking::media::VideoFrame::tail_padding);
  EXPECT_TRUE(shown.front().profile_changed);
  EXPECT_EQ(shown.front().header, api_test::bytes_of({1, 2, 3}));
  EXPECT_EQ(shown.front().width, 1280U);
  EXPECT_EQ(shown.front().height, 720U);
  EXPECT_FALSE(shown.front().hevc);
  EXPECT_EQ(rig().media.gaps(), 1U);
}

TEST_F(PlaySessionMedia, SoundsReachTheApp) {
  const auto sounds = rig().media.sounds();
  ASSERT_EQ(sounds.size(), 2U);
  EXPECT_EQ(sounds[0].kind, libchiaking::media::AudioUnit::Kind::samples);
  EXPECT_EQ(sounds[0].bytes, api_test::bytes_of({8, 9}));
  EXPECT_EQ(sounds[1].kind, libchiaking::media::AudioUnit::Kind::format);
  EXPECT_EQ(sounds[1].format.channels, 6U);
}

TEST_F(PlaySessionMedia, FeedbackReachesTheAppBySeat) {
  const auto feedback = rig().media.feedback();
  ASSERT_EQ(feedback.size(), 9U);
  EXPECT_EQ(feedback[0].kind(), FeedbackKind::rumble);
  EXPECT_EQ(feedback[0].seat, seat(2));
  ASSERT_NE(feedback[0].get<controllers::Rumble>(), nullptr);
  EXPECT_EQ(*feedback[0].get<controllers::Rumble>(),
            (controllers::Rumble{200, 100}));
  EXPECT_EQ(feedback[1].kind(), FeedbackKind::seat_confirmed);
  EXPECT_EQ(feedback[1].seat, seat(1));
  ASSERT_NE(feedback[1].get<controllers::SeatConfirmed>(), nullptr);
  EXPECT_EQ(feedback[1].get<controllers::SeatConfirmed>()->colour,
            (controllers::SeatLight{0, 0, 255}));
  EXPECT_EQ(feedback[2].seat, seat(3));
  ASSERT_NE(feedback[2].get<controllers::TriggerEffects>(), nullptr);
  EXPECT_EQ(*feedback[2].get<controllers::TriggerEffects>(), effects());
  ASSERT_NE(feedback[3].get<controllers::Lightbar>(), nullptr);
  EXPECT_EQ(feedback[3].get<controllers::Lightbar>()->colour,
            (controllers::SeatLight{1, 2, 3}));
  ASSERT_NE(feedback[4].get<controllers::PlayerLeds>(), nullptr);
  EXPECT_EQ(feedback[4].get<controllers::PlayerLeds>()->player, 2U);
  ASSERT_NE(feedback[5].get<controllers::HapticIntensity>(), nullptr);
  EXPECT_EQ(feedback[5].get<controllers::HapticIntensity>()->level,
            controllers::FeedbackIntensity::weak);
  ASSERT_NE(feedback[6].get<controllers::TriggerIntensity>(), nullptr);
  EXPECT_EQ(feedback[6].get<controllers::TriggerIntensity>()->level,
            controllers::FeedbackIntensity::medium);
  EXPECT_EQ(feedback[7].kind(), FeedbackKind::motion_reset);
  EXPECT_EQ(feedback[8].kind(), FeedbackKind::haptics);
  EXPECT_EQ(feedback[8].seat, seat(1));
  EXPECT_EQ(rig().media.haptic_audio(), pcm());
}

TEST_F(PlaySessionMedia, ArrivesOnTheThreadThatSendsEvents) {
  EXPECT_EQ(rig().media.threads(), rig().events.threads());
}

TEST_F(PlaySessionMedia, ReportsPresentedLossOnce) {
  const auto loss = rig().session->video_presented();
  ASSERT_TRUE(loss);
  EXPECT_EQ(loss->frames_lost, 2U);
  EXPECT_TRUE(loss->recovered);
  const auto again = rig().session->video_presented();
  ASSERT_TRUE(again);
  EXPECT_EQ(again->frames_lost, 0U);
}

TEST_F(PlaySessionMedia, StopsAnsweringPresentationsOnceLeaving) {
  ASSERT_TRUE(rig().session->video_presented());
  EXPECT_TRUE(rig().session->leave());
  EXPECT_FALSE(rig().session->video_presented());
}

TEST_F(PlaySessionMedia, CountsWhatArrivedInTheStatistics) {
  SessionRig::eventually(
      [this] { return rig().session->statistics().protocol_version == 12U; });
  const auto statistics = rig().session->statistics();
  EXPECT_EQ(statistics.video.units, 1U);
  EXPECT_EQ(statistics.video.bytes, 4U);
  EXPECT_EQ(statistics.video.lost, 2U);
  EXPECT_EQ(statistics.video.recovered, 1U);
  EXPECT_EQ(statistics.controller_pending, 0U);
}

TEST_F(PlaySessionMedia, PassesKeyframeRequestsAndDecoderRejectionsOn) {
  EXPECT_TRUE(rig().session->request_keyframe());
  SessionRig::eventually(
      [this] { return rig().link.keyframe_requests() >= 1; });
  rig().session->video_rejected(9, 4);
  SessionRig::eventually(
      [this] { return rig().link.count("video_loss") == 1; });
  EXPECT_EQ(rig().session->statistics().video.decoder_rejections, 1U);
  EXPECT_EQ(rig().session->statistics().video.last_decoder_error, 4);
}

TEST_F(PlaySessionMedia, EndsWhenTheDecoderOutputFails) {
  rig().session->video_output_failed(7);
  const auto& outcome = rig().session->wait_until_ended();
  EXPECT_EQ(outcome.ending(), Outcome::Ending::media_failed);
  EXPECT_TRUE(ended_as(outcome, MediaFailure::decoder_output));
  ASSERT_TRUE(outcome.media_fault());
  EXPECT_EQ(outcome.media_fault()->kind, MediaFailure::decoder_output);
  EXPECT_EQ(outcome.media_fault()->cause, 7);
  EXPECT_EQ(outcome.media_fault()->stage, "Video decoder output");
  EXPECT_FALSE(rig().session->request_keyframe());
}

using PlaySessionDecoder = SessionCase;

TEST_F(PlaySessionDecoder, EndsTheSessionWhenItRefusesAFrame) {
  SessionRig rig;
  rig.media.answer({libchiaking::media::VideoVerdict::submit_failed, 11});
  rig.start_playing();
  rig.link.describe(description());
  libchiaking::media::VideoFrame::Facts facts;
  facts.bytes = api_test::bytes_of({1});
  rig.link.show(libchiaking::media::VideoFrame{facts});
  const auto& outcome = rig.session->wait_until_ended();
  EXPECT_TRUE(ended_as(outcome, MediaFailure::decoder_submission));
  ASSERT_TRUE(outcome.media_fault());
  EXPECT_EQ(outcome.media_fault()->cause, 11);
  EXPECT_EQ(outcome.media_fault()->kind, MediaFailure::decoder_submission);
}

}  // namespace
}  // namespace play_test
