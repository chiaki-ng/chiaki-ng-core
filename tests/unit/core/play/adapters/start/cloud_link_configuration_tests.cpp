#include "tests/support/enum_printing.hpp"
#include "tests/support/utc_offset.hpp"
#include "tests/unit/core/play/adapters/start/link_request_rig.hpp"

#include <libchiaking/platform/runtime/utc_offset.hpp>

#include <gtest/gtest.h>

#include <chrono>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace play_test {

using test_support::catalog::IdleCatalog;
using test_support::play::SignedInPlayer;
namespace {

namespace play = libchiaking::play;
namespace remote_play = libchiaking::net::protocol::remote_play;
using libchiaking::catalog::Platform;
using libchiaking::catalog::ProductId;
using std::chrono::minutes;
using test_support::FixedUtcOffset;

class CloudLinkConfiguration : public LinkRequestRig {
 protected:
  static auto zone(std::optional<minutes> offset) {
    return play::CloudLinkConfiguration::create(cloud_launch(),
                                                FixedUtcOffset{offset});
  }

  const FixedUtcOffset clock{minutes{60}};
};

TEST_F(CloudLinkConfiguration, NamesATitleOfTheCatalog) {
  using namespace std::string_view_literals;
  const FixedUtcOffset utc{minutes{0}};
  for (const auto title : {"EP0109-PPSA07043_00-FAKE0043TITLE000"sv,
                           "EP0109-CUSA07031_00-FAKE0106TITLE000"sv,
                           "EP0109-NPUA07001_00-FAKE0021TITLE000"sv}) {
    SCOPED_TRACE(title);
    const auto built =
        play::CloudLinkConfiguration::create(cloud_launch(title), utc);
    ASSERT_TRUE(built);
    ASSERT_TRUE(built.value().product);
    EXPECT_EQ(*built.value().product, title);
    EXPECT_EQ(built.value().catalog, &IdleCatalog::shared());
  }
}

TEST_F(CloudLinkConfiguration, ReadsThePlatformFromTheProductId) {
  EXPECT_EQ(ProductId::parse("EP0109-PPSA07043_00-FAKE0043TITLE000")
                .value()
                .platform(),
            Platform::ps5);
  EXPECT_EQ(ProductId::parse("EP0109-CUSA07031_00-FAKE0106TITLE000")
                .value()
                .platform(),
            Platform::ps4);
  EXPECT_FALSE(ProductId::parse(""));
  EXPECT_FALSE(ProductId::parse(std::string(4097, 'e')));
  EXPECT_FALSE(ProductId::parse(std::string_view{"EP0109\0CUSA", 11}));
}

TEST_F(CloudLinkConfiguration, TakesTheTimeZoneFromThePlatformClock) {
  EXPECT_EQ(zone(minutes{0}).value().time_zone, "UTC+00:00");
  EXPECT_EQ(zone(minutes{330}).value().time_zone, "UTC+05:30");
  EXPECT_EQ(zone(minutes{-480}).value().time_zone, "UTC-08:00");
  EXPECT_EQ(zone(minutes{1439}).value().time_zone, "UTC+23:59");
  const auto unreadable = zone(std::nullopt);
  ASSERT_FALSE(unreadable);
  EXPECT_EQ(unreadable.error(), ConfigurationError::clock_unavailable);
  const auto distant = zone(minutes{-1440});
  ASSERT_FALSE(distant);
  EXPECT_EQ(distant.error(), ConfigurationError::time_zone_out_of_range);
}

TEST_F(CloudLinkConfiguration, ReadsTheSystemOffsetWithinADay) {
  const auto system = libchiaking::platform::SystemUtcOffset{}.utc_offset();
  ASSERT_TRUE(system);
  EXPECT_GT(system->count(), -1440);
  EXPECT_LT(system->count(), 1440);
}

TEST_F(CloudLinkConfiguration, CarriesTheLaunchAndTheChosenDatacenter) {
  auto launch = cloud_launch();
  const auto player = launch.user;
  launch.datacenter = play::DatacenterPreference::named(
      play::DatacenterName::parse("eu-west-1").value());
  launch.prior_datacenters.push_back(
      {.name = "eu-west-1", .rtt_ms = 21, .measured = true});
  const auto built =
      play::CloudLinkConfiguration::create(std::move(launch), clock);
  ASSERT_TRUE(built);
  const auto& cloud_config = built.value();
  ASSERT_TRUE(cloud_config.product);
  EXPECT_EQ(*cloud_config.product, "EP0109-PPSA07043_00-FAKE0043TITLE000");
  EXPECT_EQ(cloud_config.catalog, &IdleCatalog::shared());
  EXPECT_EQ(cloud_config.datacenters, nullptr);
  EXPECT_EQ(cloud_config.user, player);
  EXPECT_EQ(cloud_config.time_zone, "UTC+01:00");
  ASSERT_TRUE(cloud_config.datacenter.name());
  EXPECT_EQ(cloud_config.datacenter.name()->text(), "eu-west-1");
  EXPECT_FALSE(cloud_config.datacenter.asks_at_launch());
  ASSERT_EQ(cloud_config.prior_datacenters.size(), 1u);
  EXPECT_EQ(cloud_config.prior_datacenters.front().rtt_ms, 21);
  EXPECT_EQ(cloud_config.resolution, 1080u);
  EXPECT_EQ(cloud_config.bitrate_kbps, 15000u);
  EXPECT_TRUE(cloud_config.enable_dualsense_ctrl);
  EXPECT_FALSE(cloud_config.enable_idr_on_fec_failure);
  EXPECT_EQ(cloud_config.feedback, remote_play::FeedbackMode::standard);
}

TEST_F(CloudLinkConfiguration, CanAskForTheDatacenterAtLaunch) {
  auto picked = cloud_launch();
  picked.datacenter =
      play::DatacenterPreference::ask_at_launch(std::chrono::seconds{20})
          .value();
  const auto at_launch =
      play::CloudLinkConfiguration::create(std::move(picked), clock);
  ASSERT_TRUE(at_launch);
  EXPECT_TRUE(at_launch.value().datacenter.asks_at_launch());
  EXPECT_EQ(at_launch.value().datacenter.wait(), std::chrono::seconds{20});
  EXPECT_FALSE(at_launch.value().datacenter.name());
}

TEST_F(CloudLinkConfiguration, NeedsASignedInPlayer) {
  auto anonymous = cloud_launch();
  anonymous.user.reset();
  const auto refused =
      play::CloudLinkConfiguration::create(std::move(anonymous), clock);
  ASSERT_FALSE(refused);
  EXPECT_EQ(refused.error(), ConfigurationError::invalid_credential);
  auto offline = cloud_launch();
  offline.user = home_user();
  const auto unsigned_in =
      play::CloudLinkConfiguration::create(std::move(offline), clock);
  ASSERT_FALSE(unsigned_in);
  EXPECT_EQ(unsigned_in.error(), ConfigurationError::invalid_credential);
  EXPECT_FALSE(
      SignedInPlayer::admissible(SignedInPlayer::saved("SYNTHETIC TOKEN")));
}

TEST_F(CloudLinkConfiguration, NeedsAReadableClockWithinADay) {
  const auto unreadable = play::CloudLinkConfiguration::create(
      cloud_launch(), FixedUtcOffset{std::nullopt});
  ASSERT_FALSE(unreadable);
  EXPECT_EQ(unreadable.error(), ConfigurationError::clock_unavailable);
  const auto distant = play::CloudLinkConfiguration::create(
      cloud_launch(), FixedUtcOffset{minutes{1440}});
  ASSERT_FALSE(distant);
  EXPECT_EQ(distant.error(), ConfigurationError::time_zone_out_of_range);
}

TEST_F(CloudLinkConfiguration, PrefersAGivenTimeZoneToTheClock) {
  auto zoned = cloud_launch();
  zoned.time_zone = "UTC-03:30";
  EXPECT_EQ(play::CloudLinkConfiguration::create(std::move(zoned),
                                                 FixedUtcOffset{std::nullopt})
                .value()
                .time_zone,
            "UTC-03:30");
  auto clockless = cloud_launch();
  clockless.time_zone = "UTC+09:00";
  EXPECT_EQ(play::CloudLinkConfiguration::create(std::move(clockless))
                .value()
                .time_zone,
            "UTC+09:00");
  const auto zoneless = play::CloudLinkConfiguration::create(cloud_launch());
  ASSERT_FALSE(zoneless);
  EXPECT_EQ(zoneless.error(), ConfigurationError::clock_unavailable);
}

TEST_F(CloudLinkConfiguration, RefusesAMalformedTimeZone) {
  for (const auto* text : {"UTC+24:00", "UTC+01:60", "GMT+01:00", "UTC 01:00",
                           "UTC+1:00", "UTC+0a:00"}) {
    SCOPED_TRACE(text);
    auto malformed = cloud_launch();
    malformed.time_zone = text;
    const auto refused_zone =
        play::CloudLinkConfiguration::create(std::move(malformed), clock);
    ASSERT_FALSE(refused_zone);
    EXPECT_EQ(refused_zone.error(), ConfigurationError::time_zone_out_of_range);
  }
  for (const auto* text : {"UTC+23:59", "UTC-00:00"}) {
    SCOPED_TRACE(text);
    auto exact = cloud_launch();
    exact.time_zone = text;
    EXPECT_EQ(play::CloudLinkConfiguration::create(std::move(exact))
                  .value()
                  .time_zone,
              text);
  }
}

TEST_F(CloudLinkConfiguration, RefusesInvalidTuning) {
  auto lossy = cloud_launch();
  lossy.tuning.packet_loss_max = 2.0;
  const auto rejected =
      play::CloudLinkConfiguration::create(std::move(lossy), clock);
  ASSERT_FALSE(rejected);
  EXPECT_EQ(rejected.error(), ConfigurationError::rejected);
}

}  // namespace
}  // namespace play_test
