#include "src/core/play/adapters/links/cloud/cloud_link_configuration.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/support/stop_states.hpp"
#include "tests/support/play/signed_in_player.hpp"

#include <gtest/gtest.h>

#include <memory>
#include <type_traits>
#include <utility>

namespace play_test {

using test_support::play::SignedInPlayer;
namespace {

using libchiaking::play::CloudLinkConfiguration;

static_assert(!std::is_copy_constructible_v<CloudLinkConfiguration>);
static_assert(!std::is_copy_assignable_v<CloudLinkConfiguration>);
static_assert(std::is_move_constructible_v<CloudLinkConfiguration>);
static_assert(std::is_move_assignable_v<CloudLinkConfiguration>);

TEST(CloudLinkConfiguration, StartsWithTheStandardSettingsAndNoTitle) {
  CloudLinkConfiguration config;
  EXPECT_EQ(config.catalog, nullptr);
  EXPECT_FALSE(config.product);
  EXPECT_EQ(config.datacenters, nullptr);
  EXPECT_EQ(config.resolution, 1080);
  EXPECT_EQ(config.bitrate_kbps, 15000);
  EXPECT_FALSE(config.skip_account_attributes_check);
  EXPECT_TRUE(config.datacenter.is_automatic());
  EXPECT_TRUE(config.time_zone.empty());
  EXPECT_TRUE(config.enable_dualsense_ctrl);
  EXPECT_FALSE(config.enable_idr_on_fec_failure);
  EXPECT_EQ(config.user, nullptr);
}

TEST(CloudLinkConfiguration, KeepsItsUserAndStopWhenMoved) {
  CloudLinkConfiguration config;
  SignedInPlayer player;
  config.user = player.user;
  auto stop_state = std::make_shared<test_support::StopState>();
  config.stop = libchiaking::platform::StopToken{stop_state};
  CloudLinkConfiguration moved{std::move(config)};
  EXPECT_EQ(moved.user, player.user);
  ASSERT_NE(moved.user, nullptr);
  EXPECT_EQ(moved.user->account_id(), player.user->account_id());
  EXPECT_FALSE(moved.stop.stop_requested());
  stop_state->stopped = true;
  EXPECT_TRUE(moved.stop.stop_requested());
}

}  // namespace
}  // namespace play_test
