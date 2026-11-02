#include "tests/unit/core/cloud/links/connect_rig.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <string>
#include <string_view>
#include <type_traits>

namespace cloud_test {
namespace {

using Reason = ConnectRig::Reason;
using Stage = ConnectRig::Stage;

static_assert(!std::is_copy_constructible_v<play::HandshakeKey>);
static_assert(!std::is_copy_constructible_v<play::Handshake>);

class HandshakeKeyDecode : public PinnedTakionVersions {
 protected:
  static constexpr std::string_view golden = "AAECAwQFBgcICQoLDA0ODw==";

  static auto decodes(std::string_view text) -> testing::AssertionResult {
    const auto decoded = play::HandshakeKey::decode(text);
    if (!decoded) return testing::AssertionFailure() << "the key was refused";
    return ConnectRig::counts_up(decoded.value().bytes(), 16);
  }

  static auto refuses(std::string_view text, Reason reason)
      -> testing::AssertionResult {
    return ConnectRig::fails_with(play::HandshakeKey::decode(text),
                                  Stage::active, reason);
  }

  static auto refuses_character(std::size_t index, char character)
      -> testing::AssertionResult {
    std::string altered{golden};
    altered[index] = character;
    return refuses(altered, Reason::handshake_key_encoding);
  }
};

TEST_F(HandshakeKeyDecode, DecodesSixteenBytesOfBase64) {
  EXPECT_TRUE(decodes(golden));
}

TEST_F(HandshakeKeyDecode, RefusesAKeyOfAnotherLength) {
  EXPECT_TRUE(refuses("", Reason::handshake_key_length));
  EXPECT_TRUE(refuses(golden.substr(0, 20), Reason::handshake_key_length));
  EXPECT_TRUE(
      refuses("AAECAwQFBgcICQoLDA0ODxAR", Reason::handshake_key_length));
}

TEST_F(HandshakeKeyDecode, RefusesExtraPadding) {
  EXPECT_TRUE(
      refuses("AAECAwQFBgcICQoLDA0ODw===", Reason::handshake_key_encoding));
}

TEST_F(HandshakeKeyDecode, AcceptsMissingPaddingLineBreaksAndSpareBits) {
  EXPECT_TRUE(decodes("AAECAwQFBgcICQoLDA0ODw"));
  EXPECT_TRUE(decodes("AAECAwQFBgcI\nCQoLDA0ODw=="));
  EXPECT_TRUE(decodes("AAECAwQFBgcICQoLDA0ODx=="));
}

TEST_F(HandshakeKeyDecode, RefusesCharactersOutsideStandardBase64) {
  EXPECT_TRUE(refuses_character(2, '#'));
  EXPECT_TRUE(refuses_character(2, '-'));
  EXPECT_TRUE(refuses_character(2, '_'));
  EXPECT_TRUE(refuses_character(2, ' '));
}

TEST_F(HandshakeKeyDecode, ReadsThePlusAndSlashOfStandardBase64) {
  std::string standard{golden};
  standard[0] = '+';
  const auto plus = play::HandshakeKey::decode(standard);
  ASSERT_TRUE(plus);
  EXPECT_EQ(plus.value().bytes()[0], std::byte{0xf8});
  standard[0] = '/';
  const auto slash = play::HandshakeKey::decode(standard);
  ASSERT_TRUE(slash);
  EXPECT_EQ(slash.value().bytes()[0], std::byte{0xfc});
}

class HandshakePreparation
    : public PinnedTakionVersions,
      public ::testing::WithParamInterface<std::uint32_t> {
 protected:
  void SetUp() override {
    PinnedTakionVersions::SetUp();
    if (IsSkipped()) return;
    const auto resolved = takion::resolve_takion_wire_profile(GetParam());
    ASSERT_TRUE(resolved);
    profile.emplace(*resolved);
  }

  auto calibration() const -> libchiaking::net::protocol::senkusha::Result {
    return {GetParam(), {}, 1454, 1254, *profile};
  }

  auto prepare(const libchiaking::psn::gk::Allocation& input) {
    return play::Handshake::prepare(crypto, input, calibration());
  }

  std::optional<takion::WireProfile> profile;
  FakeCrypto crypto;
};

TEST_P(HandshakePreparation, SignsWithTheDecodedKeyOnTheProfileCurve) {
  const bool p521 = profile->ecdh_curve() == platform::EcdhCurve::secp521r1;
  auto input = ConnectRig::allocation();
  const auto result = prepare(input);
  ASSERT_TRUE(result);
  EXPECT_EQ(crypto.generated_curve, p521 ? platform::EcdhCurve::secp521r1
                                         : platform::EcdhCurve::secp256k1);
  EXPECT_EQ(result.value().session_id().view(), "session-id");
  EXPECT_EQ(result.value().launch_specification().view(), "encrypted-launch");
  EXPECT_EQ(result.value().ecdh().public_key.size, p521 ? 133U : 65U);
  ASSERT_EQ(crypto.hmac_key.size(), std::size_t{16});
  EXPECT_TRUE(ConnectRig::counts_up(result.value().key().bytes(), 16));
  EXPECT_TRUE(ConnectRig::counts_up(crypto.hmac_key, 16));
  input.session_id.clear();
  EXPECT_EQ(result.value().session_id().view(), "session-id");
}

TEST_P(HandshakePreparation, AcceptsAnEmptySessionId) {
  auto input = ConnectRig::allocation();
  input.session_id.clear();
  const auto result = prepare(input);
  ASSERT_TRUE(result);
  EXPECT_TRUE(result.value().session_id().view().empty());
}

TEST_P(HandshakePreparation, KeepsAt79CharactersOfTheSessionId) {
  for (const std::size_t length : {78U, 79U, 80U, 200U}) {
    SCOPED_TRACE(length);
    auto input = ConnectRig::allocation();
    const std::string long_id(length, 's');
    input.session_id.assign(long_id);
    const auto result = prepare(input);
    ASSERT_TRUE(result);
    EXPECT_EQ(result.value().session_id().view(), long_id.substr(0, 79));
  }
}

TEST_P(HandshakePreparation, RefusesAnEmptyLaunchSpecification) {
  auto input = ConnectRig::allocation();
  input.launch_specification.clear();
  EXPECT_TRUE(ConnectRig::fails_with(prepare(input), Stage::active,
                                     Reason::invalid_launch_specification));
}

TEST_P(HandshakePreparation, RefusesAnUndecodableKey) {
  auto input = ConnectRig::allocation();
  input.handshake_key.assign("invalid");
  EXPECT_TRUE(ConnectRig::fails_with(prepare(input), Stage::active,
                                     Reason::handshake_key_length));
}

TEST_P(HandshakePreparation, RefusesAProfileWithoutACloudHandshake) {
  const auto v7 = takion::resolve_takion_wire_profile(7);
  ASSERT_TRUE(v7);
  const libchiaking::net::protocol::senkusha::Result wrong_version{
      7, {}, 1454, 1254, *v7};
  EXPECT_TRUE(ConnectRig::fails_with(
      play::Handshake::prepare(crypto, ConnectRig::allocation(), wrong_version),
      Stage::active, Reason::invalid_profile));
}

TEST_P(HandshakePreparation, ReportsAnEcdhFailure) {
  crypto.fail = true;
  EXPECT_TRUE(ConnectRig::fails_with(prepare(ConnectRig::allocation()),
                                     Stage::active, Reason::ecdh_failure));
}

INSTANTIATE_TEST_SUITE_P(
    CloudVersions, HandshakePreparation,
    ::testing::Values(takion::takion_version_legacy_cloud_stream,
                      takion::takion_version_cloud_stream));

}  // namespace
}  // namespace cloud_test
