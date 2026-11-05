#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/play/adapters/cloud_launch/launching.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <optional>

namespace play_test {
namespace {

namespace play = libchiaking::play;
using cloud_launches::Launching;
using Precondition = play::LaunchFailure::Precondition;

constexpr bool ps5_cloud_built = LIBCHIAKING_HAS_PS5_CLOUD_LAUNCH != 0;

class Ps5CloudLaunchFlag : public cloud_launches::LaunchTest {};

TEST_F(Ps5CloudLaunchFlag, DecidesAKnownPs5TitleBeforeAskingAnyone) {
  auto launched = ps5_case();
  launched.title.product = "EP0109-PPSA07033_00-FAKE0030TITLE000";
  Launching ps5{launched};
  EXPECT_EQ(ps5.core().precondition(),
            ps5_cloud_built ? std::nullopt
                            : std::optional{
                                  Precondition::ps5_cloud_launch_not_built});
  const auto slot = ps5.core().run();
  EXPECT_EQ(slot.has_value(), ps5_cloud_built);
  if (ps5_cloud_built) return;
  EXPECT_EQ(slot.error().refused().kind(), play::LaunchRefused::Kind::not_built);
  EXPECT_EQ(ps5.clearance().clears, 0);
  EXPECT_TRUE(ps5.listener().typed.empty());
}

TEST_F(Ps5CloudLaunchFlag, StopsATitleClearedOntoPs5CloudBeforeReserving) {
  Launching unmarked{ps5_case()};
  EXPECT_EQ(unmarked.core().precondition(), std::nullopt);
  const auto slot = unmarked.core().run();
  EXPECT_EQ(slot.has_value(), ps5_cloud_built);
  if (ps5_cloud_built) return;
  EXPECT_EQ(slot.error().refused().kind(), play::LaunchRefused::Kind::not_built);
  EXPECT_EQ(unmarked.clearance().clears, 1);
  EXPECT_TRUE(std::ranges::none_of(
      unmarked.listener().typed, [](const play::LaunchProgress& progress) {
        return progress.activity() == Activity::reserving_server;
      }));
}

TEST_F(Ps5CloudLaunchFlag, LeavesTheOlderServiceAlone) {
  Launching old_service{old_service_case()};
  EXPECT_EQ(old_service.core().precondition(), std::nullopt);
  EXPECT_TRUE(old_service.core().run());
}

}  // namespace
}  // namespace play_test
