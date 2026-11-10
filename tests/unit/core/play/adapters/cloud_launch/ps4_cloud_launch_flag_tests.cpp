#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/play/adapters/cloud_launch/launching.hpp"

#include <gtest/gtest.h>

#include <optional>

namespace play_test {
namespace {

namespace play = libchiaking::play;
using cloud_launches::Launching;
using Precondition = play::LaunchFailure::Precondition;

constexpr bool ps4_cloud_built = LIBCHIAKING_HAS_PS4_CLOUD_LAUNCH != 0;

class Ps4CloudLaunchFlag : public cloud_launches::LaunchTest {};

TEST_F(Ps4CloudLaunchFlag, DecidesAKnownPs4TitleBeforeAskingAnyone) {
  Launching ps4{old_service_case()};
  EXPECT_EQ(ps4.core().precondition(),
            ps4_cloud_built
                ? std::nullopt
                : std::optional{Precondition::ps4_cloud_launch_not_built});
  const auto slot = ps4.core().run();
  EXPECT_EQ(slot.has_value(), ps4_cloud_built);
  if (ps4_cloud_built) return;
  EXPECT_EQ(slot.error().refused().kind(),
            play::LaunchRefused::Kind::not_built);
  EXPECT_EQ(ps4.clearance().clears, 0);
  EXPECT_TRUE(ps4.listener().typed.empty());
}

TEST_F(Ps4CloudLaunchFlag, LeavesPs5CloudStreamingAlone) {
  if (LIBCHIAKING_HAS_PS5_CLOUD_LAUNCH == 0) GTEST_SKIP();
  Launching ps5{ps5_case()};
  EXPECT_EQ(ps5.core().precondition(), std::nullopt);
  EXPECT_TRUE(ps5.core().run());
}

}  // namespace
}  // namespace play_test
