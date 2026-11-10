#include "libchiaking/platform/default.hpp"
#include "src/core/play/adapters/links/cloud/cloud_link_configuration.hpp"
#include "src/core/play/adapters/links/link_driver.hpp"
#include "tests/support/catalog/idle_catalog.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/support/play/signed_in_player.hpp"

#include <gtest/gtest.h>

#include <utility>

namespace default_platform_test {
namespace {

class CloudStreamKindsOnTheDefaultAssembly : public ::testing::Test {
 protected:
  libchiaking::platform::DefaultPlatform platform;
};

TEST_F(CloudStreamKindsOnTheDefaultAssembly, ACloudLinkOpensIdle) {
  libchiaking::play::CloudLinkConfiguration cloud;
  cloud.user = test_support::play::SignedInPlayer::fresh();
  cloud.catalog = &test_support::catalog::IdleCatalog::shared();
  cloud.product = test_support::catalog::IdleCatalog::title();
  cloud.time_zone = "UTC+00:00";
  auto checked_cloud = std::move(cloud).validated();
  ASSERT_TRUE(checked_cloud);
  const auto cloud_session = libchiaking::play::CloudLinkConfiguration::link(
      std::move(checked_cloud.value()), platform.factory());
  ASSERT_TRUE(cloud_session);
  ASSERT_TRUE(cloud_session.value());
  EXPECT_EQ(cloud_session.value()->state(),
            libchiaking::net::protocol::remote_play::StreamState::idle);
}

}  // namespace
}  // namespace default_platform_test
