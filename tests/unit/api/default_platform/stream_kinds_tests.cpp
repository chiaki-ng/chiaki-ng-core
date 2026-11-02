#include "libchiaking/platform/default.hpp"
#include "src/core/play/adapters/links/cloud/cloud_link_configuration.hpp"
#include "src/core/play/adapters/links/home_link_configuration.hpp"
#include "src/core/play/adapters/links/internet/internet_link.hpp"
#include "src/core/play/adapters/links/internet/internet_link_configuration.hpp"
#include "src/core/play/adapters/links/link_driver.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/support/catalog/idle_catalog.hpp"
#include "tests/support/play/signed_in_player.hpp"

#include <gtest/gtest.h>

#include <utility>

namespace default_platform_test {
namespace {

namespace play = libchiaking::play;
namespace remote_play = libchiaking::net::protocol::remote_play;
using libchiaking::net::protocol::remote_play::RegistrationTarget;

class StreamKindsOnTheDefaultAssembly : public ::testing::Test {
 protected:
  class IgnoredEvents final : public play::LinkDriverSink {
   public:
    void on_event(remote_play::StreamEvent) override {}
    void on_video_frame(libchiaking::media::VideoFrame) override {}
  };

  libchiaking::platform::DefaultPlatform platform;
  IgnoredEvents events;
};

TEST_F(StreamKindsOnTheDefaultAssembly, AHomeLinkDrivesFromIdle) {
  play::HomeLinkConfiguration local;
  local.host = "127.0.0.1";
  local.console.target = RegistrationTarget::ps5_1;
  local.ctrl_endpoint.port = 9295;
  auto checked_local = std::move(local).validated();
  ASSERT_TRUE(checked_local);
  auto local_session = play::HomeLinkConfiguration::link(
      std::move(checked_local.value()), platform.factory());
  ASSERT_TRUE(local_session);
  ASSERT_TRUE(local_session.value());
  libchiaking::platform::StopSource stop;
  play::LinkDriver driver{std::move(local_session.value()),
                          stop,
                          platform.factory().threading(),
                          events,
                          platform.log(),
                          libchiaking::play::Venue::home()};
  EXPECT_EQ(driver.state(), remote_play::StreamState::idle);
  EXPECT_FALSE(driver.failure());
}

TEST_F(StreamKindsOnTheDefaultAssembly, AnInternetLinkOpens) {
  play::InternetLinkConfiguration remote;
  remote.user = test_support::play::SignedInPlayer::fresh_for_remote_play(
      "SYNTHETIC_BEARER_TOKEN");
  remote.console.target = RegistrationTarget::ps5_1;
  auto checked_remote = std::move(remote).validated();
  ASSERT_TRUE(checked_remote);
  const auto remote_session = play::InternetLinks::playing(
      std::move(checked_remote.value()), platform.factory(), nullptr);
  ASSERT_TRUE(remote_session);
  EXPECT_TRUE(remote_session.value());
}

TEST_F(StreamKindsOnTheDefaultAssembly, ACloudLinkOpensIdle) {
  play::CloudLinkConfiguration cloud;
  cloud.user = test_support::play::SignedInPlayer::fresh();
  cloud.catalog = &test_support::catalog::IdleCatalog::shared();
  cloud.product = test_support::catalog::IdleCatalog::title();
  cloud.time_zone = "UTC+00:00";
  auto checked_cloud = std::move(cloud).validated();
  ASSERT_TRUE(checked_cloud);
  const auto cloud_session = play::CloudLinkConfiguration::link(
      std::move(checked_cloud.value()), platform.factory());
  ASSERT_TRUE(cloud_session);
  ASSERT_TRUE(cloud_session.value());
  EXPECT_EQ(cloud_session.value()->state(), remote_play::StreamState::idle);
}

}  // namespace
}  // namespace default_platform_test
