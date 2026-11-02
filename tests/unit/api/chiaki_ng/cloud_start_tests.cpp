#include "tests/unit/api/chiaki_ng/rig.hpp"

#include <gtest/gtest.h>

namespace chiaki_ng_test {
namespace {

using ChiakiNgCloudStart = ChiakiNgTest;

TEST_F(ChiakiNgCloudStart, ClearsAndStartsATitleOrRefusesItTyped) {
  Household home;
  EXPECT_TRUE(home.credentials.save(SignedInPlayer::saved()));
  EXPECT_TRUE(home.credentials.save(
      users::SavedHomeUser{AccountId{home_account}, "Living Room Player"}));
  EXPECT_TRUE(home.chiaki.users().welcome_back());
  const auto player =
      home.chiaki.users().find(AccountId{SignedInPlayer::account});
  const auto home_user = home.chiaki.users().find(AccountId{home_account});
  ASSERT_TRUE(player);
  ASSERT_TRUE(home_user);
  const auto product = test_support::play::Title::product_id(cloud_title);

  const auto unlisted =
      home.chiaki.catalog().process_library_and_list(*home_user);
  EXPECT_FALSE(unlisted);
  EXPECT_FALSE(
      home.chiaki.catalog().can_launch(*home_user, product).is_allowed());
  EXPECT_EQ(refusal(home.chiaki.play().start_on_cloud(
                home_user, product, {}, {}, home.media, home.events)),
            StartRefusal::Kind::needs_psn_sign_in);

  const auto started = home.chiaki.play().start_on_cloud(
      player, product, {}, test_support::play::LaunchCase::named("dc1"), home.media,
      home.events);
  ASSERT_TRUE(started);
  EXPECT_EQ(started.value()->where(), Where::cloud);
  EXPECT_EQ(started.value()->console(), nullptr);
  ASSERT_NE(started.value()->launch(), nullptr);
  EXPECT_EQ(started.value()->launch()->product(), product);
  home.events.until("datacenter_chosen");
  ASSERT_EQ(home.clearance.clears, 1);
  EXPECT_EQ(home.clearance.cleared_products.front(), product);
  home.end(*started.value());

  test_support::play::ScriptedClearance refusing{
      catalog::LaunchRefusal::needs_premium(users::Tier::extra)};
  home.answering.answer_with(refusing);
  RecordedEvents refused_events;
  const auto refused = home.chiaki.play().start_on_cloud(
      player, product, {}, {}, home.media, refused_events);
  ASSERT_TRUE(refused);
  const auto& outcome = refused.value()->wait_until_ended();
  EXPECT_FALSE(outcome.succeeded());
  ASSERT_NE(outcome.launch_refused(), nullptr);
  EXPECT_EQ(outcome.launch_refused()->kind(),
            play::LaunchRefused::Kind::not_cleared);
  EXPECT_EQ(home.chiaki.play().live(), nullptr);
}

}  // namespace
}  // namespace chiaki_ng_test
