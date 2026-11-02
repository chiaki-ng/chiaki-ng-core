#include "src/core/play/adapters/cloud_launch/cloud_launch_core.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/support/play/launch_rig.hpp"

#include <gtest/gtest.h>

namespace cloud_test {
namespace {

namespace catalog = libchiaking::catalog;
namespace play = libchiaking::play;
using libchiaking::cloud::CloudPlatform;
using play::OldServiceLaunch;

TEST(Ps5CloudLaunchPlan, LaunchesTheOwnedProductOnAPs5) {
  const auto owned_cloud =
      test_support::play::Title{.product = "EP0109-PPSA00001_00-GAME"}.cleared();
  const play::Ps5CloudLaunch cloud_launch{owned_cloud};
  EXPECT_EQ(cloud_launch.entitlement(), "EP0109-PPSA00001_00-GAME");
  EXPECT_EQ(cloud_launch.platform(), CloudPlatform::ps5);
}

TEST(Ps5CloudLaunchPlan, RetriesTheReservationOnceAfterARefusal) {
  const auto owned_cloud =
      test_support::play::Title{.product = "EP0109-PPSA00001_00-GAME"}.cleared();
  const play::Ps5CloudLaunch cloud_launch{owned_cloud};
  EXPECT_TRUE(cloud_launch.retries_reservation_after_refusal(0));
  EXPECT_FALSE(cloud_launch.retries_reservation_after_refusal(1));
}

TEST(Ps5CloudLaunchPlan, LaunchesTheIdentifierTheCatalogLaunchedAs) {
  const auto launched_as =
      test_support::play::Title{.product = "EP0109-PPSA00003_00-GAME",
                       .identifier = "LAUNCHED-AS"}
          .cleared();
  EXPECT_EQ(play::Ps5CloudLaunch{launched_as}.entitlement(), "LAUNCHED-AS");
}

TEST(OldServiceLaunchPlan, TakesAHeldLicenceAndRepeatsTheTitleStepsOnce) {
  const auto owned_now =
      test_support::play::Title{.product = "EP0109-CUSA07001_00-GAME",
                       .service = catalog::ServiceGeneration::old_service,
                       .platform = catalog::Platform::ps4,
                       .held_as = "EP0109-CUSA07001_00-GAME"}
          .cleared();
  EXPECT_TRUE(OldServiceLaunch::takes_known_licence(owned_now));
  EXPECT_TRUE(
      OldServiceLaunch::repeats_title_steps_after_refusal(owned_now, 0));
  EXPECT_FALSE(
      OldServiceLaunch::repeats_title_steps_after_refusal(owned_now, 1));
}

TEST(OldServiceLaunchPlan, ResolvesAnUnheldTitleWithoutRepeating) {
  const auto resolved_now =
      test_support::play::Title{.product = "EP0109-CUSA07003_00-GAME",
                       .service = catalog::ServiceGeneration::old_service,
                       .platform = catalog::Platform::ps4}
          .cleared();
  EXPECT_FALSE(OldServiceLaunch::takes_known_licence(resolved_now));
  EXPECT_FALSE(
      OldServiceLaunch::repeats_title_steps_after_refusal(resolved_now, 0));
}

TEST(OldServiceLaunchPlan, MapsEachCatalogPlatformToItsCloudPlatform) {
  EXPECT_EQ(OldServiceLaunch::platform_of(catalog::Platform::ps3),
            CloudPlatform::ps3);
  EXPECT_EQ(OldServiceLaunch::platform_of(catalog::Platform::ps4),
            CloudPlatform::ps4);
  EXPECT_EQ(OldServiceLaunch::platform_of(catalog::Platform::ps5),
            CloudPlatform::ps5);
}

}  // namespace
}  // namespace cloud_test
