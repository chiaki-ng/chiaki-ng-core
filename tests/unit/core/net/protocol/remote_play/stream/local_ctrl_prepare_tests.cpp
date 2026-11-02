#include "tests/unit/core/net/protocol/remote_play/stream/local_ctrl_rig.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <optional>
#include <span>
#include <tuple>
#include <type_traits>

namespace remote_play_test {
namespace {

namespace remote_play = libchiaking::net::protocol::remote_play;

static_assert(!std::is_copy_constructible_v<remote_play::PreparedLocalCtrl>);

class LocalCtrlPreparing : public LocalCtrlRig {
 protected:
  using Prepared =
      decltype(remote_play::LocalCtrlPreparation::prepare_from_nonce(
          std::declval<LocalCtrlCrypto&>(),
          std::declval<const remote_play::RegisteredConsole&>(), "", 0, did,
          remote_play::VideoCodec::h264, std::span<const std::byte>{}));

  static auto error_of(const Prepared& prepared)
      -> std::optional<remote_play::LocalCtrlPrepareError> {
    if (prepared) return std::nullopt;
    return prepared.error();
  }

  LocalCtrlCrypto crypto;
  const std::array<std::byte, 16> nonce = counting_nonce();
};

class LocalCtrlPreparedChain
    : public LocalCtrlPreparing,
      public ::testing::WithParamInterface<std::tuple<
          remote_play::RegistrationTarget, remote_play::VideoCodec>> {};

TEST_P(LocalCtrlPreparedChain, MatchesTheReferenceDerivationAndRequest) {
  const auto [target, codec] = GetParam();
  const auto registered = console(target);
  const auto prepared = remote_play::LocalCtrlPreparation::prepare_from_nonce(
      crypto, registered, "127.0.0.1", 9295, did, codec, nonce);
  ASSERT_TRUE(prepared);
  EXPECT_GT(crypto.calls, 0U);
  for (std::size_t i = 0; i < 16; ++i)
    EXPECT_EQ(prepared.value().nonce[i], static_cast<std::byte>(i + 1))
        << "byte " << i;

  const auto reference_material = remote_play::AuthMaterial::derive(
      target, {prepared.value().nonce.data(), prepared.value().nonce.size()},
      {registered.key.data(), registered.key.size()});
  ASSERT_TRUE(reference_material);
  for (std::size_t i = 0; i < 16; ++i) {
    EXPECT_EQ(prepared.value().material.bright[i],
              reference_material.value().bright[i])
        << "byte " << i;
    EXPECT_EQ(prepared.value().material.ambassador[i],
              reference_material.value().ambassador[i])
        << "byte " << i;
  }
  const auto reference_request = remote_play::CtrlCodec::encode_startup_request(
      crypto, target, reference_material.value(), "127.0.0.1", 9295,
      {registered.registration_key.data(), registered.registration_key.size()},
      did, codec);
  ASSERT_TRUE(reference_request);
  EXPECT_EQ(prepared.value().request.view(), reference_request.value().view());
  EXPECT_EQ(prepared.value().request.next_crypt_counter(),
            reference_request.value().next_crypt_counter());
}

INSTANTIATE_TEST_SUITE_P(
    ConsolesAndCodecs, LocalCtrlPreparedChain,
    ::testing::Values(std::tuple{remote_play::RegistrationTarget::ps4_8,
                                 remote_play::VideoCodec::h264},
                      std::tuple{remote_play::RegistrationTarget::ps4_9,
                                 remote_play::VideoCodec::h264},
                      std::tuple{remote_play::RegistrationTarget::ps4_10,
                                 remote_play::VideoCodec::h265},
                      std::tuple{remote_play::RegistrationTarget::ps5_1,
                                 remote_play::VideoCodec::h265_hdr}));

TEST_F(LocalCtrlPreparing, RefusesInvalidInputBeforeDerivingAnything) {
  auto registered = console(remote_play::RegistrationTarget::ps5_1);
  EXPECT_EQ(error_of(remote_play::LocalCtrlPreparation::prepare_from_nonce(
                crypto, registered, "", 9295, did,
                remote_play::VideoCodec::h264, nonce)),
            remote_play::LocalCtrlPrepareError::invalid_host);
  EXPECT_EQ(error_of(remote_play::LocalCtrlPreparation::prepare_from_nonce(
                crypto, registered, "host\r\nX:y", 9295, did,
                remote_play::VideoCodec::h264, nonce)),
            remote_play::LocalCtrlPrepareError::invalid_host);
  EXPECT_EQ(error_of(remote_play::LocalCtrlPreparation::prepare_from_nonce(
                crypto, registered, " ", 9295, did,
                remote_play::VideoCodec::h264, nonce)),
            remote_play::LocalCtrlPrepareError::invalid_host);
  EXPECT_EQ(error_of(remote_play::LocalCtrlPreparation::prepare_from_nonce(
                crypto, registered, "127.0.0.1", 0, did,
                remote_play::VideoCodec::h264, nonce)),
            remote_play::LocalCtrlPrepareError::invalid_port);
  EXPECT_EQ(error_of(remote_play::LocalCtrlPreparation::prepare_from_nonce(
                crypto, registered, "127.0.0.1", 9295, std::span(did).first(31),
                remote_play::VideoCodec::h264, nonce)),
            remote_play::LocalCtrlPrepareError::invalid_did);
  const std::array<std::byte, 33> long_did{};
  EXPECT_EQ(error_of(remote_play::LocalCtrlPreparation::prepare_from_nonce(
                crypto, registered, "127.0.0.1", 9295, long_did,
                remote_play::VideoCodec::h264, nonce)),
            remote_play::LocalCtrlPrepareError::invalid_did);
  EXPECT_EQ(error_of(remote_play::LocalCtrlPreparation::prepare_from_nonce(
                crypto, registered, "127.0.0.1", 9295, did,
                static_cast<remote_play::VideoCodec>(99), nonce)),
            remote_play::LocalCtrlPrepareError::invalid_codec);
  registered.target = static_cast<remote_play::RegistrationTarget>(99);
  EXPECT_EQ(error_of(remote_play::LocalCtrlPreparation::prepare_from_nonce(
                crypto, registered, "127.0.0.1", 9295, did,
                remote_play::VideoCodec::h264, nonce)),
            remote_play::LocalCtrlPrepareError::invalid_target);
  EXPECT_EQ(crypto.calls, 0U);
}

TEST_F(LocalCtrlPreparing, ReportsACryptoFailureAsARequestFailure) {
  const auto registered = console(remote_play::RegistrationTarget::ps4_9);
  crypto.fail = true;
  EXPECT_EQ(error_of(remote_play::LocalCtrlPreparation::prepare_from_nonce(
                crypto, registered, "127.0.0.1", 9295, did,
                remote_play::VideoCodec::h264, nonce)),
            remote_play::LocalCtrlPrepareError::request_failure);
  EXPECT_GT(crypto.calls, 0U);
}

TEST_F(LocalCtrlPreparing, DerivesFromTheSuppliedNonce) {
  const auto registered = console(remote_play::RegistrationTarget::ps5_1);
  std::array<std::byte, 16> first{};
  std::array<std::byte, 16> second{};
  for (std::size_t i = 0; i < first.size(); ++i) {
    first[i] = static_cast<std::byte>(i + 1);
    second[i] = static_cast<std::byte>(i + 33);
  }
  const auto a = remote_play::LocalCtrlPreparation::prepare_from_nonce(
      crypto, registered, "127.0.0.1", 9295, did, remote_play::VideoCodec::h265,
      first);
  const auto b = remote_play::LocalCtrlPreparation::prepare_from_nonce(
      crypto, registered, "127.0.0.1", 9295, did, remote_play::VideoCodec::h265,
      second);
  ASSERT_TRUE(a);
  ASSERT_TRUE(b);
  for (std::size_t i = 0; i < first.size(); ++i)
    EXPECT_EQ(a.value().nonce[i], first[i]) << "byte " << i;
  EXPECT_NE(a.value().material.bright[0], b.value().material.bright[0]);
  EXPECT_EQ(error_of(remote_play::LocalCtrlPreparation::prepare_from_nonce(
                crypto, registered, "127.0.0.1", 9295, did,
                remote_play::VideoCodec::h265, std::span(first).first(15))),
            remote_play::LocalCtrlPrepareError::invalid_nonce);
}

}  // namespace
}  // namespace remote_play_test
