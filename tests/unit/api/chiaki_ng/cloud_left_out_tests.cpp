#include "tests/unit/api/chiaki_ng/rig.hpp"

#include <gtest/gtest.h>

namespace chiaki_ng_test {
namespace {

using ChiakiNgCloudLeftOut = ChiakiNgTest;

TEST_F(ChiakiNgCloudLeftOut, RefusesEveryCloudStartAsNotBuilt) {
  Household home;
  EXPECT_TRUE(home.credentials.save(SignedInPlayer::saved()));
  EXPECT_TRUE(home.chiaki.users().welcome_back());
  const auto player =
      home.chiaki.users().find(AccountId{SignedInPlayer::account});
  ASSERT_TRUE(player);
  const auto product = test_support::play::Title::product_id(cloud_title);

  const auto started = home.chiaki.play().start_on_cloud(
      player, product, {}, {}, home.media, home.events);
  ASSERT_FALSE(started);
  EXPECT_EQ(started.error().kind(), StartRefusal::Kind::launch_refused);
  ASSERT_TRUE(started.error().launch());
  EXPECT_EQ(started.error().launch()->kind(),
            play::LaunchRefused::Kind::not_built);
  EXPECT_EQ(home.clearance.clears, 0);
  EXPECT_EQ(home.chiaki.play().live(), nullptr);
}

}  // namespace
}  // namespace chiaki_ng_test
