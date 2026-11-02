#include "libchiaking/platform/default_factory.hpp"
#include "libchiaking/platform/net/default.hpp"
#include "src/core/play/adapters/links/cloud/cloud_link_configuration.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/play/adapters/links/cloud/loopback_rig.hpp"
#include "tests/unit/core/play/cleared_launch.hpp"
#include "tests/unit/core/play/cloud_provision.hpp"
#include "tests/support/play/signed_in_player.hpp"
#include "tests/support/live_providers.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <initializer_list>
#include <utility>

namespace play_test {

using test_support::LiveProviders;
using test_support::play::SignedInPlayer;
namespace {

namespace platform = libchiaking::platform;
namespace play = libchiaking::play;
namespace remote_play = libchiaking::net::protocol::remote_play;
using cloud_loopback::GkScript;
using cloud_loopback::HandshakeCrypto;
using cloud_loopback::LoopbackSink;
using libchiaking::cloud::CloudPlatform;

class CloudProvisioning : public cloud_loopback::LoopbackTest {
 protected:
  void provision(CloudService service, std::uint8_t requested_audio_channels) {
    http_.legacy_cloud_stream = service == CloudService::legacy_cloud_stream;
    http_.allocation_port = 47001;
    ClearedLaunch cleared{service};
    play::CloudLinkConfiguration config;
    config.user = SignedInPlayer::fresh();
    cleared.arm(config);
    config.time_zone = "UTC+00:00";
    config.datacenter = play::DatacenterPreference::named(
        play::DatacenterName::parse("dc1").value());
    config.prior_datacenters = {prior_row()};
    config.tuning.requested_audio_channels = requested_audio_channels;
    LoopbackSink sink;
    const auto allocation = CloudProvision::run(std::move(config), factory_,
                                                cleared.seams(), &sink);
    ASSERT_TRUE(allocation);
    EXPECT_EQ(http_.calls, http_.legacy_cloud_stream ? 13U : 11U);
    const auto& value = allocation.value();
    EXPECT_EQ(value.service, service);
    EXPECT_EQ(value.platform, http_.legacy_cloud_stream ? CloudPlatform::ps4
                                                        : CloudPlatform::ps5);
    EXPECT_EQ(value.entitlement_id, "synthetic-entitlement");
    EXPECT_EQ(value.server_ip, "127.0.0.1");
    EXPECT_EQ(value.server_port, 47001);
    EXPECT_EQ(value.handshake_key.view(), "AAECAwQFBgcICQoLDA0ODw==");
    EXPECT_EQ(value.launch_spec.view(), "synthetic-launch");
    EXPECT_EQ(value.session_id.view(), "synthetic-session");
    EXPECT_EQ(value.psn_wrapper_type, 7);
    EXPECT_EQ(value.mtu_in, 1454);
    EXPECT_EQ(value.mtu_out, 1254);
    EXPECT_EQ(value.rtt_us, 20000);
    ASSERT_EQ(value.datacenter_pings.size(), 2U);
    EXPECT_EQ(value.datacenter_pings[0].name, "dc1");
    EXPECT_EQ(sink.events, 0U);
    EXPECT_EQ(sink.datacenter_events, 1U);
    ASSERT_FALSE(sink.progress_texts.empty());
    EXPECT_EQ(sink.progress_texts.back(),
              "Allocating Streaming Slot - Step 10 of 10");
  }

  void refuse_without_a_user(CloudService service) {
    ClearedLaunch cleared{service};
    play::CloudLinkConfiguration missing;
    missing.time_zone = "UTC+00:00";
    cleared.arm(missing);
    const auto rejected = CloudProvision::run(
        std::move(missing), failing_factory_, cleared.seams());
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().result(),
              remote_play::StreamError::invalid_configuration);
    EXPECT_EQ(failing_.calls, 0U);
  }

  HandshakeCrypto crypto_;
  GkScript http_;
  GkScript failing_;
  platform::DefaultNetworkProvider network_{LiveProviders::threading(), LiveProviders::steady()};
  platform::DefaultPlatformFactory factory_{platform::PlatformProviders{
      .network = &network_, .crypto = &crypto_, .http = &http_}};
  platform::DefaultPlatformFactory failing_factory_{platform::PlatformProviders{
      .network = &network_, .crypto = &crypto_, .http = &failing_}};
};

TEST_F(CloudProvisioning, ReturnsAStreamReadyAllocationOnCloudStream) {
  provision(CloudService::cloud_stream, 0);
}

TEST_F(CloudProvisioning, ReturnsAStreamReadyAllocationOnLegacyCloudStream) {
  provision(CloudService::legacy_cloud_stream, 0);
}

TEST_F(CloudProvisioning, ReturnsAStereoAllocationOnCloudStream) {
  provision(CloudService::cloud_stream, 2);
}

TEST_F(CloudProvisioning, ReturnsAStereoAllocationOnLegacyCloudStream) {
  provision(CloudService::legacy_cloud_stream, 2);
}

TEST_F(CloudProvisioning, RefusesAConfigurationWithoutAUserOnCloudStream) {
  refuse_without_a_user(CloudService::cloud_stream);
}

TEST_F(CloudProvisioning,
       RefusesAConfigurationWithoutAUserOnLegacyCloudStream) {
  refuse_without_a_user(CloudService::legacy_cloud_stream);
}

TEST_F(CloudProvisioning, RefusesSurroundOnLegacyCloudStreamBeforeAnyRequest) {
  constexpr auto service = CloudService::legacy_cloud_stream;
  ClearedLaunch cleared{service};
  for (const auto channels : std::initializer_list<std::uint8_t>{6, 8, 12}) {
    SCOPED_TRACE(static_cast<int>(channels));
    play::CloudLinkConfiguration surround;
    surround.user = SignedInPlayer::fresh();
    cleared.arm(surround);
    surround.time_zone = "UTC+00:00";
    surround.tuning.requested_audio_channels = channels;
    const auto refused = CloudProvision::run(std::move(surround),
                                             failing_factory_, cleared.seams());
    ASSERT_FALSE(refused);
    const auto& detail = refused.error();
    EXPECT_EQ(detail.result(), remote_play::StreamError::invalid_configuration);
    EXPECT_TRUE(detail.as<play::LinkFailure::AudioRequestRefused>());
    EXPECT_EQ(detail.describe(),
              "surround audio is not supported for this target");
    EXPECT_EQ(detail.route(), venue_of(service));
  }
  EXPECT_EQ(failing_.calls, 0U);
}

TEST(CloudSystemMenu, IsThereOnlyForAPs3Game) {
  EXPECT_TRUE(play::CloudProvisioning::has_system_menu(CloudPlatform::ps3));
  EXPECT_FALSE(play::CloudProvisioning::has_system_menu(CloudPlatform::ps4));
  EXPECT_FALSE(play::CloudProvisioning::has_system_menu(CloudPlatform::ps5));
}

}  // namespace
}  // namespace play_test
