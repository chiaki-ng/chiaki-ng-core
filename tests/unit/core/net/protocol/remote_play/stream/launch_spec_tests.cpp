#include "src/core/net/protocol/remote_play/stream/launch_spec.hpp"
#include "src/core/utils/text/base64.hpp"
#include "tests/support/enum_printing.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace remote_play_test {
namespace {

namespace platform = libchiaking::platform;
namespace remote_play = libchiaking::net::protocol::remote_play;
namespace base64 = libchiaking::text::base64;

class LaunchSpecZeroBlockCrypto final : public platform::CryptoProvider {
 public:
  auto sha256(std::span<const std::byte>, std::span<std::byte>)
      -> libchiaking::Expected<void, platform::CryptoError> override {
    return libchiaking::unexpected(platform::CryptoError::backend_failure);
  }
  auto hmac_sha256(std::span<const std::byte>, std::span<const std::byte> input,
                   std::span<std::byte> digest)
      -> libchiaking::Expected<void, platform::CryptoError> override {
    hmac_input.assign(input.begin(), input.end());
    if (fail)
      return libchiaking::unexpected(platform::CryptoError::backend_failure);
    for (auto& byte : digest) byte = std::byte{0};
    return {};
  }
  auto aes128_encrypt_block(std::span<const std::byte> key,
                            std::span<const std::byte> input,
                            std::span<std::byte> output)
      -> libchiaking::Expected<void, platform::CryptoError> override {
    aes_key.assign(key.begin(), key.end());
    if (fail)
      return libchiaking::unexpected(platform::CryptoError::backend_failure);
    for (std::size_t i = 0; i < output.size(); ++i)
      output[i] = fake_feedback ? input[i] ^ std::byte{0x5a} : std::byte{0};
    return {};
  }
  auto aes128_gmac(std::span<const std::byte>, std::span<const std::byte>,
                   std::span<const std::byte>, std::span<std::byte>)
      -> libchiaking::Expected<void, platform::CryptoError> override {
    return libchiaking::unexpected(platform::CryptoError::backend_failure);
  }

  std::vector<std::byte> hmac_input;
  std::vector<std::byte> aes_key;
  bool fail{false};
  bool fake_feedback{false};
};

class LaunchSpecEncoding : public ::testing::Test {
 protected:
  static constexpr std::string_view handshake_key_json =
      R"("handshakeKey":"AAECAwQFBgcICQoLDA0ODw=="})";
  static constexpr std::string_view hevc_auto =
      R"("videoCodec":"hevc","dynamicRange":"AUTO")";

  static auto decode_base64(std::string_view encoded)
      -> std::optional<std::vector<std::byte>> {
    if (encoded.size() % 4 != 0) return std::nullopt;
    std::vector<std::byte> result(encoded.size() / 4 * 3);
    const auto written =
        base64::decode_into(encoded, result, base64::Alphabet::standard);
    if (!written) return std::nullopt;
    result.resize(written.value());
    return result;
  }

  static auto material() -> remote_play::AuthMaterial {
    remote_play::AuthMaterial result;
    for (std::size_t i = 0; i < 16; ++i) {
      result.bright[i] = static_cast<std::byte>(0x20 + i);
      result.ambassador[i] = static_cast<std::byte>(0x40 + i);
    }
    return result;
  }

  static auto handshake_key() -> std::array<std::byte, 16> {
    std::array<std::byte, 16> result{};
    for (std::size_t i = 0; i < result.size(); ++i)
      result[i] = static_cast<std::byte>(i);
    return result;
  }

  static auto expected_ps4_json() -> std::string {
    return R"({"sessionId":"sessionId4321","streamResolutions":[{"resolution":{"width":1280,"height":720},"maxFps":60,"score":10}],"network":{"bwKbpsSent":10000,"bwLoss":0.001,"mtu":1344,"rtt":2,"ports":[53,2053]},"slotId":1,"appSpecification":{"minFps":30,"minBandwidth":0,"extTitleId":"ps3","version":1,"timeLimit":1,"startTimeout":100,"afkTimeout":100,"afkTimeoutDisconnect":100},"konan":{"ps3AccessToken":"accessToken","ps3RefreshToken":"refreshToken"},"requestGameSpecification":{"model":"bravia_tv","platform":"android","audioChannels":"5.1","language":"sp","acceptButton":"X","connectedControllers":["xinput","ds3","ds4"],"yuvCoefficient":"bt601","videoEncoderProfile":"hw4.1","audioEncoderProfile":"audio1"},"userProfile":{"onlineId":"psnId","npId":"npId","region":"US","languagesUsed":["en","jp"]},"handshakeKey":"AAECAwQFBgcICQoLDA0ODw=="})";
  }

  static auto expected_ps5_json() -> std::string {
    return R"({"sessionId":"sessionId4321","streamResolutions":[{"resolution":{"width":1280,"height":720},"maxFps":60,"score":10}],"network":{"bwKbpsSent":10000,"bwLoss":0.001,"mtu":1344,"rtt":2,"ports":[53,2053]},"slotId":1,"appSpecification":{"minFps":30,"minBandwidth":0,"extTitleId":"ps3","version":1,"timeLimit":1,"startTimeout":100,"afkTimeout":100,"afkTimeoutDisconnect":100},"konan":{"ps3AccessToken":"accessToken","ps3RefreshToken":"refreshToken"},"requestGameSpecification":{"model":"bravia_tv","platform":"android","audioChannels":"5.1","language":"sp","acceptButton":"X","connectedControllers":["xinput","ds3","ds4"],"yuvCoefficient":"bt601","videoEncoderProfile":"hw4.1","audioEncoderProfile":"audio1","adaptiveStreamMode":"resize"},"userProfile":{"onlineId":"psnId","npId":"npId","region":"US","languagesUsed":["en","jp"]},"videoCodec":"hevc","dynamicRange":"AUTO",)";
  }

  static auto expected_audio_settings(unsigned channels) -> std::string {
    const std::string profiles =
        R"("audioSettings":{"audioChannels":[{"name":"main","fecMode":1,"settings":[{"channels":2,"sampleRate":48000,"samplesPerFrame":480,"bitrate":160,"isRawPcm":false,"profileEnumType":0},{"channels":6,"sampleRate":48000,"samplesPerFrame":480,"bitrate":256,"isRawPcm":false,"profileEnumType":1},{"channels":8,"sampleRate":48000,"samplesPerFrame":480,"bitrate":256,"isRawPcm":false,"profileEnumType":2},{"channels":12,"sampleRate":48000,"samplesPerFrame":480,"bitrate":256,"isRawPcm":false,"profileEnumType":3},{"channels":2,"sampleRate":48000,"samplesPerFrame":480,"bitrate":160,"isRawPcm":false,"profileEnumType":4}]}]},)";
    switch (channels) {
      case 2:
        return R"("audioChannelNumRP":0,)" + profiles;
      case 6:
        return R"("audioChannelNumRP":1,)" + profiles;
      case 8:
        return R"("audioChannelNumRP":2,)" + profiles;
      case 12:
        return R"("audioChannelNumRP":3,)" + profiles;
      default:
        ADD_FAILURE() << "no audio settings for " << channels << " channels";
        return {};
    }
  }

  void expect_exact_json(remote_play::RegistrationTarget target,
                         remote_play::VideoCodec codec,
                         const std::string& expected,
                         std::uint8_t requested_audio_channels = 0) {
    LaunchSpecZeroBlockCrypto crypto;
    const auto output = remote_play::LaunchSpecCodec::encode(
        crypto, target, codec, video, 1344, 2005, key, auth,
        requested_audio_channels);
    ASSERT_TRUE(output);
    const auto plain = decode_base64(output.value().view());
    ASSERT_TRUE(plain);
    ASSERT_EQ(plain->size(), expected.size() + 1);
    EXPECT_EQ(plain->back(), std::byte{0});
    EXPECT_EQ(std::string(reinterpret_cast<const char*>(plain->data()),
                          plain->size() - 1),
              expected);
    ASSERT_EQ(crypto.hmac_input.size(), 24U);
    ASSERT_GE(crypto.aes_key.size(), 16U);
    for (std::size_t i = 0; i < 16; ++i) {
      EXPECT_EQ(crypto.hmac_input[i], auth.ambassador[i]) << "byte " << i;
      EXPECT_EQ(crypto.aes_key[i], auth.bright[i]) << "byte " << i;
    }
    for (std::size_t i = 16; i < 24; ++i)
      EXPECT_EQ(crypto.hmac_input[i], std::byte{0}) << "byte " << i;
  }

  auto encode(LaunchSpecZeroBlockCrypto& crypto,
              remote_play::RegistrationTarget target,
              remote_play::VideoCodec codec,
              const remote_play::LaunchSpecVideo& spec_video, std::uint32_t mtu,
              std::uint64_t rtt, std::span<const std::byte> spec_key) const {
    return remote_play::LaunchSpecCodec::encode(
        crypto, target, codec, spec_video, mtu, rtt, spec_key, auth);
  }

  const remote_play::AuthMaterial auth = material();
  const std::array<std::byte, 16> key = handshake_key();
  const remote_play::LaunchSpecVideo video{1280, 720, 60, 10000};
};

class LaunchSpecPs5Json : public LaunchSpecEncoding {
 protected:
  void SetUp() override {
    ps5 = expected_ps4_json();
    constexpr std::string_view audio_profile =
        R"("audioEncoderProfile":"audio1")";
    const auto audio = ps5.find(audio_profile);
    ASSERT_NE(audio, std::string::npos);
    ps5.insert(audio + audio_profile.size(),
               R"(,"adaptiveStreamMode":"resize")");
    const auto handshake = ps5.find(R"("handshakeKey")");
    ASSERT_NE(handshake, std::string::npos);
    ps5.insert(handshake, std::string{hevc_auto} + ",");
    hevc = ps5.find(hevc_auto);
    ASSERT_NE(hevc, std::string::npos);
  }

  auto with_codec(std::string_view replacement) const -> std::string {
    auto result = ps5;
    result.replace(hevc, hevc_auto.size(), replacement);
    return result;
  }

  std::string ps5;
  std::size_t hevc{};
};

TEST_F(LaunchSpecEncoding, WritesTheExactPs4JsonThroughTheProvider) {
  ASSERT_NO_FATAL_FAILURE(
      expect_exact_json(remote_play::RegistrationTarget::ps4_9,
                        remote_play::VideoCodec::h264, expected_ps4_json()));
}

TEST_F(LaunchSpecPs5Json, AddsTheAdaptiveModeAndHdrHevcForAPs5) {
  ASSERT_NO_FATAL_FAILURE(
      expect_exact_json(remote_play::RegistrationTarget::ps5_1,
                        remote_play::VideoCodec::h265_hdr, ps5));
}

TEST_F(LaunchSpecPs5Json, DeclaresSdrAvcForH264) {
  ASSERT_NO_FATAL_FAILURE(expect_exact_json(
      remote_play::RegistrationTarget::ps5_1, remote_play::VideoCodec::h264,
      with_codec(R"("videoCodec":"avc","dynamicRange":"SDR")")));
}

TEST_F(LaunchSpecPs5Json, DeclaresSdrHevcForH265) {
  ASSERT_NO_FATAL_FAILURE(expect_exact_json(
      remote_play::RegistrationTarget::ps5_1, remote_play::VideoCodec::h265,
      with_codec(R"("videoCodec":"hevc","dynamicRange":"SDR")")));
}

TEST_F(LaunchSpecEncoding, XorsTheJsonWithANonZeroFeedbackStream) {
  LaunchSpecZeroBlockCrypto crypto;
  crypto.fake_feedback = true;
  const auto expected = expected_ps4_json();
  const auto output =
      encode(crypto, remote_play::RegistrationTarget::ps4_9,
             remote_play::VideoCodec::h264, video, 1344, 2005, key);
  ASSERT_TRUE(output);
  const auto wire = decode_base64(output.value().view());
  ASSERT_TRUE(wire);
  EXPECT_EQ(wire->size(), expected.size() + 1);
  EXPECT_GT(wire->size(), 32U);
  for (std::size_t i = 0; i < wire->size(); ++i) {
    const auto stream_byte = (i / 16) % 2 == 0 ? std::byte{0x5a} : std::byte{0};
    const auto json_byte = i < expected.size()
                               ? static_cast<std::byte>(expected[i])
                               : std::byte{0};
    EXPECT_EQ((*wire)[i], stream_byte ^ json_byte) << "byte " << i;
  }
}

TEST_F(LaunchSpecEncoding, RefusesAnUnknownTarget) {
  LaunchSpecZeroBlockCrypto crypto;
  const auto result =
      encode(crypto, static_cast<remote_play::RegistrationTarget>(99),
             remote_play::VideoCodec::h264, video, 1344, 2005, key);
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error(), remote_play::LaunchSpecError::invalid_target);
}

TEST_F(LaunchSpecEncoding, RefusesAnUnknownCodec) {
  LaunchSpecZeroBlockCrypto crypto;
  const auto result =
      encode(crypto, remote_play::RegistrationTarget::ps4_9,
             static_cast<remote_play::VideoCodec>(99), video, 1344, 2005, key);
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error(), remote_play::LaunchSpecError::invalid_codec);
}

TEST_F(LaunchSpecEncoding, AcceptsZeroSizeBitrateAndFrameRate) {
  LaunchSpecZeroBlockCrypto crypto;
  EXPECT_TRUE(encode(crypto, remote_play::RegistrationTarget::ps4_9,
                     remote_play::VideoCodec::h264, {0, 0, 60, 0}, 1344, 2005,
                     key));
  EXPECT_TRUE(encode(crypto, remote_play::RegistrationTarget::ps4_9,
                     remote_play::VideoCodec::h264, {640, 360, 0, 2000}, 1344,
                     2005, key));
}

TEST_F(LaunchSpecEncoding, RefusesAZeroMtu) {
  LaunchSpecZeroBlockCrypto crypto;
  const auto result =
      encode(crypto, remote_play::RegistrationTarget::ps4_9,
             remote_play::VideoCodec::h264, video, 0, 2005, key);
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error(), remote_play::LaunchSpecError::invalid_mtu);
}

TEST_F(LaunchSpecEncoding, RefusesAShortHandshakeKey) {
  LaunchSpecZeroBlockCrypto crypto;
  const auto result = encode(crypto, remote_play::RegistrationTarget::ps4_9,
                             remote_play::VideoCodec::h264, video, 1344, 2005,
                             std::span(key).first(15));
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error(),
            remote_play::LaunchSpecError::invalid_handshake_key);
}

TEST_F(LaunchSpecEncoding, RefusesARoundTripBeyondThirtyTwoBitMilliseconds) {
  LaunchSpecZeroBlockCrypto crypto;
  const auto result = encode(
      crypto, remote_play::RegistrationTarget::ps4_9,
      remote_play::VideoCodec::h264, video, 1344,
      (static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max()) +
       1) *
          1000,
      key);
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error(), remote_play::LaunchSpecError::invalid_rtt);
}

TEST_F(LaunchSpecEncoding, ReportsAProviderFailure) {
  LaunchSpecZeroBlockCrypto crypto;
  crypto.fail = true;
  const auto result =
      encode(crypto, remote_play::RegistrationTarget::ps4_9,
             remote_play::VideoCodec::h264, video, 1344, 2005, key);
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error(), remote_play::LaunchSpecError::crypto_failure);
}

TEST_F(LaunchSpecEncoding, OmitsAudioSettingsWhenNoChannelsAreRequested) {
  ASSERT_NO_FATAL_FAILURE(expect_exact_json(
      remote_play::RegistrationTarget::ps5_1, remote_play::VideoCodec::h265_hdr,
      expected_ps5_json() + std::string{handshake_key_json}, 0));
}

TEST_F(LaunchSpecEncoding, DeclaresTheMainAudioLaneForRequestedChannels) {
  for (const auto channels : std::initializer_list<std::uint8_t>{2, 6, 8, 12}) {
    SCOPED_TRACE(testing::Message() << unsigned{channels} << " channels");
    ASSERT_NO_FATAL_FAILURE(expect_exact_json(
        remote_play::RegistrationTarget::ps5_1,
        remote_play::VideoCodec::h265_hdr,
        expected_ps5_json() + expected_audio_settings(channels) +
            std::string{handshake_key_json},
        channels));
  }
}

TEST_F(LaunchSpecEncoding, DescribesTwelveChannelsAsProfileThree) {
  EXPECT_EQ(
      expected_audio_settings(12),
      R"("audioChannelNumRP":3,"audioSettings":{"audioChannels":[{"name":"main","fecMode":1,"settings":[{"channels":2,"sampleRate":48000,"samplesPerFrame":480,"bitrate":160,"isRawPcm":false,"profileEnumType":0},{"channels":6,"sampleRate":48000,"samplesPerFrame":480,"bitrate":256,"isRawPcm":false,"profileEnumType":1},{"channels":8,"sampleRate":48000,"samplesPerFrame":480,"bitrate":256,"isRawPcm":false,"profileEnumType":2},{"channels":12,"sampleRate":48000,"samplesPerFrame":480,"bitrate":256,"isRawPcm":false,"profileEnumType":3},{"channels":2,"sampleRate":48000,"samplesPerFrame":480,"bitrate":160,"isRawPcm":false,"profileEnumType":4}]}]},)");
}

TEST_F(LaunchSpecEncoding, IgnoresRequestedChannelsForAPs4) {
  for (const auto channels :
       std::initializer_list<std::uint8_t>{0, 2, 6, 8, 12}) {
    SCOPED_TRACE(testing::Message() << unsigned{channels} << " channels");
    ASSERT_NO_FATAL_FAILURE(expect_exact_json(
        remote_play::RegistrationTarget::ps4_9, remote_play::VideoCodec::h264,
        expected_ps4_json(), channels));
  }
}

TEST_F(LaunchSpecEncoding, RefusesUnsupportedChannelCounts) {
  LaunchSpecZeroBlockCrypto crypto;
  for (unsigned channels = 0; channels < 256; ++channels) {
    const auto requested = static_cast<std::uint8_t>(channels);
    for (const auto target : {remote_play::RegistrationTarget::ps5_1,
                              remote_play::RegistrationTarget::ps4_10}) {
      const auto output = remote_play::LaunchSpecCodec::encode(
          crypto, target, remote_play::VideoCodec::h264, video, 1344, 2005, key,
          auth, requested);
      const bool supported = channels == 0 || channels == 2 || channels == 6 ||
                             channels == 8 || channels == 12;
      EXPECT_EQ(output.has_value(), supported) << channels << " channels";
      if (!supported && !output) {
        EXPECT_EQ(output.error(),
                  remote_play::LaunchSpecError::invalid_audio_channels)
            << channels << " channels";
      }
    }
  }
}

TEST_F(LaunchSpecEncoding, FitsTheLargestRequestInTheJsonCapacity) {
  LaunchSpecZeroBlockCrypto crypto;
  const remote_play::LaunchSpecVideo largest{0xffffffffU, 0xffffffffU,
                                             0xffffffffU, 0xffffffffU};
  const auto output = remote_play::LaunchSpecCodec::encode(
      crypto, remote_play::RegistrationTarget::ps5_1,
      remote_play::VideoCodec::h265_hdr, largest, 0xffffffffU,
      static_cast<std::uint64_t>(0xffffffffU) * 1000, key, auth, 12);
  ASSERT_TRUE(output);
  const auto plain = decode_base64(output.value().view());
  ASSERT_TRUE(plain);
  ASSERT_FALSE(plain->empty());
  const std::string json(reinterpret_cast<const char*>(plain->data()),
                         plain->size() - 1);
  EXPECT_NE(json.find(expected_audio_settings(12)), std::string::npos);
  EXPECT_TRUE(json.ends_with(handshake_key_json));
}

}  // namespace
}  // namespace remote_play_test
