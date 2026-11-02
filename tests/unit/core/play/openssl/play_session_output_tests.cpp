#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/play/openssl/household.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <cstddef>
#include <memory>
#include <string_view>

namespace play_test {

using test_support::LiveProviders;
using test_support::play::SessionRig;
using test_support::play::SignedInPlayer;
namespace {

namespace media = libchiaking::media;
namespace play = libchiaking::play;
using play::SessionState;
using play::Where;

class DeafOutput final : public media::MediaSink {
 public:
  void on_stream_description(const media::StreamDescription&) override {
    ++described;
  }
  auto on_video(const media::VideoUnit&) -> media::VideoResult override {
    return {};
  }
  void on_video_gap() override {}
  auto on_audio(const media::AudioUnit&) -> media::AudioResult override {
    ++sounds;
    return {media::AudioVerdict::failed, 9};
  }
  void on_feedback(
      const libchiaking::controllers::ControllerFeedback&) override {}
  auto decoder_name() const -> std::string_view override {
    return "VideoToolbox";
  }

  std::atomic<int> described{};
  std::atomic<int> sounds{};
};

class PlaySessionOutput : public ::testing::Test {
 protected:
  void SetUp() override {
    home_.play.journal_sessions_in(journal_);
    description_.audio.channels = 2;
    description_.audio.rate = 48000;
    description_.profiles.push_back({1280, 720, {std::byte{1}}});
    strict_.output = play::OutputRequest::create(1024, true).value();
  }

  void play_tolerantly() {
    play::testing::ScriptedLink tolerant{LiveProviders::threading(), LiveProviders::steady()};
    tolerant.connect();
    DeafOutput deaf;
    RecordedEvents events;
    const auto first = home_.play.start_scripted(player_, Where::home, tolerant,
                                                 {}, deaf, events);
    ASSERT_TRUE(first);
    EXPECT_EQ(first.value()->journal(), journal_);
    events.until_state(SessionState::playing);
    tolerant.describe(description_);
    tolerant.sound(media::AudioUnit{});
    tolerant.sound(media::AudioUnit{});
    SessionRig::eventually([&] { return deaf.sounds == 2; });
    EXPECT_EQ(first.value()->state(), SessionState::playing);
    EXPECT_EQ(first.value()->statistics().audio_failures, 2U);
    EXPECT_TRUE(first.value()->leave());
    EXPECT_TRUE(first.value()->wait_until_ended().was_left());
  }

  void outgrow_a_strict_output() {
    play::testing::ScriptedLink large{LiveProviders::threading(), LiveProviders::steady()};
    large.connect();
    RecordedEvents refused_events;
    DeafOutput unseen;
    const auto third = home_.play.start_scripted(
        player_, Where::home, large, strict_, unseen, refused_events);
    ASSERT_TRUE(third);
    refused_events.until_state(SessionState::playing);
    large.describe(description_);
    const auto& oversize = third.value()->wait_until_ended();
    EXPECT_EQ(oversize.ending(), play::Outcome::Ending::media_failed);
    ASSERT_TRUE(oversize.media_fault());
    EXPECT_EQ(oversize.media_fault()->kind,
              media::MediaFailure::profile_validation);
  }

  auto negotiated() -> std::size_t {
    std::size_t negotiated = 0;
    while (const auto entry = journal_->read())
      negotiated += entry->message().starts_with(
                        "stream negotiated: 1 video profiles (1280x720)")
                        ? 1U
                        : 0U;
    return negotiated;
  }

  Household home_;
  std::shared_ptr<libchiaking::users::PsUser> player_ = SignedInPlayer::fresh();
  std::shared_ptr<libchiaking::diagnostics::SessionJournal> journal_ =
      std::make_shared<libchiaking::diagnostics::SessionJournal>(LiveProviders::threading(),
                                                                 LiveProviders::steady(), 1);
  media::StreamDescription description_;
  play::Preferences strict_;
};

TEST_F(PlaySessionOutput, UsesTheDefaultControllerAndSeatDepths) {
  EXPECT_EQ(play::Session::Core::core_settings().controller_depth,
            play::Controllers::default_depth);
  EXPECT_EQ(play::Session::Core::core_settings().seat_depth,
            play::Seats::default_depth);
}

TEST_F(PlaySessionOutput, KeepsPlayingThroughAudioFailuresByDefault) {
  play_tolerantly();
}

TEST_F(PlaySessionOutput, EndsWhenAStrictOutputFailsToPlayAudio) {
  play::testing::ScriptedLink ending{LiveProviders::threading(), LiveProviders::steady()};
  ending.connect();
  RecordedEvents events;
  DeafOutput output;
  const auto second = home_.play.start_scripted(player_, Where::home, ending,
                                                strict_, output, events);
  ASSERT_TRUE(second);
  EXPECT_EQ(second.value()->journal(), journal_);
  events.until_state(SessionState::playing);
  ending.sound(media::AudioUnit{});
  const auto& outcome = second.value()->wait_until_ended();
  EXPECT_EQ(outcome.ending(), play::Outcome::Ending::media_failed);
  ASSERT_TRUE(outcome.media_fault());
  EXPECT_EQ(outcome.media_fault()->kind, media::MediaFailure::audio_output);
  EXPECT_EQ(outcome.media_fault()->cause, 9);
  EXPECT_EQ(output.sounds, 1);
}

TEST_F(PlaySessionOutput, EndsWhenAStreamOutgrowsAStrictOutput) {
  outgrow_a_strict_output();
}

TEST_F(PlaySessionOutput, JournalsOnlyTheStreamThatWasNegotiated) {
  ASSERT_NO_FATAL_FAILURE(play_tolerantly());
  ASSERT_NO_FATAL_FAILURE(outgrow_a_strict_output());
  EXPECT_EQ(negotiated(), 1U);
}

TEST_F(PlaySessionOutput, GivesASessionItsOwnJournalWhenNoneIsShared) {
  home_.play.journal_sessions_in(nullptr);
  play::testing::ScriptedLink own{LiveProviders::threading(), LiveProviders::steady()};
  own.connect();
  RecordedEvents own_events;
  RecordedMedia own_media;
  const auto fourth = home_.play.start_scripted(player_, Where::home, own, {},
                                                own_media, own_events);
  ASSERT_TRUE(fourth);
  EXPECT_TRUE(fourth.value()->journal());
  EXPECT_NE(fourth.value()->journal(), journal_);
  EXPECT_TRUE(fourth.value()->leave());
  EXPECT_TRUE(fourth.value()->wait_until_ended().was_left());
}

TEST_F(PlaySessionOutput, NamesTheAppsDecoderWhenItsOutputFails) {
  play::testing::ScriptedLink named{LiveProviders::threading(), LiveProviders::steady()};
  named.connect();
  RecordedEvents named_events;
  DeafOutput toolbox;
  const auto fifth = home_.play.start_scripted(player_, Where::home, named, {},
                                               toolbox, named_events);
  ASSERT_TRUE(fifth);
  named_events.until_state(SessionState::playing);
  fifth.value()->video_output_failed(7);
  const auto& failed = fifth.value()->wait_until_ended();
  ASSERT_TRUE(failed.media_fault());
  EXPECT_EQ(failed.media_fault()->kind, media::MediaFailure::decoder_output);
  EXPECT_EQ(failed.media_fault()->cause, 7);
  EXPECT_EQ(failed.media_fault()->stage, "VideoToolbox output");
}

TEST(MediaSink, NamesAnUnnamedDecoderVideoDecoder) {
  EXPECT_EQ(RecordedMedia{}.decoder_name(), "Video decoder");
  EXPECT_EQ(media::MediaSink::default_decoder_name, "Video decoder");
}

}  // namespace
}  // namespace play_test
