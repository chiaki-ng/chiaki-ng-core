#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/play/openssl/household.hpp"

#include <gtest/gtest.h>

#include <memory>

namespace play_test {

using test_support::LiveProviders;
using test_support::play::SignedInPlayer;
using test_support::play::Title;
namespace {

namespace play = libchiaking::play;
using play::SessionState;
using play::StartRefusal;
using play::Where;
using Generation = libchiaking::consoles::Generation;

class PlayLiveSession : public ::testing::Test {
 protected:
  void SetUp() override {
    link_.connect();
    ASSERT_EQ(home_.play.live(), nullptr);
    const auto started = home_.play.start_scripted(
        player_, Where::home, link_, {}, home_.media, home_.events);
    ASSERT_TRUE(started);
    session_ = started.value();
    home_.events.until_state(SessionState::playing);
  }

  void TearDown() override {
    for (const auto& session : {next_, session_}) {
      if (!session) continue;
      static_cast<void>(session->leave());
      static_cast<void>(session->wait_until_ended());
    }
  }

  Household home_;
  std::shared_ptr<libchiaking::users::PsUser> player_ = SignedInPlayer::fresh();
  play::testing::ScriptedLink link_{LiveProviders::threading(),
                                    LiveProviders::steady()};
  std::shared_ptr<play::Session> session_;
  play::testing::ScriptedLink second_{LiveProviders::threading(),
                                      LiveProviders::steady()};
  RecordedEvents second_events_;
  RecordedMedia second_media_;
  std::shared_ptr<play::Session> next_;
};

TEST_F(PlayLiveSession, IsTheScriptedSessionThatPlays) {
  EXPECT_EQ(home_.play.live(), session_);
  EXPECT_EQ(session_->where(), Where::home);
  EXPECT_EQ(session_->player().account_id(), player_->account_id());
  EXPECT_EQ(session_->console(), nullptr);
  EXPECT_EQ(session_->launch(), nullptr);
}

TEST_F(PlayLiveSession, RefusesEveryOtherStartAsBusy) {
  for (const auto where : {Where::home, Where::internet, Where::cloud}) {
    SCOPED_TRACE(testing::PrintToString(where));
    EXPECT_EQ(Household::refusal(home_.play.start_scripted(
                  player_, where, second_, {}, second_media_, second_events_)),
              StartRefusal::Kind::busy);
  }
  const auto console =
      home_.paired(*player_, Household::living_room, Generation::ps5,
                   "127.0.0.1", Household::device(std::byte{1}));
  EXPECT_EQ(Household::refusal(home_.play.start_on_console(
                player_, console, {}, second_media_, second_events_)),
            StartRefusal::Kind::busy);
  EXPECT_EQ(Household::refusal(home_.play.start_over_internet(
                player_, console, {}, second_media_, second_events_)),
            StartRefusal::Kind::busy);
  EXPECT_EQ(Household::refusal(home_.play.start_on_cloud(
                player_, Title::product_id(Household::cloud_title), {}, {},
                second_media_, second_events_)),
            StartRefusal::Kind::busy);
  EXPECT_TRUE(second_events_.names().empty());
  EXPECT_TRUE(second_.calls().empty());
}

TEST_F(PlayLiveSession, MakesRoomForTheNextStartOnceLeft) {
  EXPECT_TRUE(session_->leave());
  const auto& outcome = session_->wait_until_ended();
  EXPECT_TRUE(outcome.was_left());
  EXPECT_TRUE(ended_as(outcome, play::Outcome::Ending::left));
  EXPECT_EQ(home_.play.live(), nullptr);

  second_.connect();
  const auto again = home_.play.start_scripted(
      player_, Where::internet, second_, {}, second_media_, second_events_);
  ASSERT_TRUE(again);
  next_ = again.value();
  EXPECT_NE(next_, session_);
  EXPECT_EQ(next_->where(), Where::internet);
  second_events_.until_state(SessionState::playing);
  EXPECT_EQ(home_.play.live(), next_);
  EXPECT_EQ(session_->state(), SessionState::ended);
  EXPECT_TRUE(session_->outcome());
}

TEST_F(PlayLiveSession, EndsWhenItsPlayerIsForgotten) {
  home_.play.forget(libchiaking::AccountId{999});
  EXPECT_EQ(session_->state(), SessionState::playing);
  home_.play.forget(player_->account_id());
  EXPECT_TRUE(session_->wait_until_ended().was_left());
  EXPECT_EQ(home_.play.live(), nullptr);
}

}  // namespace
}  // namespace play_test
