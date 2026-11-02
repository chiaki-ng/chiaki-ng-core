/// @file
/// @brief Declares LaunchSpecCodec, the encoder of the launch specification
/// a client sends in its Takion BIG message.
///
/// The launch specification is a camelCase JSON document: the picture size
/// and frame rate, the bitrate, MTU and round trip, fixed values the Windows
/// client sends (session id, app specification, user and request profiles),
/// for a PS5 the codec, dynamic range and optional audio channel profile, and
/// the base64 handshake key. It is sealed with the session's AuthMaterial at
/// counter 0 as the cipher's keystream of zeros XORed with the text, a NUL
/// included, then base64-encoded.
#pragma once

#include <libchiaking/detail/secret_string.hpp>
#include "libchiaking/detail/expected.hpp"
#include "libchiaking/platform/crypto/provider.hpp"
#include "src/core/net/protocol/remote_play/auth_material.hpp"
#include "src/core/net/protocol/remote_play/ctrl_codec.hpp"
#include "src/core/net/protocol/remote_play/registered_console.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace libchiaking::net::protocol::remote_play {

/// @brief Why a launch specification could not be made.
enum class LaunchSpecError : std::uint8_t {
  /// @brief The target is not a known variant.
  invalid_target,
  /// @brief The codec is outside the enumeration.
  invalid_codec,
  /// @brief The MTU is 0.
  invalid_mtu,
  /// @brief The handshake key is not 16 bytes.
  invalid_handshake_key,
  /// @brief The round trip in milliseconds does not fit 32 bits.
  invalid_rtt,
  /// @brief The JSON reached launch_spec_json_capacity bytes.
  too_large,
  /// @brief The cipher failed.
  crypto_failure,
  /// @brief The audio channel count is not 0, 2, 6, 8 or 12.
  invalid_audio_channels,
  /// @brief The JSON could not be written.
  unwritable,
};

/// @brief The picture a launch specification asks for.
///
/// Plain data.
struct LaunchSpecVideo {
  /// @brief Width in pixels.
  std::uint32_t width{};
  /// @brief Height in pixels.
  std::uint32_t height{};
  /// @brief Highest frame rate.
  std::uint32_t max_fps{};
  /// @brief Bitrate in kilobits per second (`bwKbpsSent`).
  std::uint32_t bitrate_kbps{};
};

/// @brief Encodes launch specifications.
///
/// Stateless; every member is static.
class LaunchSpecCodec {
 public:
  /// @brief Encodes and seals a launch specification.
  ///
  /// A PS5 also gets `adaptiveStreamMode: "resize"`, `videoCodec` ("avc" or
  /// "hevc"), `dynamicRange` ("SDR", or "AUTO" for HDR) and, when channels are
  /// requested, `audioChannelNumRP` (0 stereo, 1 5.1, 2 7.1, 3 7.1.4) with the
  /// matching audio settings.
  ///
  /// @param crypto The provider that runs the cipher.
  /// @param target The console variant.
  /// @param codec The codec (PS5 only).
  /// @param video The picture.
  /// @param mtu The measured MTU, in bytes.
  /// @param rtt_microseconds The measured round trip, sent in milliseconds.
  /// @param handshake_key The 16-byte handshake key.
  /// @param material The session's material.
  /// @param requested_audio_channels The channels to ask for, 0 for none.
  /// @return The base64 of the sealed document.
  /// @retval LaunchSpecError Why it could not be made.
  [[nodiscard]] static auto encode(
      platform::CryptoProvider& crypto, RegistrationTarget target,
      VideoCodec codec, const LaunchSpecVideo& video, std::uint32_t mtu,
      std::uint64_t rtt_microseconds, std::span<const std::byte> handshake_key,
      const AuthMaterial& material, std::uint8_t requested_audio_channels = 0)
      -> Expected<SecretString, LaunchSpecError>;

 private:
  /// @brief The JSON document's shape.
  struct Document;

  /// @brief Whether a target is a PS5.
  ///
  /// @param target The variant.
  /// @return True for PS5, false for a PS4.
  /// @retval LaunchSpecError::invalid_target Any other value.
  static auto is_ps5(RegistrationTarget target)
      -> Expected<bool, LaunchSpecError>;
  /// @brief The `audioChannelNumRP` value of a channel request.
  ///
  /// @param requested_audio_channels 0, 2, 6, 8 or 12.
  /// @return Nothing for 0, else 0 to 3.
  /// @retval LaunchSpecError::invalid_audio_channels Any other count.
  static auto audio_profile(std::uint8_t requested_audio_channels)
      -> Expected<std::optional<unsigned>, LaunchSpecError>;
  /// @brief The base64 of the handshake key.
  ///
  /// @param handshake_key The 16-byte key.
  /// @return The 24 characters.
  /// @retval LaunchSpecError::invalid_handshake_key The key is not 16 bytes.
  [[nodiscard]] static auto handshake_text(
      std::span<const std::byte> handshake_key)
      -> Expected<SecretString, LaunchSpecError>;
  /// @brief Writes the document.
  ///
  /// @param document The document.
  /// @return The JSON.
  /// @retval LaunchSpecError::unwritable Writing failed.
  /// @retval LaunchSpecError::too_large It is too long.
  [[nodiscard]] static auto written(const Document& document)
      -> Expected<SecretString, LaunchSpecError>;
  /// @brief Seals the JSON and encodes it in base64.
  ///
  /// @param crypto The provider that runs the cipher.
  /// @param target The console variant.
  /// @param material The session's material.
  /// @param plaintext The JSON.
  /// @return The base64 text.
  /// @retval LaunchSpecError::crypto_failure The cipher failed.
  [[nodiscard]] static auto sealed(platform::CryptoProvider& crypto,
                                   RegistrationTarget target,
                                   const AuthMaterial& material,
                                   const SecretString& plaintext)
      -> Expected<SecretString, LaunchSpecError>;
};

}  // namespace libchiaking::net::protocol::remote_play
