#include "src/core/play/adapters/datacenters/datacenter_selector.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/play/adapters/session/session_engine_rig.hpp"

#include <gtest/gtest.h>

#include <optional>

namespace play_test {
namespace {

namespace play = libchiaking::play;
namespace remote_play = libchiaking::net::protocol::remote_play;
using libchiaking::controllers::ControllerKind;
using libchiaking::controllers::SeatNumber;
using libchiaking::testing::ScriptedSession;
using Cause = play::EndReason::Cause;
using StreamError = remote_play::StreamError;
using StreamState = remote_play::StreamState;

class SessionEngineEvents : public ::testing::Test {
 protected:
  void SetUp() override {
    rig_.session->refused_command =
        remote_play::StreamCommand{remote_play::command::CouchJoin{}}.index();
    rig_.session->link.available = true;
    rig_.session->link.video_units = 7;
    rig_.session->version = 12;
    rig_.then([this](ScriptedSession& s) {
      s.set_state(StreamState::streaming);
      s.emit(remote_play::event::EncodedMedia{
          remote_play::EncodedMediaKind::audio, {}});
      s.emit(remote_play::event::EncodedMedia{
          remote_play::EncodedMediaKind::audio_profile, {}});
      s.emit(remote_play::event::EncodedMedia{
          remote_play::EncodedMediaKind::rumble,
          api_test::bytes_of({0, 1, 2, 0})});
      s.emit(remote_play::event::EncodedMedia{
          remote_play::EncodedMediaKind::motion_reset, {}});
      s.emit(remote_play::event::Holepunch{true});
      s.emit(remote_play::event::PadAnnounced{});
      EXPECT_TRUE(rig_.runner->seat_rules().add_player(
          SeatNumber::masked(2), ControllerKind::dualsense, std::nullopt));
      EXPECT_TRUE(rig_.runner->select_datacenter("dc1"));
    });
    outcome_.emplace(rig_.run());
  }

  auto rig() -> SessionEngineRig& { return rig_; }
  auto outcome() const -> const play::Outcome& { return *outcome_; }

 private:
  SessionEngineRig rig_;
  std::optional<play::Outcome> outcome_;
};

TEST_F(SessionEngineEvents, FinishTheRun) {
  EXPECT_TRUE(outcome().succeeded());
}

TEST_F(SessionEngineEvents, SendAudioAndFeedbackToTheMedia) {
  EXPECT_EQ(rig().media.audio.size(), 2U);
  ASSERT_EQ(rig().media.feedback.size(), 2U);
  EXPECT_EQ(rig().media.feedback[0],
            libchiaking::controllers::ControllerFeedback::Kind::rumble);
}

TEST_F(SessionEngineEvents, SendSeatsAndStatesToTheApp) {
  EXPECT_EQ(rig().events.count("seat_joined"), 1U);
  EXPECT_EQ(rig().events.joined_seat(), SeatNumber::masked(0));
  EXPECT_GE(rig().events.count("state"), 2U);
}

TEST_F(SessionEngineEvents, ReportASeatRequestTheLinkRefused) {
  const auto refusals = rig().events.refusals();
  ASSERT_EQ(refusals.size(), 1U);
  EXPECT_EQ(refusals[0].seat(), SeatNumber::masked(2));
  EXPECT_EQ(refusals[0].kind(), play::SeatRefusal::Kind::not_sent);
  EXPECT_EQ(refusals[0].cause(), Cause::invalid_argument);
  EXPECT_FALSE(refusals[0].console_status());
}

TEST_F(SessionEngineEvents, ForwardADatacenterOnlyWhileRunning) {
  EXPECT_EQ(rig().session->datacenter, "dc1");
  const auto late = rig().runner->select_datacenter("dc2");
  ASSERT_FALSE(late);
  EXPECT_EQ(late.error(), StreamError::invalid_state);
}

TEST_F(SessionEngineEvents, LogTheHolepunchAndTheTakionVersion) {
  EXPECT_EQ(rig().logged("holepunch finished"), 1U);
  EXPECT_EQ(rig().logged("takion version: negotiated=12"), 1U);
}

TEST_F(SessionEngineEvents, ReachTheStatistics) {
  const auto statistics = rig().runner->statistics();
  EXPECT_TRUE(statistics.link.available);
  EXPECT_EQ(statistics.link.video_units, 7U);
  EXPECT_EQ(statistics.protocol_version, 12U);
  EXPECT_EQ(statistics.loop_errors, 0U);
}

TEST(SessionEngineDatacenters, ForwardsPicksWhileRunningOnly) {
  SessionEngineRig h{
      play::Venue::cloud(libchiaking::catalog::ServiceGeneration::ps5_cloud)};
  h.then([&h](ScriptedSession& s) {
    play::DatacenterSelector& selector = *h.runner;
    EXPECT_TRUE(selector.select_datacenter("eu-west-1"));
    EXPECT_EQ(s.datacenter, "eu-west-1");
  });
  EXPECT_TRUE(h.run().succeeded());
  play::DatacenterSelector& selector = *h.runner;
  EXPECT_FALSE(selector.select_datacenter("dc"));
}

}  // namespace
}  // namespace play_test
