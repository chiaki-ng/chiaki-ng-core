#include <libchiaking/play_types.hpp>

#include "tests/support/enum_printing.hpp"

#include <gtest/gtest.h>

namespace play_test {
namespace {

using libchiaking::catalog::ServiceGeneration;
using libchiaking::consoles::Generation;
using libchiaking::play::ControllerChoice;
using libchiaking::play::ControlsRequest;
using libchiaking::play::FeedbackStyle;

TEST(ControlsRequest, FollowsTheTargetByDefault) {
  const ControlsRequest standard;
  EXPECT_EQ(standard.controller(), ControllerChoice::console_default);
  EXPECT_FALSE(standard.console_keyboard());
  EXPECT_EQ(standard.feedback(), FeedbackStyle::standard);
  EXPECT_TRUE(standard.uses_dualsense_on(Generation::ps5));
  EXPECT_FALSE(standard.uses_dualsense_on(Generation::ps4));
  EXPECT_TRUE(standard.uses_dualsense_on(ServiceGeneration::ps5_cloud));
  EXPECT_TRUE(standard.uses_dualsense_on(ServiceGeneration::old_service));
}

TEST(ControlsRequest, UsesADualSenseEverywhereWhenChosen) {
  const ControlsRequest dualsense{ControllerChoice::dualsense,
                                  FeedbackStyle::switch_compatibility, true};
  EXPECT_TRUE(dualsense.uses_dualsense_on(Generation::ps4));
  EXPECT_TRUE(dualsense.uses_dualsense_on(Generation::ps5));
  EXPECT_TRUE(dualsense.console_keyboard());
  EXPECT_EQ(dualsense.feedback(), FeedbackStyle::switch_compatibility);
}

TEST(ControlsRequest, NeverUsesADualSenseWhenADualShock4IsChosen) {
  const ControlsRequest dualsense{ControllerChoice::dualsense,
                                  FeedbackStyle::switch_compatibility, true};
  const ControlsRequest dualshock{ControllerChoice::dualshock4,
                                  FeedbackStyle::standard, false};
  EXPECT_FALSE(dualshock.uses_dualsense_on(Generation::ps5));
  EXPECT_FALSE(dualshock.uses_dualsense_on(Generation::ps4));
  EXPECT_FALSE(dualshock.uses_dualsense_on(ServiceGeneration::ps5_cloud));
  EXPECT_NE(dualshock, dualsense);
  EXPECT_EQ(dualshock,
            (ControlsRequest{ControllerChoice::dualshock4, {}, false}));
}

}  // namespace
}  // namespace play_test
