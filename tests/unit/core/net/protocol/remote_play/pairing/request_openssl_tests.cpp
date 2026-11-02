#include "libchiaking/platform/crypto/openssl/provider.hpp"
#include "src/core/net/protocol/remote_play/pairing/material.hpp"
#include "src/core/net/protocol/remote_play/pairing/request.hpp"
#include "src/core/utils/text/hex.hpp"
#include "tests/support/enum_printing.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace remote_play_test {
namespace {

using libchiaking::net::protocol::remote_play::PayloadError;
using libchiaking::net::protocol::remote_play::RegistrationMaterial;
using libchiaking::net::protocol::remote_play::RegistrationRequestCodec;
using libchiaking::net::protocol::remote_play::RegistrationTarget;

struct OpenSslRegistrationPayloadCase {
  RegistrationTarget target;
  bool online;
  std::string_view tail;
  std::string_view aeropause;
  std::size_t size;
};

class OpenSslRegistrationPayloadRig {
 public:
  static constexpr std::string_view tail_pre10_account =
      "2240b3a0336f851c4e0f80a994bf310298629b5dd39935dabec801b578909b9d9e2871fc"
      "a17efecdf04d39699d89697dc34e";
  static constexpr std::string_view tail_pre10_online =
      "2240b3a0336f851c4e0f80a994bf310298629b5dd39935dabec60cba7e8b90c4f8b1b32f"
      "ed1de04f993bbfa014e72cf2bc5d81e3";
  static constexpr std::string_view tail_ps4_10 =
      "65c415f1a02c009f80090ee1634637e0860fdd627ea8bf30bb5bf94c3af8731796a2e1b2"
      "212045abc216fe2a54a1aa6e3d60ab4ba8de4d8a931ceccb50d2d711463a73b576ab4e2f"
      "4ac79f7a7c128e8b31d375d1006b6d7e159fb67dd93b798df1afcb2e6320e7d991d7a2";
  static constexpr std::string_view tail_ps5_1 =
      "72bf826590ac0f1466f8a838ee1c99eabf5b2b8d1dc5ba05e22bb4bd18515bfd88a0760b"
      "f30ffaac6b9780f8aa1c8c4c34a6ae7b4d01fad22ba8b88969cfad8b693174d6f6002ad4"
      "bc35ad54a4c0442d3c57d31afcb7a06019961b1733cbcd605ce356a350c1f30aed79ed";
  static constexpr std::string_view aero_pre10 =
      "363b4bed0a6adf522bd9caaf47e517d3";
  static constexpr std::string_view aero_ps4_10 =
      "88142727c2e37371df5ed5a59cf77e8f";
  static constexpr std::string_view aero_ps5_1 =
      "7b0a388cfad1da4345260d8aec78656c";

 protected:
  using Bytes = std::vector<std::byte>;

  static auto from_hex(std::string_view hex) -> Bytes {
    Bytes out(hex.size() / 2);
    EXPECT_TRUE(libchiaking::text::decode_hex(hex, out)) << hex;
    return out;
  }

  static auto account() -> Bytes { return from_hex("0001020304050607"); }

  static auto material_for(RegistrationTarget target)
      -> std::optional<RegistrationMaterial> {
    const auto ambassador = from_hex("000102030405060708090a0b0c0d0e0f");
    auto derived =
        RegistrationMaterial::derive_local(target, ambassador, 0x01020304U);
    if (!derived) return std::nullopt;
    return std::move(derived).value();
  }

  static auto prefix_matches(std::span<const std::byte> payload,
                             std::string_view aero_hex, bool legacy)
      -> testing::AssertionResult {
    const auto aero = from_hex(aero_hex);
    if (payload.size() <= 480)
      return testing::AssertionFailure()
             << "the payload has only " << payload.size() << " bytes";
    for (std::size_t i = 0; i < 480; ++i) {
      if ((legacy && i >= 0x11c && i < 0x12c) ||
          (!legacy && ((i >= 0xc7 && i < 0xcf) || (i >= 0x191 && i < 0x199))))
        continue;
      if (payload[i] != static_cast<std::byte>('A'))
        return testing::AssertionFailure() << "byte " << i << " is not 'A'";
    }
    if (legacy) {
      if (!std::equal(aero.begin(), aero.end(), payload.begin() + 0x11c))
        return testing::AssertionFailure() << "the aeropause is not at 0x11c";
    } else {
      if (!std::equal(aero.begin() + 8, aero.end(), payload.begin() + 0xc7))
        return testing::AssertionFailure()
               << "the aeropause tail is not at 0xc7";
      if (!std::equal(aero.begin(), aero.begin() + 8, payload.begin() + 0x191))
        return testing::AssertionFailure()
               << "the aeropause head is not at 0x191";
    }
    return testing::AssertionSuccess();
  }

  libchiaking::platform::openssl::OpenSslCryptoProvider provider;
};

class OpenSslRegistrationPayload
    : public ::testing::TestWithParam<OpenSslRegistrationPayloadCase>,
      protected OpenSslRegistrationPayloadRig {};

TEST_P(OpenSslRegistrationPayload, MatchesTheReferenceVector) {
  const auto& golden = GetParam();
  const auto material = material_for(golden.target);
  ASSERT_TRUE(material);
  const auto result = RegistrationRequestCodec::encode_payload(
      provider, golden.target, *material, account(),
      golden.online ? std::optional<std::string_view>("synthetic-user")
                    : std::nullopt);
  ASSERT_TRUE(result);
  const auto& payload = result.value();
  EXPECT_EQ(payload.size(), golden.size);
  EXPECT_TRUE(prefix_matches(payload, golden.aeropause,
                             golden.target == RegistrationTarget::ps4_unknown ||
                                 golden.target == RegistrationTarget::ps4_8 ||
                                 golden.target == RegistrationTarget::ps4_9));
  ASSERT_GE(payload.size(), 480U);
  EXPECT_EQ(Bytes(payload.begin() + 480, payload.end()), from_hex(golden.tail));
}

INSTANTIATE_TEST_SUITE_P(
    EveryTargetAndIdentity, OpenSslRegistrationPayload,
    ::testing::Values(
        OpenSslRegistrationPayloadCase{
            RegistrationTarget::ps4_8, false,
            OpenSslRegistrationPayloadRig::tail_pre10_account,
            OpenSslRegistrationPayloadRig::aero_pre10, 530},
        OpenSslRegistrationPayloadCase{
            RegistrationTarget::ps4_8, true,
            OpenSslRegistrationPayloadRig::tail_pre10_online,
            OpenSslRegistrationPayloadRig::aero_pre10, 532},
        OpenSslRegistrationPayloadCase{
            RegistrationTarget::ps4_9, false,
            OpenSslRegistrationPayloadRig::tail_pre10_account,
            OpenSslRegistrationPayloadRig::aero_pre10, 530},
        OpenSslRegistrationPayloadCase{
            RegistrationTarget::ps4_unknown, false,
            OpenSslRegistrationPayloadRig::tail_pre10_account,
            OpenSslRegistrationPayloadRig::aero_pre10, 530},
        OpenSslRegistrationPayloadCase{
            RegistrationTarget::ps4_unknown, true,
            OpenSslRegistrationPayloadRig::tail_pre10_online,
            OpenSslRegistrationPayloadRig::aero_pre10, 532},
        OpenSslRegistrationPayloadCase{
            RegistrationTarget::ps4_10, false,
            OpenSslRegistrationPayloadRig::tail_ps4_10,
            OpenSslRegistrationPayloadRig::aero_ps4_10, 587},
        OpenSslRegistrationPayloadCase{
            RegistrationTarget::ps5_1, false,
            OpenSslRegistrationPayloadRig::tail_ps5_1,
            OpenSslRegistrationPayloadRig::aero_ps5_1, 587}));

class OpenSslRegistrationPayloadChecks
    : public ::testing::Test,
      protected OpenSslRegistrationPayloadRig {};

TEST_F(OpenSslRegistrationPayloadChecks, IgnoresTheOnlineIdOfAModernTarget) {
  const auto material = material_for(RegistrationTarget::ps5_1);
  ASSERT_TRUE(material);
  const auto with_online = RegistrationRequestCodec::encode_payload(
      provider, RegistrationTarget::ps5_1, *material, account(),
      std::optional<std::string_view>("ignored"));
  ASSERT_TRUE(with_online);
  EXPECT_EQ(with_online.value().size(), 587U);
}

TEST_F(OpenSslRegistrationPayloadChecks, RefusesAnAccountIdOfTheWrongSize) {
  const auto material = material_for(RegistrationTarget::ps4_8);
  ASSERT_TRUE(material);
  EXPECT_FALSE(RegistrationRequestCodec::encode_payload(
      provider, RegistrationTarget::ps4_8, *material, {}));
  const auto short_account = account();
  EXPECT_FALSE(RegistrationRequestCodec::encode_payload(
      provider, RegistrationTarget::ps4_8, *material,
      std::span<const std::byte>(short_account).first(7)));
  const Bytes long_account(9, std::byte(0));
  EXPECT_FALSE(RegistrationRequestCodec::encode_payload(
      provider, RegistrationTarget::ps4_8, *material, long_account));
}

TEST_F(OpenSslRegistrationPayloadChecks, RefusesAnUnknownTarget) {
  const auto material = material_for(RegistrationTarget::ps4_8);
  ASSERT_TRUE(material);
  const auto result = RegistrationRequestCodec::encode_payload(
      provider, static_cast<RegistrationTarget>(99), *material, account());
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error(), PayloadError::invalid_argument);
}

TEST_F(OpenSslRegistrationPayloadChecks, CarriesAnOnlineIdWithControlBytes) {
  const auto material = material_for(RegistrationTarget::ps4_8);
  ASSERT_TRUE(material);
  EXPECT_TRUE(RegistrationRequestCodec::encode_payload(
      provider, RegistrationTarget::ps4_8, *material, account(),
      std::optional<std::string_view>("raw\r\nid\x01\x7f\xff")));
}

TEST_F(OpenSslRegistrationPayloadChecks, RefusesAnOnlineIdWithANul) {
  const auto material = material_for(RegistrationTarget::ps4_8);
  ASSERT_TRUE(material);
  EXPECT_FALSE(RegistrationRequestCodec::encode_payload(
      provider, RegistrationTarget::ps4_8, *material, account(),
      std::optional<std::string_view>(std::string_view("nul\0id", 6))));
}

TEST_F(OpenSslRegistrationPayloadChecks, RefusesAnOverlongOnlineId) {
  const auto material = material_for(RegistrationTarget::ps4_8);
  ASSERT_TRUE(material);
  const std::string overlong(600, 'x');
  EXPECT_FALSE(RegistrationRequestCodec::encode_payload(
      provider, RegistrationTarget::ps4_8, *material, account(),
      std::optional<std::string_view>(overlong)));
}

TEST_F(OpenSslRegistrationPayloadChecks, AcceptsAnEmptyOnlineId) {
  const auto material = material_for(RegistrationTarget::ps4_8);
  ASSERT_TRUE(material);
  const auto empty = RegistrationRequestCodec::encode_payload(
      provider, RegistrationTarget::ps4_8, *material, account(),
      std::optional<std::string_view>(""));
  ASSERT_TRUE(empty);
  EXPECT_EQ(empty.value().size(), 480U + 13 + 7 + 2 + 14 + 2);
}

}  // namespace
}  // namespace remote_play_test
