#include "src/core/play/adapters/links/link_stages.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/play/adapters/session/session_engine_rig.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <format>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

namespace play_test {

using test_support::LiveProviders;
using test_support::play::RecordedEvents;
using test_support::play::SignedInPlayer;
namespace {

namespace play = libchiaking::play;
namespace remote_play = libchiaking::net::protocol::remote_play;
using namespace std::chrono_literals;
using libchiaking::media::MediaFailure;
using libchiaking::media::MediaFault;
using libchiaking::media::VideoVerdict;
using libchiaking::testing::ScriptedSession;
using Cause = play::EndReason::Cause;
using Ending = play::Outcome::Ending;
using StreamError = remote_play::StreamError;
using StreamState = remote_play::StreamState;

TEST(SessionEngineMedia, EndsOnAFailedDecoderFormat) {
  SessionEngineRig decoder;
  decoder.media.verdict = {VideoVerdict::format_failed, -777};
  decoder.streaming();
  decoder.then([&decoder](ScriptedSession& s) {
    s.emit(api_test::stream_info());
    s.emit(api_test::video_media(1));
    decoder.runner->video_output_failed(9);
    s.fail_cleanup(SessionEngineRig::failure(
        StreamError::shutdown_failed, remote_play::FailureStage::shutdown));
  });
  const auto outcome = decoder.run();
  EXPECT_EQ(outcome.ending(), Ending::media_failed);
  EXPECT_TRUE(ended_as(outcome, MediaFailure::decoder_submission));
  EXPECT_EQ(outcome.media_fault(),
            (MediaFault{MediaFailure::decoder_submission,
                        "Video decoder format setup", -777}));
  ASSERT_NE(outcome.cleanup(), nullptr);
  EXPECT_EQ(outcome.cleanup()->cause(), Cause::shutdown_failed);
  EXPECT_EQ(outcome.cleanup()->step(), play::EndReason::Step::shutdown);
  EXPECT_EQ(decoder.session->pumps, 2);
  EXPECT_EQ(decoder.logged("media failure: kind=decoder_submission cause=-777"),
            1U);
  EXPECT_EQ(decoder.logged("session cleanup failed: "), 1U);
  EXPECT_EQ(decoder.logged("session ended: "), 0U);
  EXPECT_EQ(decoder.logged(std::string{play::LinkStages::name(
                               remote_play::FailureStage::shutdown)} +
                           " — " +
                           std::string{SessionEngineRig::failure(
                                           StreamError::shutdown_failed,
                                           remote_play::FailureStage::shutdown)
                                           .describe()}),
            1U);
}

TEST(SessionEngineMedia, EndsOnAFailedSubmission) {
  SessionEngineRig submit;
  submit.media.verdict = {VideoVerdict::submit_failed, 4};
  submit.streaming();
  submit.then([](ScriptedSession& s) {
    s.emit(api_test::stream_info());
    s.emit(api_test::video_media(1));
  });
  const auto outcome = submit.run();
  EXPECT_TRUE(ended_as(outcome, MediaFailure::decoder_submission));
  EXPECT_EQ(outcome.cleanup(), nullptr);
  EXPECT_EQ(outcome.media_fault(), (MediaFault{MediaFailure::decoder_submission,
                                               "Video decoder submission", 4}));
  EXPECT_EQ(submit.logged("media failure: kind=decoder_submission cause=4"),
            1U);
}

TEST(SessionEngineMedia, EndsOnAFrameBeforeAnyDescription) {
  SessionEngineRig metadata;
  metadata.streaming();
  metadata.then([](ScriptedSession& s) { s.emit(api_test::video_media(1)); });
  const auto outcome = metadata.run();
  EXPECT_TRUE(ended_as(outcome, MediaFailure::frame_validation));
  EXPECT_EQ(metadata.media.videos, 0);
  EXPECT_EQ(outcome.media_fault(),
            (MediaFault{MediaFailure::frame_validation,
                        "Video frame metadata validation", 1}));
  EXPECT_EQ(metadata.logged("media failure: kind=frame_validation cause=1"),
            1U);
}

TEST(SessionEngineMedia, EndsOnTooManyProfiles) {
  SessionEngineRig profiles;
  profiles.streaming();
  profiles.then([](ScriptedSession& s) { s.emit(api_test::stream_info(9)); });
  const auto outcome = profiles.run();
  EXPECT_TRUE(ended_as(outcome, MediaFailure::profile_validation));
  EXPECT_EQ(profiles.media.infos, 1);
  EXPECT_EQ(outcome.media_fault(),
            (MediaFault{MediaFailure::profile_validation,
                        "Video stream profile validation", 1}));
  EXPECT_EQ(profiles.logged("media failure: kind=profile_validation cause=1"),
            1U);
}

TEST(SessionEngineMedia, EndsWhenARecoveryRequestFails) {
  SessionEngineRig recovery;
  recovery.session->loss_result =
      libchiaking::unexpected(StreamError::invalid_state);
  recovery.media.verdict = {VideoVerdict::recover, -1};
  recovery.streaming();
  recovery.then([](ScriptedSession& s) {
    s.emit(api_test::stream_info());
    s.emit(api_test::video_media(1));
  });
  const auto outcome = recovery.run();
  EXPECT_TRUE(ended_as(outcome, MediaFailure::recovery_request));
  EXPECT_EQ(
      outcome.media_fault(),
      (MediaFault{MediaFailure::recovery_request, "Video recovery request",
                  std::to_underlying(Cause::invalid_state)}));
  EXPECT_EQ(recovery.logged(
                std::format("media failure: kind=recovery_request cause={}",
                            std::to_underlying(Cause::invalid_state))),
            1U);
}

TEST(SessionEngineMedia, EndsWhenTheDecoderOutputFails) {
  SessionEngineRig output;
  output.streaming();
  output.then(
      [&output](ScriptedSession&) { output.runner->video_output_failed(55); });
  const auto outcome = output.run();
  EXPECT_TRUE(ended_as(outcome, MediaFailure::decoder_output));
  EXPECT_EQ(outcome.media_fault(), (MediaFault{MediaFailure::decoder_output,
                                               "Video decoder output", 55}));
  EXPECT_EQ(output.logged("media failure: kind=decoder_output cause=55"), 1U);
}

TEST(SessionEngineMedia, YieldsToALinkFailureThatCameFirst) {
  SessionEngineRig core_first;
  core_first.streaming();
  core_first.then([&core_first](ScriptedSession& s) {
    s.fail(SessionEngineRig::failure(StreamError::route_failed));
    core_first.runner->video_output_failed(1);
  });
  const auto outcome = core_first.run();
  EXPECT_TRUE(ended_as(outcome, Ending::session_failed, Cause::route_failed));
  EXPECT_FALSE(outcome.media_fault());
}

TEST(SessionEngineMedia, ToleratesAudioFailuresByDefault) {
  SessionEngineRig tolerant;
  tolerant.media.audio_verdict = {libchiaking::media::AudioVerdict::failed,
                                  -61};
  tolerant.streaming();
  tolerant.then([](ScriptedSession& s) {
    for (int index = 0; index < 3; ++index)
      s.emit(remote_play::event::EncodedMedia{
          remote_play::EncodedMediaKind::audio, {}});
  });
  tolerant.then([](ScriptedSession&) {});
  const auto outcome = tolerant.run();
  EXPECT_TRUE(outcome.succeeded());
  EXPECT_FALSE(outcome.media_fault());
  EXPECT_EQ(tolerant.media.audio.size(), 3U);
  EXPECT_EQ(tolerant.runner->statistics().audio_failures, 3U);
  EXPECT_EQ(tolerant.logged("audio output failure: cause=-61 failures="), 2U);
  EXPECT_EQ(tolerant.logged("media failure"), 0U);
}

TEST(SessionEngineMedia, EndsOnAnAudioFailureWhenAsked) {
  play::SessionSettings strict;
  strict.audio_failure_ends_run = true;
  SessionEngineRig ending{play::Venue::home(), strict};
  ending.media.audio_verdict = {libchiaking::media::AudioVerdict::failed, -61};
  ending.streaming();
  ending.then([](ScriptedSession& s) {
    s.emit(remote_play::event::EncodedMedia{
        remote_play::EncodedMediaKind::audio, {}});
  });
  ending.then([](ScriptedSession&) {});
  const auto outcome = ending.run();
  EXPECT_EQ(outcome.ending(), Ending::media_failed);
  EXPECT_TRUE(ended_as(outcome, MediaFailure::audio_output));
  EXPECT_EQ(outcome.media_fault(),
            (MediaFault{MediaFailure::audio_output, "Audio output", -61}));
  EXPECT_EQ(ending.session->pumps, 2);
  EXPECT_EQ(ending.logged("media failure: kind=audio_output cause=-61"), 1U);
}

TEST(SessionEngineMedia, ReportsADroppedFrameAndAsksForAKeyframe) {
  SessionEngineRig h;
  h.streaming();
  h.then([&h](ScriptedSession& s) {
    s.emit(api_test::stream_info());
    h.media.verdict = {VideoVerdict::dropped, 0};
    s.emit(api_test::video_media(5));
  });
  h.then([](ScriptedSession& s) {
    ASSERT_EQ(s.losses.size(), 1U);
    EXPECT_EQ(s.losses[0].first, 5);
    EXPECT_EQ(s.losses[0].second, 5);
    EXPECT_EQ(s.keyframe_requests, 1);
  });
  const auto outcome = h.run();
  EXPECT_TRUE(outcome.succeeded());
  EXPECT_EQ(h.runner->statistics().video.lost, 1U);
}

TEST(SessionEngineMedia, APresentedVerdictStandsInForVideoPresented) {
  SessionEngineRig h;
  h.streaming();
  h.then([&h](ScriptedSession& s) {
    s.emit(api_test::stream_info());
    h.media.verdict = {VideoVerdict::recover, -9};
    s.emit(api_test::video_media(1));
    h.media.verdict = {VideoVerdict::presented, 0};
    auto lossy = api_test::video_media(2);
    lossy.frames_lost = 4;
    s.emit(lossy);
    h.media.verdict = {VideoVerdict::recover, -9};
    s.emit(api_test::video_media(3));
    const auto presented = h.runner->video_presented();
    ASSERT_TRUE(presented);
    EXPECT_EQ(presented->frames_lost, 0U);
  });
  h.then([](ScriptedSession& s) {
    EXPECT_EQ(s.keyframe_requests, 0);
    EXPECT_EQ(s.losses.size(), 1U);
  });
  const auto outcome = h.run();
  EXPECT_TRUE(outcome.succeeded());
  EXPECT_EQ(h.session->keyframe_requests, 0);
  const auto statistics = h.runner->statistics();
  EXPECT_EQ(statistics.video.decoder_rejections, 2U);
  EXPECT_EQ(statistics.video.lost, 4U);
  EXPECT_EQ(statistics.video.units, 3U);
}

TEST(SessionEngineMedia,
     AsksForKeyframesAfterFecFailuresGuestRequestsAndDecoderReports) {
  SessionEngineRig h;
  EXPECT_FALSE(h.runner->request_keyframe());
  const remote_play::event::EncodedMedia fec{
      remote_play::EncodedMediaKind::video_fec_failure,
      api_test::bytes_of({0})};
  h.streaming();
  h.then([fec](ScriptedSession& s) { s.emit(fec); });
  h.then([fec](ScriptedSession& s) {
    EXPECT_EQ(s.keyframe_requests, 1);
    s.emit(fec);
  });
  h.then([&h](ScriptedSession& s) {
    EXPECT_EQ(s.keyframe_requests, 1);
    h.clock.advance(1000ms);
  });
  h.then([&h](ScriptedSession& s) {
    EXPECT_EQ(s.keyframe_requests, 2);
    EXPECT_TRUE(h.runner->request_keyframe());
  });
  h.then([&h](ScriptedSession& s) {
    EXPECT_EQ(s.keyframe_requests, 3);
    h.runner->video_rejected(9, -3);
    h.runner->video_rejected(10, -3);
  });
  h.then([&h](ScriptedSession& s) {
    ASSERT_EQ(s.losses.size(), 1U);
    EXPECT_EQ(s.losses[0].first, 9);
    EXPECT_EQ(s.losses[0].second, 10);
    EXPECT_EQ(s.keyframe_requests, 3);
    s.emit(api_test::stream_info());
    auto lossy = api_test::video_media(11);
    lossy.frames_lost = 2;
    s.emit(lossy);
    const auto presented = h.runner->video_presented();
    ASSERT_TRUE(presented);
    EXPECT_EQ(presented->frames_lost, 2U);
  });
  const auto outcome = h.run();
  EXPECT_TRUE(outcome.succeeded());
  EXPECT_TRUE(ended_as(outcome, Ending::finished));
  EXPECT_EQ(h.media.gaps, 2);
  const auto statistics = h.runner->statistics();
  EXPECT_EQ(statistics.video.keyframe_requests, 3U);
  EXPECT_EQ(statistics.video.decoder_rejections, 2U);
  EXPECT_EQ(statistics.video.lost, 2U);
  EXPECT_EQ(statistics.video.units, 1U);
}

class NamedDecoder final : public libchiaking::media::MediaSink {
 public:
  void on_stream_description(
      const libchiaking::media::StreamDescription&) override {}
  auto on_video(const libchiaking::media::VideoUnit&)
      -> libchiaking::media::VideoResult override {
    return verdict;
  }
  void on_video_gap() override {}
  auto on_audio(const libchiaking::media::AudioUnit&)
      -> libchiaking::media::AudioResult override {
    return {};
  }
  void on_feedback(
      const libchiaking::controllers::ControllerFeedback&) override {}
  auto decoder_name() const -> std::string_view override { return "Decoder"; }

  libchiaking::media::VideoResult verdict{};
};

class NamedStagesRig {
 public:
  NamedStagesRig() {
    auto owned = std::make_unique<ScriptedSession>(stop, play::Venue::home());
    session = owned.get();
    play::SessionSettings settings;
    settings.stages.recovery_request = "Decoder catch-up";
    runner = play::Session::Core::open(
                 {.player = SignedInPlayer::fresh(),
                  .console = nullptr,
                  .where = play::Where::home,
                  .service = std::nullopt,
                  .link = std::move(owned),
                  .stop = stop,
                  .media = media,
                  .events = events,
                  .clock = clock,
                  .threading = LiveProviders::threading(),
                  .journal = journal,
                  .first_controller =
                      libchiaking::controllers::ControllerKind::dualsense,
                  .settings = settings,
                  .share = nullptr})
                 .value();
  }

  auto run() -> play::Outcome {
    EXPECT_TRUE(runner->run_on_this_thread());
    return runner->outcome().value();
  }

  auto logged(std::string_view text) -> std::size_t {
    std::size_t total = 0;
    while (auto entry = journal->read())
      total += entry->line().find(text) != std::string::npos ? 1U : 0U;
    return total;
  }

  libchiaking::platform::StopSource stop;
  libchiaking::testing::ManualClock clock;
  NamedDecoder media;
  RecordedEvents events;
  std::shared_ptr<libchiaking::diagnostics::SessionJournal> journal{
      std::make_shared<libchiaking::diagnostics::SessionJournal>(
          LiveProviders::threading(), LiveProviders::steady(), 1)};
  ScriptedSession* session{};
  std::unique_ptr<play::Session::Core> runner;
};

TEST(SessionEngineMedia, NamesASubmissionStageAfterTheDecoder) {
  NamedStagesRig named;
  named.media.verdict = {VideoVerdict::submit_failed, 4};
  named.session->script.push_back([](ScriptedSession& s) {
    s.set_state(StreamState::streaming);
    s.emit(api_test::stream_info());
    s.emit(api_test::video_media(1));
  });
  const auto outcome = named.run();
  EXPECT_TRUE(ended_as(outcome, MediaFailure::decoder_submission));
  EXPECT_EQ(outcome.media_fault(), (MediaFault{MediaFailure::decoder_submission,
                                               "Decoder submission", 4}));
  EXPECT_EQ(named.logged("media failure: kind=decoder_submission cause=4"), 1U);
}

TEST(SessionEngineMedia, NamesAnOutputStageAfterTheDecoder) {
  NamedStagesRig drained;
  drained.session->script.push_back([&drained](ScriptedSession& s) {
    s.set_state(StreamState::streaming);
    drained.runner->video_output_failed(2);
  });
  const auto outcome = drained.run();
  EXPECT_TRUE(ended_as(outcome, MediaFailure::decoder_output));
  EXPECT_EQ(outcome.media_fault(),
            (MediaFault{MediaFailure::decoder_output, "Decoder output", 2}));
}

TEST(SessionEngineMedia, UsesAConfiguredRecoveryStageName) {
  NamedStagesRig behind;
  behind.session->loss_result =
      libchiaking::unexpected(StreamError::invalid_state);
  behind.media.verdict = {VideoVerdict::recover, -1};
  behind.session->script.push_back([](ScriptedSession& s) {
    s.set_state(StreamState::streaming);
    s.emit(api_test::stream_info());
    s.emit(api_test::video_media(1));
  });
  const auto outcome = behind.run();
  EXPECT_TRUE(ended_as(outcome, MediaFailure::recovery_request));
  EXPECT_EQ(outcome.media_fault(),
            (MediaFault{MediaFailure::recovery_request, "Decoder catch-up",
                        std::to_underlying(Cause::invalid_state)}));
}

}  // namespace
}  // namespace play_test
