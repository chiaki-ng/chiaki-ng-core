#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/play/adapters/session/session_share_case.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <optional>
#include <utility>

namespace play_test {

using test_support::play::SessionRig;
namespace {

namespace play = libchiaking::play;
using play::GuestTag;
using play::InputRefusal;
using play::NotShareable;
using play::SeatRefusal;

using PlaySessionShareEnding = ShareCase;

TEST_F(PlaySessionShareEnding, IsToldExactlyOnceToTheAudienceSharing) {
  RecordedAudience stopped;
  RecordedAudience audience;
  std::optional<play::SharedSeat> kept;
  std::optional<play::Sharing> outlived;
  libchiaking::controllers::ControllerState state;
  {
    SessionRig rig;
    rig.start_playing();
    auto& share = rig.session->share();
    auto early = share.begin(stopped);
    ASSERT_TRUE(early);
    early.value().stop();
    auto sharing = share.begin(audience);
    ASSERT_TRUE(sharing);
    auto guest = share.seat_guest({GuestTag{1}});
    ASSERT_TRUE(guest);
    EXPECT_TRUE(rig.session->leave());
    rig.session->wait_until_ended();
    EXPECT_EQ(audience.count("session_ended"), 1U);
    EXPECT_EQ(stopped.count("session_ended"), 0U);
    EXPECT_EQ(audience.threads(), rig.events.threads());
    EXPECT_EQ(guest.value().send(state).error(), InputRefusal::not_running);
    EXPECT_EQ(share.seat_guest({GuestTag{2}}).error().kind(),
              SeatRefusal::Kind::not_sent);
    EXPECT_EQ(share.begin(stopped).error(), NotShareable::session_ended);
    sharing.value().stop();
    kept.emplace(std::move(guest.value()));
    outlived.emplace(std::move(sharing.value()));
  }
  EXPECT_EQ(audience.count("session_ended"), 1U);
  EXPECT_EQ(stopped.count("session_ended"), 0U);
  EXPECT_EQ(kept->send(state).error(), InputRefusal::not_running);
  EXPECT_EQ(kept->number(), seat(1));
  outlived->stop();
  kept.reset();
  outlived.reset();
}

TEST_F(PlaySessionShareEnding, IsToldWhenTheSessionFails) {
  RecordedAudience failed;
  SessionRig failing;
  auto sharing = failing.session->share().begin(failed);
  ASSERT_TRUE(sharing);
  failing.start_playing();
  failing.link.fail(
      play::EndReason{play::EndReason::Cause::control_timeout,
                      {.step = play::EndReason::Step::control_receive,
                       .console_reason = 0x80108b10,
                       .detail = "lost"}});
  failing.session->wait_until_ended();
  EXPECT_EQ(failed.count("session_ended"), 1U);
}

TEST_F(PlaySessionShareEnding, IsToldWhenASessionThatNeverStartedGoes) {
  RecordedAudience unstarted;
  std::optional<play::Sharing> token;
  {
    SessionRig never;
    auto begun = never.session->share().begin(unstarted);
    ASSERT_TRUE(begun);
    token.emplace(std::move(begun.value()));
    EXPECT_EQ(unstarted.count("session_ended"), 0U);
  }
  EXPECT_EQ(unstarted.count("session_ended"), 1U);
  token.reset();
  EXPECT_EQ(unstarted.count("session_ended"), 1U);
}

TEST_F(PlaySessionShareEnding, AnAudienceCanStopFromInsideItsOwnTurn) {
  SessionRig rig;
  rig.start_playing();
  auto& share = rig.session->share();
  RecordedAudience audience;
  std::optional<play::Sharing> sharing;
  std::atomic<bool> stopped{false};
  std::optional<play::SharedSeat> seated;
  {
    auto begun = share.begin(audience);
    ASSERT_TRUE(begun);
    sharing.emplace(std::move(begun.value()));
  }
  audience.in_next_service([&] {
    auto guest = share.seat_guest({GuestTag{1}});
    EXPECT_TRUE(guest);
    if (guest) seated.emplace(std::move(guest.value()));
    share.request_keyframe();
    share.wake();
    sharing->stop();
    stopped.store(true);
  });
  share.wake();
  SessionRig::eventually([&] { return stopped.load(); });
  const auto services = audience.count("service");
  RecordedAudience next;
  auto again = share.begin(next);
  ASSERT_TRUE(again);
  next.until("service", 3);
  EXPECT_EQ(audience.count("service"), services);
  EXPECT_TRUE(rig.session->seats().seat(seat(1)).is_empty());
  libchiaking::controllers::ControllerState state;
  ASSERT_TRUE(seated);
  EXPECT_EQ(seated->send(state).error(), InputRefusal::not_owner);
}

}  // namespace
}  // namespace play_test
