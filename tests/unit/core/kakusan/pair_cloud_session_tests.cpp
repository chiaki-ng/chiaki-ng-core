#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/kakusan/pair_rig.hpp"

#include <gtest/gtest.h>

namespace kakusan_test {
namespace {

class KakusanCloudSession : public PairTest {};

TEST_F(KakusanCloudSession, AnswersWhyItCannotBeShared) {
  test_support::play::PublicHousehold home;
  auto pair = KakusanPair::create(home.factory, {host_store, guest_store});
  ASSERT_TRUE(pair);
  ScriptedLink link{PairTime::threading(), PairTime::steady()};
  test_support::play::PublicMedia media;
  test_support::play::PublicEvents events;
  PairHost host;

  const auto unsigned_in = home.play.start_scripted(
      home.home_user(), libchiaking::play::Where::cloud, link, {}, media,
      events);
  ASSERT_FALSE(unsigned_in);
  EXPECT_EQ(unsigned_in.error().kind(),
            libchiaking::play::StartRefusal::Kind::needs_psn_sign_in);
  EXPECT_TRUE(events.states.empty());
  EXPECT_TRUE(link.calls().empty());
  EXPECT_EQ(home.play.live(), nullptr);

  link.step_by_hand();
  const auto started = home.play.start_scripted(
      test_support::play::SignedInPlayer::fresh(), libchiaking::play::Where::cloud, link,
      {}, media, events);
  ASSERT_TRUE(started);
  link.connect();
  while (!events.reached(libchiaking::play::SessionState::playing))
    ASSERT_TRUE(link.turn());
  const auto shareable = started.value()->share().can_be_shared();
  ASSERT_FALSE(shareable);
  EXPECT_EQ(shareable.error(), libchiaking::play::NotShareable::cloud_session);
  EXPECT_TRUE(PairStage::refused(
      pair.value()->host().share(
          started.value()->share(),
          *libchiaking::kakusan::SharingRules::create(4, 1), host),
      Refusal::Kind::cannot_be_shared));
  EXPECT_EQ(host.seen.size(), 0U);
  EXPECT_TRUE(started.value()->leave());
  EXPECT_TRUE(started.value()->wait_until_ended().was_left());
}

}  // namespace
}  // namespace kakusan_test
