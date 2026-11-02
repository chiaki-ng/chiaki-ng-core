#include "src/core/play/adapters/links/internet/internet_link_configuration.hpp"
#include "tests/unit/core/net/protocol/remote_play/stream/local_prepare_rig.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace remote_play_test {
namespace {

namespace play = libchiaking::play;
namespace platform = libchiaking::platform;
namespace remote_play = libchiaking::net::protocol::remote_play;
namespace takion = libchiaking::net::protocol::takion;

class LocalStreamPreparing : public LocalPrepareRig {
 protected:
  void SetUp() override {
    ASSERT_TRUE(id);
    ASSERT_TRUE(result);
  }

  auto inputs() -> remote_play::LocalStreamInputs {
    return {crypto,
            random,
            console,
            remote_play::VideoCodec::h264,
            {1280, 720, 60, 10000},
            *result,
            *id,
            nonce};
  }

  auto error_of(const remote_play::LocalStreamInputs& prepared_inputs)
      -> std::optional<remote_play::LocalPrepareError> {
    const auto prepared =
        remote_play::LocalStreamPreparation::prepare(prepared_inputs);
    if (prepared) return std::nullopt;
    return prepared.error();
  }

  LocalPrepareCrypto crypto;
  LocalPrepareRandom random;
  const std::optional<remote_play::SessionId> id = session_id();
  remote_play::RegisteredConsole console = registered_console();
  const std::array<std::byte, 16> nonce = ctrl_nonce();
  std::optional<SenkushaResult> result = calibration(12);
};

class LocalStreamPreparedMaterial
    : public LocalStreamPreparing,
      public ::testing::WithParamInterface<
          std::tuple<std::uint32_t, platform::EcdhCurve>> {};

TEST_P(LocalStreamPreparedMaterial, DerivesTheHandshakeKeyEcdhAndLaunchSpec) {
  const auto [version, expected_curve] = GetParam();
  result = calibration(version);
  ASSERT_TRUE(result);
  const auto prepared = remote_play::LocalStreamPreparation::prepare(inputs());
  ASSERT_TRUE(prepared);
  EXPECT_EQ(random.calls, 1U);
  EXPECT_EQ(crypto.generate_calls, 1U);
  EXPECT_EQ(crypto.generated_curve, expected_curve);
  EXPECT_EQ(prepared.value().prepared_ecdh.curve, expected_curve);
  EXPECT_EQ(prepared.value().session_key->view(), id->view());
  std::vector<std::byte> expected_key(16);
  for (std::size_t i = 0; i < 16; ++i) {
    expected_key[i] = static_cast<std::byte>(0x10 + i);
    EXPECT_EQ(prepared.value().handshake_key[i], expected_key[i])
        << "byte " << i;
  }
  EXPECT_EQ(crypto.ecdh_hmac_key, expected_key);
  EXPECT_GT(crypto.aes_calls, 0U);
  const auto expected_auth = remote_play::AuthMaterial::derive(
      console.target, nonce, {console.key.data(), console.key.size()});
  ASSERT_TRUE(expected_auth);
  EXPECT_EQ(crypto.launch_aes_key,
            std::vector<std::byte>(expected_auth.value().bright.begin(),
                                   expected_auth.value().bright.end()));
  ASSERT_EQ(crypto.launch_hmac_input.size(), 24U);
  EXPECT_TRUE(std::equal(expected_auth.value().ambassador.begin(),
                         expected_auth.value().ambassador.end(),
                         crypto.launch_hmac_input.begin()));
  const auto json =
      launch_spec_json(prepared.value().encrypted_launch_spec->view());
  ASSERT_TRUE(json);
  ASSERT_FALSE(json->empty());
  EXPECT_EQ(json->back(), '\0');
  EXPECT_NE(json->find("\"mtu\":1300"), std::string::npos);
  EXPECT_NE(json->find("\"rtt\":2"), std::string::npos);
  EXPECT_NE(json->find("\"handshakeKey\":\"EBESExQVFhcYGRobHB0eHw==\""),
            std::string::npos);
}

INSTANTIATE_TEST_SUITE_P(
    BootstrapCurves, LocalStreamPreparedMaterial,
    ::testing::Values(std::tuple{12U, platform::EcdhCurve::secp256k1},
                      std::tuple{20U, platform::EcdhCurve::secp521r1}));

TEST_F(LocalStreamPreparing, RefusesACalibrationWithoutAnMtu) {
  result->outbound_mtu = 0;
  EXPECT_EQ(error_of(inputs()), remote_play::LocalPrepareError::invalid_mtu);
  EXPECT_EQ(random.calls, 0U);
  EXPECT_EQ(crypto.generate_calls, 0U);
}

TEST_F(LocalStreamPreparing, RefusesANegativeRoundTrip) {
  result->round_trip_time.adjusted_mean = std::chrono::microseconds{-1};
  EXPECT_EQ(error_of(inputs()), remote_play::LocalPrepareError::invalid_rtt);
  EXPECT_EQ(random.calls, 0U);
  EXPECT_EQ(crypto.generate_calls, 0U);
}

TEST_F(LocalStreamPreparing, RefusesANonceOfTheWrongLength) {
  auto short_inputs = inputs();
  short_inputs.nonce = std::span<const std::byte>{nonce}.first(15);
  EXPECT_EQ(error_of(short_inputs),
            remote_play::LocalPrepareError::invalid_nonce);
  auto long_nonce = std::array<std::byte, 17>{};
  std::copy(nonce.begin(), nonce.end(), long_nonce.begin());
  auto long_inputs = inputs();
  long_inputs.nonce = long_nonce;
  EXPECT_EQ(error_of(long_inputs),
            remote_play::LocalPrepareError::invalid_nonce);
  EXPECT_EQ(random.calls, 0U);
  EXPECT_EQ(crypto.generate_calls, 0U);
}

TEST_F(LocalStreamPreparing, RefusesAnUnknownConsoleBeforeAnyRandomness) {
  console.target = static_cast<remote_play::RegistrationTarget>(0xff);
  EXPECT_EQ(error_of(inputs()),
            remote_play::LocalPrepareError::auth_material_failure);
  EXPECT_EQ(random.calls, 0U);
  EXPECT_EQ(crypto.generate_calls, 0U);
}

TEST_F(LocalStreamPreparing, ReportsARandomFailureBeforeTheKeyExchange) {
  random.fail = true;
  EXPECT_EQ(error_of(inputs()), remote_play::LocalPrepareError::random_failure);
  EXPECT_EQ(crypto.generate_calls, 0U);
  EXPECT_EQ(crypto.aes_calls, 0U);
}

TEST_F(LocalStreamPreparing, ReportsAKeyExchangeFailureBeforeTheLaunchSpec) {
  crypto.fail_generate = true;
  EXPECT_EQ(error_of(inputs()), remote_play::LocalPrepareError::ecdh_failure);
  EXPECT_EQ(crypto.aes_calls, 0U);
}

TEST_F(LocalStreamPreparing, ReportsALaunchSpecEncryptionFailure) {
  crypto.fail_aes = true;
  EXPECT_EQ(error_of(inputs()),
            remote_play::LocalPrepareError::launch_spec_failure);
}

TEST_F(LocalStreamPreparing, RefusesACalibrationForAnotherProtocolVersion) {
  result->takion_version = 20;
  EXPECT_EQ(error_of(inputs()),
            remote_play::LocalPrepareError::invalid_calibration);
}

TEST_F(LocalStreamPreparing, RefusesAnEmptySessionId) {
  const remote_play::SessionId empty_id;
  const remote_play::LocalStreamInputs empty_inputs{
      crypto,
      random,
      console,
      remote_play::VideoCodec::h264,
      {1280, 720, 60, 10000},
      *result,
      empty_id,
      nonce};
  EXPECT_EQ(error_of(empty_inputs),
            remote_play::LocalPrepareError::invalid_session_id);
}

class LocalStreamAudioChannels : public LocalStreamPreparing {
 protected:
  void SetUp() override {
    LocalStreamPreparing::SetUp();
    console.target = remote_play::RegistrationTarget::ps5_1;
    result = calibration(20);
    ASSERT_TRUE(result);
  }

  auto prepare(std::uint8_t channels) {
    LocalPrepareCrypto fresh_crypto;
    LocalPrepareRandom fresh_random;
    const remote_play::LocalStreamInputs channel_inputs{
        fresh_crypto,
        fresh_random,
        console,
        remote_play::VideoCodec::h264,
        {1280, 720, 60, 10000},
        *result,
        *id,
        nonce,
        channels};
    return remote_play::LocalStreamPreparation::prepare(channel_inputs);
  }
};

TEST_F(LocalStreamAudioChannels, ReachThePs5LaunchSpec) {
  const auto requested = prepare(8);
  ASSERT_TRUE(requested);
  const auto json =
      launch_spec_json(requested.value().encrypted_launch_spec->view());
  ASSERT_TRUE(json);
  EXPECT_NE(
      json->find(
          R"("audioChannelNumRP":2,"audioSettings":{"audioChannels":[{"name":"main","fecMode":1,)"),
      std::string::npos);
  EXPECT_NE(
      json->find(
          R"({"channels":8,"sampleRate":48000,"samplesPerFrame":480,"bitrate":256,"isRawPcm":false,"profileEnumType":2})"),
      std::string::npos);
  EXPECT_NE(
      json->find(
          R"({"channels":2,"sampleRate":48000,"samplesPerFrame":480,"bitrate":160,"isRawPcm":false,"profileEnumType":4}]}]},"handshakeKey")"),
      std::string::npos);
}

TEST_F(LocalStreamAudioChannels, AreLeftOutWhenNoneAreRequested) {
  const auto defaulted = prepare(0);
  ASSERT_TRUE(defaulted);
  const auto json =
      launch_spec_json(defaulted.value().encrypted_launch_spec->view());
  ASSERT_TRUE(json);
  EXPECT_EQ(json->find("audioSettings"), std::string::npos);
}

TEST_F(LocalStreamAudioChannels, RefuseAnUnsupportedLayout) {
  const auto refused = prepare(7);
  ASSERT_FALSE(refused);
  EXPECT_EQ(refused.error(),
            remote_play::LocalPrepareError::launch_spec_failure);
}

class LocalStreamFallbackCalibration
    : public LocalPrepareRig,
      public ::testing::WithParamInterface<remote_play::RegistrationTarget> {};

TEST_P(LocalStreamFallbackCalibration, MatchesTheReferenceDefaults) {
  const auto target = GetParam();
  const auto fallback =
      remote_play::LocalStreamPreparation::make_reference_fallback_calibration(
          target);
  ASSERT_TRUE(fallback);
  const auto version = console_version(target);
  const auto profile = takion::resolve_takion_wire_profile(version);
  ASSERT_TRUE(profile);
  EXPECT_EQ(fallback->takion_version, version);
  EXPECT_EQ(fallback->wire_profile.version(), profile->version());
  EXPECT_EQ(fallback->wire_profile.ecdh_curve(), profile->ecdh_curve());
  EXPECT_EQ(fallback->inbound_mtu, 1454U);
  EXPECT_EQ(fallback->outbound_mtu, 1454U);
  EXPECT_EQ(fallback->round_trip_time.adjusted_mean,
            std::chrono::microseconds{1000});
  EXPECT_EQ(fallback->round_trip_time.attempted, 0U);
}

INSTANTIATE_TEST_SUITE_P(
    Consoles, LocalStreamFallbackCalibration,
    ::testing::Values(remote_play::RegistrationTarget::ps4_8,
                      remote_play::RegistrationTarget::ps4_9,
                      remote_play::RegistrationTarget::ps4_10,
                      remote_play::RegistrationTarget::ps5_1));

class LocalStreamConsoleProfile : public LocalPrepareRig {
 protected:
  static auto active_version(remote_play::RegistrationTarget target,
                             std::uint32_t override_version)
      -> std::optional<std::uint32_t> {
    const auto profile =
        remote_play::LocalStreamPreparation::console_active_profile(
            target, override_version);
    if (!profile) return std::nullopt;
    return profile->version();
  }

  static constexpr auto ps5 = remote_play::RegistrationTarget::ps5_1;
  static constexpr auto ps4 = remote_play::RegistrationTarget::ps4_10;
};

TEST_F(LocalStreamConsoleProfile, HasNoFallbackCalibrationForAnUnknownConsole) {
  EXPECT_FALSE(
      remote_play::LocalStreamPreparation::make_reference_fallback_calibration(
          static_cast<remote_play::RegistrationTarget>(0xff)));
}

TEST_F(LocalStreamConsoleProfile, UsesTheConsoleVersionWithoutAnOverride) {
  EXPECT_EQ(active_version(ps5, 0),
            takion::select_takion_version(takion::takion_version_ps5, 0));
  EXPECT_EQ(active_version(ps4, 0),
            takion::select_takion_version(takion::takion_version_ps4, 0));
}

TEST_F(LocalStreamConsoleProfile, FollowsAnOverrideTheConsoleSpeaks) {
  EXPECT_EQ(active_version(ps5, 12), 12U);
  EXPECT_EQ(active_version(ps4, 20), 20U);
  EXPECT_EQ(active_version(ps4, 7), 7U);
  EXPECT_EQ(active_version(ps5, 8),
            takion::select_takion_version(takion::takion_version_ps5, 8));
  EXPECT_EQ(active_version(ps5, 22),
            takion::select_takion_version(takion::takion_version_ps5, 22));
  EXPECT_EQ(
      active_version(ps4, 0xffffffffU),
      takion::select_takion_version(takion::takion_version_ps4, 0xffffffffU));
}

TEST_F(LocalStreamConsoleProfile, IgnoresCloudStreamOnlyOverrides) {
  for (const std::uint32_t version : {21U, 23U, 25U}) {
    EXPECT_EQ(active_version(ps5, version),
              takion::select_takion_version(takion::takion_version_ps5, 0))
        << "version " << version;
    EXPECT_EQ(active_version(ps4, version),
              takion::select_takion_version(takion::takion_version_ps4, 0))
        << "version " << version;
    const auto chosen = active_version(ps5, version);
    ASSERT_TRUE(chosen) << "version " << version;
    EXPECT_FALSE(takion::is_cloud_stream_only_takion_version(*chosen))
        << "version " << version;
    const auto fallback = remote_play::LocalStreamPreparation::
        make_reference_fallback_calibration(ps5, version);
    ASSERT_TRUE(fallback) << "version " << version;
    EXPECT_EQ(fallback->takion_version,
              takion::select_takion_version(takion::takion_version_ps5, 0))
        << "version " << version;
  }
  if (takion::is_cloud_stream_only_takion_version(
          takion::takion_build_version_override)) {
    EXPECT_EQ(active_version(ps5, 0), takion::takion_version_ps5);
    EXPECT_EQ(active_version(ps4, 0), takion::takion_version_ps4);
  }
}

TEST_F(LocalStreamConsoleProfile, ShapesTheFallbackCalibrationForAnOverride) {
  const auto fallback =
      remote_play::LocalStreamPreparation::make_reference_fallback_calibration(
          ps4, 13);
  ASSERT_TRUE(fallback);
  EXPECT_EQ(fallback->takion_version, 13U);
  EXPECT_EQ(fallback->wire_profile.version(), 13U);
  EXPECT_EQ(fallback->wire_profile.ecdh_curve(),
            platform::EcdhCurve::secp521r1);
  const auto unknown =
      remote_play::LocalStreamPreparation::make_reference_fallback_calibration(
          ps5, 99);
  ASSERT_TRUE(unknown);
  EXPECT_EQ(unknown->takion_version,
            takion::select_takion_version(takion::takion_version_ps5, 99));
}

class LocalStreamDid : public LocalPrepareRig {
 protected:
  LocalPrepareRandom random;
  const std::array<std::byte, 32> zero{};
};

TEST_F(LocalStreamDid, IsGeneratedLikeTheReferenceWhenNoneIsSupplied) {
  const auto generated =
      remote_play::LocalStreamPreparation::resolve_local_did(zero, random);
  ASSERT_TRUE(generated);
  constexpr std::array<std::uint8_t, 10> prefix{0x00, 0x18, 0x00, 0x00, 0x00,
                                                0x07, 0x00, 0x40, 0x00, 0x80};
  for (std::size_t i = 0; i < prefix.size(); ++i)
    EXPECT_EQ((*generated)[i], static_cast<std::byte>(prefix[i]))
        << "byte " << i;
  EXPECT_TRUE(
      std::any_of(generated->begin() + 10, generated->begin() + 26,
                  [](std::byte value) { return value != std::byte{0}; }));
  for (std::size_t i = 26; i < 32; ++i)
    EXPECT_EQ((*generated)[i], std::byte{0}) << "byte " << i;
  EXPECT_EQ(random.calls, 1U);
}

TEST_F(LocalStreamDid, KeepsASuppliedDidWithoutRandomness) {
  std::array<std::byte, 32> supplied{};
  supplied[31] = std::byte{1};
  const auto kept =
      remote_play::LocalStreamPreparation::resolve_local_did(supplied, random);
  ASSERT_TRUE(kept);
  EXPECT_EQ(*kept, supplied);
  EXPECT_EQ(random.calls, 0U);
}

TEST_F(LocalStreamDid, FailsWhenRandomnessFails) {
  LocalPrepareRandom failing;
  failing.fail_on_call = 1;
  EXPECT_FALSE(
      remote_play::LocalStreamPreparation::resolve_local_did(zero, failing));
}

class LocalStreamDisabledVideo : public LocalPrepareRig {};

TEST_F(LocalStreamDisabledVideo, KeepsTheConfiguredVideoWhileEnabled) {
  play::HomeLinkConfiguration configuration;
  configuration.codec = remote_play::VideoCodec::h265_hdr;
  configuration.video = {1920, 1080, 60, 15000, false};
  const auto kept = remote_play::LocalStreamPreparation::apply_disabled_video(
      std::move(configuration));
  EXPECT_EQ(kept.codec, remote_play::VideoCodec::h265_hdr);
  EXPECT_EQ(kept.video.width, 1920U);
  EXPECT_EQ(kept.video.max_fps, 60U);
}

TEST_F(LocalStreamDisabledVideo, UsesTheReference360pPreset) {
  play::HomeLinkConfiguration configuration;
  configuration.codec = remote_play::VideoCodec::h265_hdr;
  configuration.video = {1920, 1080, 60, 15000, false};
  auto kept = remote_play::LocalStreamPreparation::apply_disabled_video(
      std::move(configuration));
  kept.tuning.disable_video = true;
  const auto disabled =
      remote_play::LocalStreamPreparation::apply_disabled_video(
          std::move(kept));
  EXPECT_EQ(disabled.codec, remote_play::VideoCodec::h264);
  EXPECT_EQ(disabled.video.width, 640U);
  EXPECT_EQ(disabled.video.height, 360U);
  EXPECT_EQ(disabled.video.bitrate_kbps, 2000U);
  EXPECT_EQ(disabled.video.max_fps, 0U);
  EXPECT_FALSE(disabled.video.auto_downgrade);
}

}  // namespace
}  // namespace remote_play_test
