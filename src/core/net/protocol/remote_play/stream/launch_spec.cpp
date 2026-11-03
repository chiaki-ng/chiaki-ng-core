#include "launch_spec.hpp"

#include "libchiaking/detail/secret_array.hpp"
#include "libchiaking/platform/runtime/time_units.hpp"
#include "src/core/net/protocol/remote_play/ecdh_wire.hpp"
#include "src/core/net/protocol/remote_play/pairing/cipher.hpp"
#include "src/core/net/protocol/remote_play/stream/audio_layout.hpp"
#include "src/core/net/protocol/remote_play/stream/limits.hpp"
#include "src/core/utils/json/ps_reply.hpp"
#include "src/core/utils/text/base64.hpp"

#include <glaze/util/key_transformers.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <functional>
#include <limits>
#include <span>
#include <string_view>
#include <utility>

namespace libchiaking::net::protocol::remote_play {

struct LaunchSpecCodec::Document {
  struct Resolution {
    using keys = json::ps::CamelCaseKeys;
    std::uint32_t width{};
    std::uint32_t height{};
  };
  struct StreamResolution {
    using keys = json::ps::CamelCaseKeys;
    Resolution resolution{};
    std::uint32_t max_fps{};
    int score{10};
  };
  struct Network {
    using keys = json::ps::CamelCaseKeys;
    std::uint32_t bw_kbps_sent{};
    double bw_loss{0.001};
    std::uint32_t mtu{};
    std::uint32_t rtt{};
    std::array<int, 2> ports{53, 2053};
  };
  struct AppSpecification {
    using keys = json::ps::CamelCaseKeys;
    int min_fps{30};
    int min_bandwidth{0};
    std::string_view ext_title_id{"ps3"};
    int version{1};
    int time_limit{1};
    int start_timeout{100};
    int afk_timeout{100};
    int afk_timeout_disconnect{100};
  };
  struct Konan {
    using keys = json::ps::CamelCaseKeys;
    std::string_view ps3_access_token{"accessToken"};
    std::string_view ps3_refresh_token{"refreshToken"};
  };
  struct RequestProfile {
    using keys = json::ps::CamelCaseKeys;
    std::string_view model{"bravia_tv"};
    std::string_view platform{"android"};
    std::string_view audio_channels{"5.1"};
    std::string_view language{"sp"};
    std::string_view accept_button{"X"};
    std::array<std::string_view, 3> connected_controllers{"xinput", "ds3",
                                                          "ds4"};
    std::string_view yuv_coefficient{"bt601"};
    std::string_view video_encoder_profile{"hw4.1"};
    std::string_view audio_encoder_profile{"audio1"};
    std::optional<std::string_view> adaptive_stream_mode{};
  };
  struct UserProfile {
    using keys = json::ps::CamelCaseKeys;
    std::string_view online_id{"psnId"};
    std::string_view np_id{"npId"};
    std::string_view region{"US"};
    std::array<std::string_view, 2> languages_used{"en", "jp"};
  };
  struct AudioProfile {
    using keys = json::ps::CamelCaseKeys;
    int channels{};
    int sample_rate{48000};
    int samples_per_frame{480};
    int bitrate{};
    bool is_raw_pcm{};
    int profile_enum_type{};
  };
  struct AudioLane {
    using keys = json::ps::CamelCaseKeys;
    std::string_view name{"main"};
    int fec_mode{1};
    std::array<AudioProfile, 5> settings{
        AudioProfile{.channels = 2, .bitrate = 160},
        AudioProfile{.channels = 6, .bitrate = 256, .profile_enum_type = 1},
        AudioProfile{.channels = 8, .bitrate = 256, .profile_enum_type = 2},
        AudioProfile{.channels = 12, .bitrate = 256, .profile_enum_type = 3},
        AudioProfile{.channels = 2, .bitrate = 160, .profile_enum_type = 4}};
  };
  struct AudioSettings {
    using keys = json::ps::CamelCaseKeys;
    std::array<AudioLane, 1> audio_channels{};
  };

  std::string_view session_id{"sessionId4321"};
  std::array<StreamResolution, 1> stream_resolutions{};
  Network network{};
  int slot_id{1};
  AppSpecification app_specification{};
  Konan konan{};
  RequestProfile request_game_specification{};
  UserProfile user_profile{};
  std::optional<std::string_view> video_codec{};
  std::optional<std::string_view> dynamic_range{};
  std::optional<unsigned> audio_channel_num_rp{};
  std::optional<AudioSettings> audio_settings{};
  SecretString handshake_key{};
};

}  // namespace libchiaking::net::protocol::remote_play

template <>
struct glz::meta<
    libchiaking::net::protocol::remote_play::LaunchSpecCodec::Document> {
  static constexpr auto rename_key(const auto key) -> std::string {
    if (std::string_view{key} == "audio_channel_num_rp")
      return "audioChannelNumRP";
    return glz::to_camel_case(key);
  }
};

namespace libchiaking::net::protocol::remote_play {

auto LaunchSpecCodec::encode(platform::CryptoProvider& crypto,
                             RegistrationTarget target, VideoCodec codec,
                             const LaunchSpecVideo& video, std::uint32_t mtu,
                             std::uint64_t rtt_microseconds,
                             std::span<const std::byte> handshake_key,
                             const AuthMaterial& material,
                             std::uint8_t requested_audio_channels)
    -> Expected<SecretString, LaunchSpecError> {
  const auto ps5 = is_ps5(target);
  if (!ps5) return unexpected(ps5.error());
  std::string_view video_codec;
  std::string_view dynamic_range;
  switch (codec) {
    case VideoCodec::h264:
      video_codec = "avc";
      dynamic_range = "SDR";
      break;
    case VideoCodec::h265:
      video_codec = "hevc";
      dynamic_range = "SDR";
      break;
    case VideoCodec::h265_hdr:
      video_codec = "hevc";
      dynamic_range = "AUTO";
      break;
    default:
      return unexpected(LaunchSpecError::invalid_codec);
  }
  if (!valid_requested_audio_channels(requested_audio_channels))
    return unexpected(LaunchSpecError::invalid_audio_channels);
  if (mtu == 0) return unexpected(LaunchSpecError::invalid_mtu);
  if (handshake_key.size() != ecdh_handshake_key_size)
    return unexpected(LaunchSpecError::invalid_handshake_key);
  const auto rtt_ms = rtt_microseconds / microseconds_per_millisecond;
  if (rtt_ms > std::numeric_limits<std::uint32_t>::max())
    return unexpected(LaunchSpecError::invalid_rtt);
  auto key_text = handshake_text(handshake_key);
  if (!key_text) return unexpected(key_text.error());

  Document document;
  document.stream_resolutions[0].resolution = {video.width, video.height};
  document.stream_resolutions[0].max_fps = video.max_fps;
  document.network.bw_kbps_sent = video.bitrate_kbps;
  document.network.mtu = mtu;
  document.network.rtt = static_cast<std::uint32_t>(rtt_ms);
  if (ps5.value()) {
    document.request_game_specification.adaptive_stream_mode = "resize";
    document.video_codec = video_codec;
    document.dynamic_range = dynamic_range;
    const auto profile = audio_profile(requested_audio_channels);
    if (!profile) return unexpected(profile.error());
    if (profile.value()) {
      document.audio_channel_num_rp = *profile.value();
      document.audio_settings = Document::AudioSettings{};
    }
  }
  document.handshake_key = std::move(key_text).value();
  return written(document).and_then([&](const SecretString& plaintext) {
    return sealed(crypto, target, material, plaintext);
  });
}

auto LaunchSpecCodec::is_ps5(RegistrationTarget target)
    -> Expected<bool, LaunchSpecError> {
  switch (target) {
    case RegistrationTarget::ps4_8:
    case RegistrationTarget::ps4_9:
    case RegistrationTarget::ps4_10:
      return false;
    case RegistrationTarget::ps5_1:
      return true;
    default:
      return unexpected(LaunchSpecError::invalid_target);
  }
}

auto LaunchSpecCodec::audio_profile(std::uint8_t requested_audio_channels)
    -> Expected<std::optional<unsigned>, LaunchSpecError> {
  switch (requested_audio_channels) {
    case 0:
      return std::optional<unsigned>{};
    case 2:
      return std::optional<unsigned>{0U};
    case 6:
      return std::optional<unsigned>{1U};
    case 8:
      return std::optional<unsigned>{2U};
    case 12:
      return std::optional<unsigned>{3U};
    default:
      return unexpected(LaunchSpecError::invalid_audio_channels);
  }
}

auto LaunchSpecCodec::handshake_text(std::span<const std::byte> handshake_key)
    -> Expected<SecretString, LaunchSpecError> {
  SecretArray<32, char> encoded;
  const auto size =
      text::base64::encode_into(handshake_key, {encoded.data(), encoded.size()},
                                text::base64::Alphabet::standard);
  if (size != 24U) return unexpected(LaunchSpecError::invalid_handshake_key);
  return SecretString{std::string_view{encoded.data(), *size}};
}

auto LaunchSpecCodec::written(const Document& document)
    -> Expected<SecretString, LaunchSpecError> {
  auto text = json::ps::write(document);
  if (!text) return unexpected(LaunchSpecError::unwritable);
  if (text.value().size() >= launch_spec_json_capacity)
    return unexpected(LaunchSpecError::too_large);
  return std::move(text).value();
}

auto LaunchSpecCodec::sealed(platform::CryptoProvider& crypto,
                             RegistrationTarget target,
                             const AuthMaterial& material,
                             const SecretString& plaintext)
    -> Expected<SecretString, LaunchSpecError> {
  const auto plain_size = plaintext.size() + 1;
  SecretArray<launch_spec_json_capacity> encrypted;
  SecretArray<launch_spec_json_capacity> zeros;
  const auto transformed = RegistrationCipher::transform(
      crypto, target, {material.bright.data(), material.bright.size()},
      {material.ambassador.data(), material.ambassador.size()}, 0,
      CipherDirection::encrypt, {zeros.data(), plain_size},
      {encrypted.data(), plain_size});
  if (!transformed) return unexpected(LaunchSpecError::crypto_failure);

  const std::span<std::byte> stream{encrypted.data(), plaintext.size()};
  std::ranges::transform(stream, plaintext.bytes(), stream.begin(),
                         std::bit_xor<>{});

  return text::base64::encode<SecretString>({encrypted.data(), plain_size},
                                            text::base64::Alphabet::standard);
}

}  // namespace libchiaking::net::protocol::remote_play
