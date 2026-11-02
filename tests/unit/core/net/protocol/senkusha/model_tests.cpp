#include "src/core/net/protocol/senkusha/validator.hpp"
#include "tests/support/enum_printing.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <variant>
#include <vector>

namespace senkusha_test {
namespace {

namespace senkusha = libchiaking::net::protocol::senkusha;
namespace takion = libchiaking::net::protocol::takion;

class SenkushaConfiguration : public ::testing::Test {
 protected:
  template <class Mutator>
  static auto rejects(Mutator mutate, senkusha::Error expected)
      -> testing::AssertionResult {
    senkusha::Profile profile;
    senkusha::Policy policy;
    mutate(profile, policy);
    const auto result =
        senkusha::Validator::validate_configuration(profile, policy);
    if (result)
      return testing::AssertionFailure() << "the configuration was accepted";
    if (result.error() != expected)
      return testing::AssertionFailure()
             << "the error is " << testing::PrintToString(result.error())
             << ", not " << testing::PrintToString(expected);
    return testing::AssertionSuccess();
  }

  static auto cloud_big_echo() -> senkusha::Profile {
    senkusha::Profile profile;
    profile.bootstrap_version = 9;
    profile.profile_selection = takion::FixedTakionProfile{9};
    profile.big_response = senkusha::BigResponsePolicy::fragmented_big_echo;
    return profile;
  }
};

TEST_F(SenkushaConfiguration, DefaultsToTheLocalProfile) {
  const senkusha::Profile profile;
  const senkusha::Policy policy;
  EXPECT_TRUE(senkusha::Validator::validate_configuration(profile, policy));
  EXPECT_EQ(profile.bootstrap_version, 7U);
  EXPECT_EQ(profile.big_response, senkusha::BigResponsePolicy::typed_bang);
  EXPECT_EQ(profile.big.client_version, 9U);
  EXPECT_FALSE(profile.bang_validation.require_version_accepted);
  EXPECT_FALSE(profile.bang_validation.require_encrypted_key_accepted);
  EXPECT_FALSE(profile.bang_validation.require_protocol_ack);
  EXPECT_EQ(profile.handoff, senkusha::TransportHandoffPolicy::return_route);
  const auto* negotiated =
      std::get_if<takion::NegotiatedTakionProfiles>(&profile.profile_selection);
  ASSERT_NE(negotiated, nullptr);
  EXPECT_EQ(negotiated->supported_versions,
            (std::vector<std::uint32_t>{12, 15, 18, 20}));
  EXPECT_EQ(policy.probe.count, 10U);
  EXPECT_EQ(policy.probe.timeout, std::chrono::milliseconds{1000});
  EXPECT_EQ(policy.probe.minimum_payload_size, 8U);
  EXPECT_EQ(policy.probe.tag_offset, 4U);
  EXPECT_TRUE(policy.probe.require_unit_index);
  EXPECT_EQ(policy.mtu.lower_bound, 576U);
  EXPECT_EQ(policy.mtu.upper_bound, 1454U);
  EXPECT_EQ(policy.mtu.attempts_per_candidate, 3U);
  EXPECT_EQ(policy.mtu.network_overhead, 28U);
  static_assert(senkusha::mtu_av_header_size == 18);
  EXPECT_EQ(policy.mtu.timeout_multiplier, 5U);
  EXPECT_EQ(policy.mtu.timeout_floor, std::chrono::milliseconds{5});
  EXPECT_EQ(policy.mtu.timeout_ceiling, std::chrono::milliseconds{500});
}

TEST_F(SenkushaConfiguration, RejectsUnsupportedVersions) {
  EXPECT_TRUE(
      rejects([](auto& profile, auto&) { profile.bootstrap_version = 0; },
              senkusha::Error::unsupported_version));
  EXPECT_TRUE(
      rejects([](auto& profile, auto&) { profile.bootstrap_version = 8; },
              senkusha::Error::unsupported_version));
  EXPECT_TRUE(rejects(
      [](auto& profile, auto&) {
        profile.profile_selection = takion::FixedTakionProfile{8};
      },
      senkusha::Error::unsupported_version));
  EXPECT_TRUE(rejects(
      [](auto& profile, auto&) {
        profile.profile_selection = takion::NegotiatedTakionProfiles{{}};
      },
      senkusha::Error::invalid_configuration));
  EXPECT_TRUE(rejects(
      [](auto& profile, auto&) {
        profile.profile_selection = takion::NegotiatedTakionProfiles{{12, 8}};
      },
      senkusha::Error::unsupported_version));
  EXPECT_TRUE(rejects(
      [](auto& profile, auto&) {
        profile.profile_selection = takion::NegotiatedTakionProfiles{{12, 12}};
      },
      senkusha::Error::duplicate_version));
}

TEST_F(SenkushaConfiguration, RejectsCloudStreamOnlyVersions) {
  for (const std::uint32_t version : {21U, 23U, 25U}) {
    EXPECT_TRUE(rejects(
        [version](auto& profile, auto&) {
          profile.profile_selection =
              takion::NegotiatedTakionProfiles{{12, 15, version}};
        },
        senkusha::Error::unsupported_version))
        << "version " << version;
    EXPECT_TRUE(rejects(
        [version](auto& profile, auto&) {
          profile.profile_selection = takion::FixedTakionProfile{version};
        },
        senkusha::Error::unsupported_version))
        << "version " << version;
    EXPECT_TRUE(
        rejects([version](auto& profile,
                          auto&) { profile.bootstrap_version = version; },
                senkusha::Error::unsupported_version))
        << "version " << version;
  }
}

TEST_F(SenkushaConfiguration, OffersNoCloudStreamOnlyVersionByDefault) {
  const senkusha::Profile defaults;
  const auto* offered = std::get_if<takion::NegotiatedTakionProfiles>(
      &defaults.profile_selection);
  ASSERT_NE(offered, nullptr);
  for (const auto version : offered->supported_versions)
    EXPECT_FALSE(takion::is_cloud_stream_only_takion_version(version))
        << "version " << version;
}

TEST_F(SenkushaConfiguration, RejectsAnInvalidPolicy) {
  EXPECT_TRUE(rejects([](auto&, auto& policy) { policy.probe.count = 0; },
                      senkusha::Error::invalid_configuration));
  EXPECT_TRUE(rejects([](auto&, auto& policy) { policy.probe.count = 0x801; },
                      senkusha::Error::invalid_configuration));
  EXPECT_TRUE(rejects(
      [](auto&, auto& policy) {
        policy.probe.timeout = std::chrono::milliseconds{0};
      },
      senkusha::Error::invalid_configuration));
  EXPECT_TRUE(rejects(
      [](auto&, auto& policy) {
        policy.connect_timeout = std::chrono::milliseconds{-1};
      },
      senkusha::Error::invalid_configuration));
  EXPECT_TRUE(rejects(
      [](auto&, auto& policy) {
        policy.response_timeout = std::chrono::milliseconds{0};
      },
      senkusha::Error::invalid_configuration));
  EXPECT_TRUE(rejects(
      [](auto&, auto& policy) { policy.mtu.attempts_per_candidate = 0; },
      senkusha::Error::invalid_configuration));
  EXPECT_TRUE(
      rejects([](auto&, auto& policy) { policy.mtu.timeout_multiplier = 0; },
              senkusha::Error::invalid_configuration));
  EXPECT_TRUE(rejects(
      [](auto&, auto& policy) {
        policy.mtu.lower_bound = policy.mtu.upper_bound;
      },
      senkusha::Error::invalid_configuration));
  EXPECT_TRUE(rejects(
      [](auto&, auto& policy) {
        policy.mtu.timeout_floor = std::chrono::milliseconds{501};
      },
      senkusha::Error::invalid_configuration));
  EXPECT_TRUE(rejects([](auto&, auto& policy) { policy.probe.tag_offset = 5; },
                      senkusha::Error::invalid_configuration));
  EXPECT_TRUE(rejects(
      [](auto& profile, auto&) {
        profile.rtt_adjustment.subtract = std::chrono::microseconds{-1};
      },
      senkusha::Error::invalid_configuration));
  EXPECT_TRUE(rejects(
      [](auto& profile, auto&) {
        profile.big_response = senkusha::BigResponsePolicy::fragmented_big_echo;
      },
      senkusha::Error::invalid_configuration));
}

TEST_F(SenkushaConfiguration,
       AllowsACloudBigEchoOnlyWithTheFixedV9Credentials) {
  auto profile = cloud_big_echo();
  profile.big.session_key =
      libchiaking::net::protocol::takion::SharedSecretText{"synthetic-key"};
  EXPECT_TRUE(senkusha::Validator::validate_configuration(profile, {}));
  profile.big.launch_spec =
      libchiaking::net::protocol::takion::SharedSecretText{"unexpected"};
  EXPECT_FALSE(senkusha::Validator::validate_configuration(profile, {}));
  profile.big.launch_spec =
      libchiaking::net::protocol::takion::SharedSecretText{};
  profile.big.ecdh_pub_key = std::vector<std::byte>{std::byte{1}};
  profile.big.ecdh_sig = std::vector<std::byte>{std::byte{2}};
  EXPECT_FALSE(senkusha::Validator::validate_configuration(profile, {}));
}

TEST_F(SenkushaConfiguration, RequiresASessionKeyForACloudBigEcho) {
  auto profile = cloud_big_echo();
  auto secure = std::make_shared<libchiaking::SecretString>();
  secure->assign("synthetic-secure-key");
  profile.big.session_key = takion::SharedSecretText{secure};
  EXPECT_TRUE(senkusha::Validator::validate_configuration(profile, {}));
  secure->clear();
  EXPECT_FALSE(senkusha::Validator::validate_configuration(profile, {}));
  profile.big.session_key =
      libchiaking::net::protocol::takion::SharedSecretText{};
  EXPECT_FALSE(senkusha::Validator::validate_configuration(profile, {}));
}

TEST_F(SenkushaConfiguration, ChecksTheMtuArithmetic) {
  EXPECT_TRUE(
      rejects([](auto&, auto& policy) { policy.mtu.network_overhead = 551; },
              senkusha::Error::invalid_configuration));
  EXPECT_TRUE(rejects(
      [](auto&, auto& policy) {
        policy.mtu.network_overhead = std::numeric_limits<std::uint32_t>::max();
      },
      senkusha::Error::arithmetic_overflow));
  EXPECT_TRUE(rejects(
      [](auto&, auto& policy) {
        policy.probe.tag_offset = std::numeric_limits<std::uint32_t>::max();
      },
      senkusha::Error::arithmetic_overflow));
}

TEST_F(SenkushaConfiguration, ValidatesTheBigRequest) {
  EXPECT_TRUE(
      rejects([](auto& profile, auto&) { profile.big.client_version = 0; },
              senkusha::Error::invalid_configuration));
  EXPECT_TRUE(rejects(
      [](auto& profile, auto&) {
        profile.big.ecdh_pub_key = std::vector<std::byte>{std::byte{1}};
      },
      senkusha::Error::invalid_configuration));
  EXPECT_TRUE(rejects(
      [](auto& profile, auto&) {
        profile.big.ecdh_sig = std::vector<std::byte>{std::byte{2}};
      },
      senkusha::Error::invalid_configuration));
  senkusha::Profile profile;
  profile.big.ecdh_pub_key = std::vector<std::byte>{std::byte{1}};
  profile.big.ecdh_sig = std::vector<std::byte>{std::byte{2}};
  EXPECT_TRUE(
      senkusha::Validator::validate_configuration(profile, senkusha::Policy{}));
}

TEST(SenkushaBangValidation, AcceptsADefaultBangForLocalCalibration) {
  const senkusha::Profile profile;
  const takion::Bang bang;
  EXPECT_TRUE(senkusha::Validator::validate_bang(profile, bang, 12));
}

TEST(SenkushaBangValidation, EnforcesTheAcceptancePolicy) {
  senkusha::Profile profile;
  profile.bang_validation.require_version_accepted = true;
  profile.bang_validation.require_encrypted_key_accepted = true;
  takion::Bang bang;
  const auto neither = senkusha::Validator::validate_bang(profile, bang, 12);
  ASSERT_FALSE(neither);
  EXPECT_EQ(neither.error(), senkusha::Error::negotiation_failed);
  bang.version_accepted = true;
  const auto version_only =
      senkusha::Validator::validate_bang(profile, bang, 12);
  ASSERT_FALSE(version_only);
  EXPECT_EQ(version_only.error(), senkusha::Error::negotiation_failed);
  bang.encrypted_key_accepted = true;
  EXPECT_TRUE(senkusha::Validator::validate_bang(profile, bang, 12));
}

TEST(SenkushaBangValidation, RequiresAProtocolAckOnlyWhenThePolicySays) {
  senkusha::Profile profile;
  takion::Bang bang;
  bang.protocol_ack = takion::ProtocolRequestAck{12};
  EXPECT_TRUE(senkusha::Validator::validate_bang(profile, bang, 12));
  bang.protocol_ack = takion::ProtocolRequestAck{15};
  EXPECT_TRUE(senkusha::Validator::validate_bang(profile, bang, 12));
  bang.protocol_ack = takion::ProtocolRequestAck{};
  EXPECT_TRUE(senkusha::Validator::validate_bang(profile, bang, 12));
  profile.bang_validation.require_protocol_ack = true;
  EXPECT_TRUE(senkusha::Validator::validate_bang(profile, bang, 12));
  profile.bang_validation.require_protocol_ack = false;

  bang.protocol_ack.reset();
  profile.bang_validation.require_protocol_ack = true;
  const auto missing = senkusha::Validator::validate_bang(profile, bang, 12);
  ASSERT_FALSE(missing);
  EXPECT_EQ(missing.error(), senkusha::Error::malformed_response);
  const auto unsupported = senkusha::Validator::validate_bang(profile, bang, 8);
  ASSERT_FALSE(unsupported);
  EXPECT_EQ(unsupported.error(), senkusha::Error::unsupported_version);
}

}  // namespace
}  // namespace senkusha_test
