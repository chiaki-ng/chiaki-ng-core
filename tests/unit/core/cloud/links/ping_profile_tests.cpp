#include "src/core/play/adapters/links/cloud/server_connect.hpp"
#include "tests/unit/core/cloud/links/ping_rig.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <variant>

namespace cloud_test {
namespace {

namespace play = libchiaking::play;
namespace takion = libchiaking::net::protocol::takion;
namespace senkusha = libchiaking::net::protocol::senkusha;
using play::PingService;

TEST(PingProfileEnvelope, SendsCloudStreamPlain) {
  const auto cloud = play::PingProfile{PingService::cloud_stream}.envelope();
  EXPECT_TRUE(std::holds_alternative<takion::PlainEnvelope>(cloud));
}

TEST(PingProfileEnvelope, WrapsTheLegacyServiceInTypeOne) {
  const auto now =
      play::PingProfile{PingService::legacy_cloud_stream}.envelope();
  const auto* psn = std::get_if<takion::PsnEnvelope>(&now);
  ASSERT_NE(psn, nullptr);
  EXPECT_EQ(psn->type, 1);
}

class PingProfileSenkusha : public ::testing::TestWithParam<PingService> {
 protected:
  const senkusha::ClientConfig config =
      play::PingProfile{GetParam()}.senkusha_config();
};

TEST_P(PingProfileSenkusha, BootstrapsAtVersionNineAndReceivesPlain) {
  const auto& profile = config.profile;
  EXPECT_EQ(profile.bootstrap_version, 9U);
  ASSERT_TRUE(profile.receive_envelope);
  EXPECT_TRUE(
      std::holds_alternative<takion::PlainEnvelope>(*profile.receive_envelope));
  const auto* fixed =
      std::get_if<takion::FixedTakionProfile>(&profile.profile_selection);
  ASSERT_NE(fixed, nullptr);
  EXPECT_EQ(fixed->version, 9U);
}

TEST_P(PingProfileSenkusha, EchoesAFragmentedBigWithoutSecrets) {
  const auto& profile = config.profile;
  EXPECT_EQ(profile.big_response,
            senkusha::BigResponsePolicy::fragmented_big_echo);
  EXPECT_EQ(profile.big.client_version, 9U);
  EXPECT_TRUE(profile.big.session_key.empty());
  EXPECT_TRUE(profile.big.launch_spec.empty());
  EXPECT_TRUE(profile.big.encrypted_key.empty());
  EXPECT_EQ(profile.handoff, senkusha::TransportHandoffPolicy::close_after_use);
}

TEST_P(PingProfileSenkusha, AdjustsTheRoundTripLikeTheReference) {
  EXPECT_EQ(config.profile.rtt_adjustment.subtract,
            std::chrono::milliseconds{20});
  EXPECT_EQ(config.profile.rtt_adjustment.floor, std::chrono::milliseconds{1});
}

TEST_P(PingProfileSenkusha, ProbesTenTimesAndSearchesTheMtu) {
  EXPECT_EQ(config.policy.probe.count, 10U);
  EXPECT_EQ(config.policy.mtu.lower_bound, 576U);
  EXPECT_EQ(config.policy.mtu.upper_bound, 1454U);
  EXPECT_EQ(config.policy.mtu.attempts_per_candidate, 3U);
  EXPECT_EQ(config.policy.mtu.timeout_floor, std::chrono::milliseconds{200});
  EXPECT_EQ(config.policy.mtu.timeout_ceiling, std::chrono::milliseconds{1000});
}

TEST_P(PingProfileSenkusha, SendsWithTheServicesEnvelope) {
  if (GetParam() == PingService::cloud_stream) {
    EXPECT_TRUE(
        std::holds_alternative<takion::PlainEnvelope>(config.profile.envelope));
    return;
  }
  const auto* actual =
      std::get_if<takion::PsnEnvelope>(&config.profile.envelope);
  ASSERT_NE(actual, nullptr);
  EXPECT_EQ(actual->type, 1);
}

INSTANTIATE_TEST_SUITE_P(PingServices, PingProfileSenkusha,
                         ::testing::Values(PingService::cloud_stream,
                                           PingService::legacy_cloud_stream));

TEST(ServerConnectEnvelope, FollowsTheWrapperType) {
  const auto plain_active = play::ServerConnect::takion_config(0).envelope;
  EXPECT_TRUE(std::holds_alternative<takion::PlainEnvelope>(plain_active));
  const auto wrapped_active = play::ServerConnect::takion_config(1).envelope;
  const auto* wrapped = std::get_if<takion::PsnEnvelope>(&wrapped_active);
  ASSERT_NE(wrapped, nullptr);
  EXPECT_EQ(wrapped->type, 1);
  const auto max_active = play::ServerConnect::takion_config(255).envelope;
  const auto* widest = std::get_if<takion::PsnEnvelope>(&max_active);
  ASSERT_NE(widest, nullptr);
  EXPECT_EQ(widest->type, 255);
}

}  // namespace
}  // namespace cloud_test
