#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/play/adapters/links/link_configuration_rig.hpp"

#include <gtest/gtest.h>

namespace play_test {
namespace {

namespace remote_play = libchiaking::net::protocol::remote_play;
using link_configurations::Configurations;
using remote_play::ControllerPreference;
using remote_play::StreamError;

static_assert(static_cast<int>(ControllerPreference::console_default) == 0);
static_assert(static_cast<int>(ControllerPreference::dualshock4) == 1);
static_assert(static_cast<int>(ControllerPreference::dualsense) == 2);

class LinkConfigurationControllers : public ::testing::TestWithParam<int> {
 protected:
  auto unknown() const -> ControllerPreference {
    return static_cast<ControllerPreference>(GetParam());
  }
};

TEST_P(LinkConfigurationControllers, RefusesAnUnknownChoiceAtHome) {
  auto console = Configurations::local();
  console.enable_dualsense_ctrl = true;
  const auto local_choice = console.prefer_controller(unknown());
  ASSERT_FALSE(local_choice);
  EXPECT_EQ(local_choice.error(), StreamError::invalid_configuration);
  EXPECT_TRUE(console.enable_dualsense_ctrl);
  console.enable_dualsense_ctrl = false;
  EXPECT_FALSE(console.prefer_controller(unknown()));
  EXPECT_FALSE(console.enable_dualsense_ctrl);
}

TEST_P(LinkConfigurationControllers, RefusesAnUnknownChoiceOverTheInternet) {
  auto psn = Configurations::remote();
  psn.enable_dualsense_ctrl = true;
  const auto remote_choice = psn.prefer_controller(unknown());
  ASSERT_FALSE(remote_choice);
  EXPECT_EQ(remote_choice.error(), StreamError::invalid_configuration);
  EXPECT_TRUE(psn.enable_dualsense_ctrl);
}

TEST_P(LinkConfigurationControllers, RefusesAnUnknownChoiceInTheCloud) {
  auto streamed = Configurations::cloud();
  EXPECT_TRUE(streamed.prefer_controller(ControllerPreference::dualshock4));
  const auto cloud_choice = streamed.prefer_controller(unknown());
  ASSERT_FALSE(cloud_choice);
  EXPECT_EQ(cloud_choice.error(), StreamError::invalid_configuration);
  EXPECT_FALSE(streamed.enable_dualsense_ctrl);
}

INSTANTIATE_TEST_SUITE_P(UnknownPreferences, LinkConfigurationControllers,
                         ::testing::Values(3, 4, 127, 255));

TEST(LinkConfigurationKnownControllers, AreAcceptedOnEveryKind) {
  for (const auto known :
       {ControllerPreference::console_default, ControllerPreference::dualshock4,
        ControllerPreference::dualsense}) {
    SCOPED_TRACE(testing::PrintToString(known));
    EXPECT_TRUE(Configurations::local().prefer_controller(known));
    EXPECT_TRUE(Configurations::remote().prefer_controller(known));
    EXPECT_TRUE(Configurations::cloud().prefer_controller(known));
  }
}

}  // namespace
}  // namespace play_test
