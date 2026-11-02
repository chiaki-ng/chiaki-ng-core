#include "libchiaking/platform/net/default.hpp"
#include "src/core/net/protocol/takion/profile.hpp"
#include "src/core/play/adapters/cloud_launch/cloud_launch_core.hpp"
#include "tests/support/clock.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/support/play/launch_rig.hpp"
#include "tests/support/play/signed_in_player.hpp"
#include "tests/support/process_threading.hpp"
#include "tests/support/random.hpp"
#include "tests/support/users/signed_in.hpp"
#include "tests/support/waiter.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

namespace cloud_test {
namespace {

namespace catalog = libchiaking::catalog;
namespace play = libchiaking::play;
namespace platform = libchiaking::platform;
namespace takion = libchiaking::net::protocol::takion;
using Precondition = play::LaunchFailure::Precondition;
using SpecJson = libchiaking::Expected<std::string, Precondition>;

class Launching {
 public:
  explicit Launching(const test_support::play::LaunchCase& launched)
      : clearance_(launched.clearance()),
        utc_offset_(launched.utc_offset),
        core_({offline_, network_, clock_, test_support::process_threading(),
               random_, waiter_, clearance_, utc_offset_},
              launched.request(), listener_) {
    waiter_.answer = false;
  }

  auto spec_json(std::string_view entitlement, std::string_view language) const
      -> SpecJson {
    auto spec = core_.launch_spec(entitlement, language);
    if (!spec) return libchiaking::unexpected(spec.error());
    return std::string{spec.value().json()};
  }

  auto core() const noexcept -> const play::CloudLaunch::Core& { return core_; }

 private:
  test_support::users::Offline offline_;
  platform::SteadyClock steady_;
  platform::DefaultNetworkProvider network_{test_support::process_threading(),
                                            steady_};
  test_support::FakeClock clock_{1'000};
  test_support::FailingRandom random_;
  test_support::FakeWaiter waiter_;
  test_support::play::ScriptedClearance clearance_;
  test_support::FixedUtcOffset utc_offset_;
  test_support::play::RecordingListener listener_;
  play::CloudLaunch::Core core_;
};

class CloudLaunchSpec : public ::testing::Test {
 protected:
  void SetUp() override {
    config.user = player.user;
    config.time_zone = "UTC+01:00";
    config.resolution = 720;
  }

  static auto spec_json(const test_support::play::LaunchCase& launched,
                        std::string_view entitlement = "synthetic-entitlement",
                        std::string_view language = "en-US") -> SpecJson {
    return Launching{launched}.spec_json(entitlement, language);
  }

  static auto protocol(std::uint32_t version) -> std::string {
    return "\"protocolVersion\":" + std::to_string(version) + ",";
  }

  static auto sends(const SpecJson& spec, std::string_view field)
      -> testing::AssertionResult {
    if (!spec) return testing::AssertionFailure() << "the spec was refused";
    if (spec.value().find(field) == std::string::npos)
      return testing::AssertionFailure() << "the spec lacks " << field;
    return testing::AssertionSuccess();
  }

  static auto refuses(const SpecJson& spec, Precondition reason)
      -> testing::AssertionResult {
    if (spec) return testing::AssertionFailure() << "the spec was built";
    if (spec.error() != reason)
      return testing::AssertionFailure()
             << "refused as " << testing::PrintToString(spec.error());
    return testing::AssertionSuccess();
  }

  const std::uint32_t cloud_stream_pinned =
      takion::select_takion_version(takion::takion_version_cloud_stream, 0,
                                    takion::SessionScope::cloud_stream);
  const std::uint32_t legacy_pinned = takion::select_takion_version(
      takion::takion_version_legacy_cloud_stream, 0,
      takion::SessionScope::legacy_cloud_stream);
  test_support::play::SignedInPlayer player;
  test_support::play::LaunchCase config;
};

TEST_F(CloudLaunchSpec, SendsTheRequestedTitleLanguageZoneAndResolution) {
  const auto valid = spec_json(config);
  EXPECT_TRUE(sends(valid, "\"entitlementId\":\"synthetic-entitlement\""));
  EXPECT_TRUE(sends(valid, "\"language\":\"en\""));
  EXPECT_TRUE(sends(valid, "\"timeZone\":\"UTC+01:00\""));
  EXPECT_TRUE(sends(valid, "\"resolutionSetting\":\"720\""));
}

TEST_F(CloudLaunchSpec, FallsBackToEnglishWithoutALanguage) {
  EXPECT_TRUE(sends(spec_json(config, "synthetic-entitlement", {}),
                    "\"language\":\"en\""));
}

TEST_F(CloudLaunchSpec, PinsTheCloudStreamProtocolVersion) {
  EXPECT_TRUE(sends(spec_json(config), protocol(cloud_stream_pinned)));
}

TEST_F(CloudLaunchSpec, SendsAnOverriddenProtocolVersion) {
  config.takion_version_override = 20;
  EXPECT_TRUE(sends(spec_json(config), protocol(20)));
  config.takion_version_override = 25;
  EXPECT_TRUE(sends(spec_json(config), protocol(25)));
}

TEST_F(CloudLaunchSpec, PinsTheLegacyVersionForTheOldServiceDespiteAnOverride) {
  config.takion_version_override = 25;
  config.title.service = catalog::ServiceGeneration::old_service;
  EXPECT_TRUE(sends(spec_json(config), protocol(legacy_pinned)));
}

TEST_F(CloudLaunchSpec, SendsTheDesktopSpecForEveryOldServicePlatform) {
  config.title.service = catalog::ServiceGeneration::old_service;
  for (const auto target : {catalog::Platform::ps3, catalog::Platform::ps4,
                            catalog::Platform::ps5}) {
    SCOPED_TRACE(testing::PrintToString(target));
    config.title.platform = target;
    const auto legacy = spec_json(config);
    EXPECT_TRUE(sends(legacy, protocol(legacy_pinned)));
    EXPECT_TRUE(sends(legacy, "\"platform\":\"PC\""));
  }
}

TEST_F(CloudLaunchSpec, RefusesAMissingUser) {
  config.user.reset();
  EXPECT_TRUE(refuses(spec_json(config), Precondition::invalid_npsso));
  EXPECT_EQ(Launching{config}.core().precondition(),
            Precondition::invalid_npsso);
}

TEST_F(CloudLaunchSpec, RefusesAHomeUserWithoutAnNpsso) {
  const auto home = player.signed_in.users.add_home_user(
      libchiaking::AccountId{40}, "Living Room Player");
  ASSERT_TRUE(home);
  config.user = home.value();
  EXPECT_TRUE(refuses(spec_json(config), Precondition::invalid_npsso));
}

TEST_F(CloudLaunchSpec, RefusesANoBitrate) {
  config.bitrate_kbps = 0;
  EXPECT_TRUE(refuses(spec_json(config), Precondition::invalid_bitrate));
  EXPECT_EQ(Launching{config}.core().precondition(),
            Precondition::invalid_bitrate);
}

TEST_F(CloudLaunchSpec, RefusesAnEmptyEntitlement) {
  EXPECT_TRUE(refuses(spec_json(config, {}), Precondition::invalid_spec));
}

TEST_F(CloudLaunchSpec, AcceptsAZoneNameOrNone) {
  config.time_zone = "Europe/London";
  EXPECT_TRUE(spec_json(config));
  config.time_zone.clear();
  EXPECT_TRUE(spec_json(config));
}

TEST_F(CloudLaunchSpec, HasNoPreconditionForAPlayerWithAnyCaseDevice) {
  auto identity = config;
  identity.time_zone = "UTC+01:00";
  EXPECT_FALSE(Launching{identity}.core().precondition());
  test_support::play::SignedInPlayer upper{
      test_support::play::SignedInPlayer::saved(
          test_support::play::SignedInPlayer::npsso,
          "0000000700410080000102030405060708090A0B0C0D0E0F")};
  identity.user = upper.user;
  EXPECT_FALSE(Launching{identity}.core().precondition());
}

TEST_F(CloudLaunchSpec, ParsesNoMalformedDuid) {
  EXPECT_FALSE(libchiaking::users::DeviceId::parse(
      "1111111111111111000102030405060708090a0b0c0d0e0f"));
  EXPECT_FALSE(libchiaking::users::DeviceId::parse(
      "000000070041008000010203040506070809"));
}

TEST_F(CloudLaunchSpec, RefusesAZoneWithANul) {
  config.time_zone = std::string{"U\0C", 3};
  EXPECT_EQ(Launching{config}.core().precondition(),
            Precondition::invalid_spec);
}

}  // namespace
}  // namespace cloud_test
