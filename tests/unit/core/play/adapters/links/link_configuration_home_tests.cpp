#include "src/core/play/adapters/links/home_link_configuration.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/play/adapters/links/link_configuration_rig.hpp"

#include <gtest/gtest.h>

#include <string_view>
#include <utility>

namespace play_test {
namespace {

namespace remote_play = libchiaking::net::protocol::remote_play;
using libchiaking::play::HomeLinkConfiguration;
using link_configurations::Configurations;
using remote_play::ControllerPreference;
using remote_play::RegistrationTarget;
using remote_play::StreamError;
using namespace std::string_view_literals;

static_assert(HomeLinkConfiguration::host_limit == 255);

TEST(LinkConfigurationAtHome, AcceptsALocalConsole) {
  auto checked = Configurations::local().validated();
  ASSERT_TRUE(checked);
  EXPECT_EQ(checked.value().host, "127.0.0.1");
}

TEST(LinkConfigurationAtHome, RefusesAnEmptyHost) {
  auto configuration = Configurations::local();
  configuration.host.clear();
  EXPECT_EQ(Configurations::refusal(std::move(configuration)),
            StreamError::invalid_configuration);
}

TEST(LinkConfigurationAtHome, AcceptsAHostUpToTheLimit) {
  auto configuration = Configurations::local();
  configuration.host.assign(HomeLinkConfiguration::host_limit + 1, 'h');
  EXPECT_EQ(Configurations::refusal(std::move(configuration)),
            StreamError::invalid_configuration);
  configuration = Configurations::local();
  configuration.host.assign(HomeLinkConfiguration::host_limit, 'h');
  EXPECT_TRUE(Configurations::accepts(std::move(configuration)));
}

TEST(LinkConfigurationAtHome, RefusesAHostWithANul) {
  auto configuration = Configurations::local();
  configuration.host = "127.0\0.0.1"sv;
  EXPECT_EQ(Configurations::refusal(std::move(configuration)),
            StreamError::invalid_configuration);
}

TEST(LinkConfigurationAtHome, RefusesAnUnknownTarget) {
  auto configuration = Configurations::local();
  configuration.console.target = RegistrationTarget::ps4_unknown;
  EXPECT_EQ(Configurations::refusal(std::move(configuration)),
            StreamError::invalid_configuration);
  configuration = Configurations::local();
  configuration.console.target = static_cast<RegistrationTarget>(99);
  EXPECT_EQ(Configurations::refusal(std::move(configuration)),
            StreamError::invalid_configuration);
}

TEST(LinkConfigurationAtHome, AcceptsEveryKnownPs4) {
  for (const auto target :
       {RegistrationTarget::ps4_8, RegistrationTarget::ps4_9,
        RegistrationTarget::ps4_10}) {
    SCOPED_TRACE(testing::PrintToString(target));
    auto configuration = Configurations::local();
    configuration.console.target = target;
    EXPECT_TRUE(Configurations::accepts(std::move(configuration)));
  }
}

TEST(LinkConfigurationAtHome, RefusesAnUnknownCodec) {
  auto configuration = Configurations::local();
  configuration.codec = static_cast<remote_play::VideoCodec>(99);
  EXPECT_EQ(Configurations::refusal(std::move(configuration)),
            StreamError::invalid_configuration);
}

TEST(LinkConfigurationAtHome, RefusesAnUnknownFeedbackMode) {
  auto configuration = Configurations::local();
  configuration.feedback = static_cast<remote_play::FeedbackMode>(99);
  EXPECT_EQ(Configurations::refusal(std::move(configuration)),
            StreamError::invalid_configuration);
}

TEST(LinkConfigurationAtHome, RefusesAZeroPort) {
  auto configuration = Configurations::local();
  configuration.ctrl_endpoint.port = 0;
  EXPECT_EQ(Configurations::refusal(std::move(configuration)),
            StreamError::invalid_configuration);
  configuration = Configurations::local();
  configuration.active_stream_port = 0;
  EXPECT_EQ(Configurations::refusal(std::move(configuration)),
            StreamError::invalid_configuration);
}

TEST(LinkConfigurationAtHome, PicksTheControllerTheTargetExpects) {
  auto console = Configurations::local();
  EXPECT_TRUE(console.prefer_controller(ControllerPreference::console_default));
  EXPECT_TRUE(console.enable_dualsense_ctrl);
  EXPECT_TRUE(console.prefer_controller(ControllerPreference::dualshock4));
  EXPECT_FALSE(console.enable_dualsense_ctrl);
  console.console.target = RegistrationTarget::ps4_10;
  EXPECT_TRUE(console.prefer_controller(ControllerPreference::console_default));
  EXPECT_FALSE(console.enable_dualsense_ctrl);
  EXPECT_TRUE(console.prefer_controller(ControllerPreference::dualsense));
  EXPECT_TRUE(console.enable_dualsense_ctrl);
}

}  // namespace
}  // namespace play_test
