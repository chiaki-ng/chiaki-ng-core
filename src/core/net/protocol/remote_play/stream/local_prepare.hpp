/// @file
/// @brief Declares LocalStreamPreparation, which makes everything a stream
/// sends in its BIG message once calibration and the control channel are
/// done: the handshake key, the signed ECDH key, the session key and the
/// sealed launch specification.
#pragma once

#include "libchiaking/detail/expected.hpp"
#include "libchiaking/detail/secret_array.hpp"
#include "libchiaking/detail/secret_string.hpp"
#include "libchiaking/detail/wire_sizes.hpp"
#include "libchiaking/platform/runtime/random_provider.hpp"
#include "src/core/net/protocol/remote_play/auth_material.hpp"
#include "src/core/net/protocol/remote_play/ecdh_wire.hpp"
#include "src/core/net/protocol/remote_play/registered_console.hpp"
#include "src/core/net/protocol/remote_play/session_id.hpp"
#include "src/core/net/protocol/remote_play/stream/direct_route.hpp"
#include "src/core/net/protocol/remote_play/stream/launch_spec.hpp"
#include "src/core/net/protocol/remote_play/video_presets.hpp"
#include "src/core/net/protocol/senkusha/types.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>

namespace libchiaking::net::protocol::remote_play {

/// @brief Why a stream could not be prepared.
enum class LocalPrepareError : std::uint8_t {
  /// @brief The calibration's Takion version, wire profile and curve disagree.
  invalid_calibration,
  /// @brief There is no session id.
  invalid_session_id,
  /// @brief A measured MTU is 0.
  invalid_mtu,
  /// @brief The measured round trip is negative.
  invalid_rtt,
  /// @brief The session nonce is not 16 bytes.
  invalid_nonce,
  /// @brief The session material could not be derived.
  auth_material_failure,
  /// @brief Random bytes could not be had.
  random_failure,
  /// @brief The Takion version's curve has no platform equivalent.
  unsupported_curve,
  /// @brief The ECDH key could not be made.
  ecdh_failure,
  /// @brief The launch specification could not be made.
  launch_spec_failure,
};

/// @brief What a prepared stream sends in BIG.
///
/// Move-only through its secrets.
struct PreparedLocalStream {
  /// @brief The fresh 16-byte key that signs the ECDH keys.
  SecretArray<ecdh_handshake_key_size> handshake_key;
  /// @brief The ECDH key pair and its signed public key.
  PreparedEcdh prepared_ecdh;
  /// @brief The session id, sent as BIG's session key.
  std::shared_ptr<const SecretString> session_key;
  /// @brief The sealed, base64 launch specification.
  std::shared_ptr<const SecretString> encrypted_launch_spec;
};

/// @brief What preparing a stream needs. References must outlive the call.
struct LocalStreamInputs {
  /// @brief The provider for ECDH, HMAC and the cipher.
  platform::CryptoProvider& crypto;
  /// @brief Randomness for the handshake key.
  platform::SecureRandomProvider& random;
  /// @brief The registered console, with its variant and morning key.
  const RegisteredConsole& console;
  /// @brief The codec to ask for.
  VideoCodec codec;
  /// @brief The picture to ask for.
  LaunchSpecVideo video;
  /// @brief The route's calibration: Takion version, MTU and round trip.
  const senkusha::Result& calibration;
  /// @brief The session id from the control channel.
  const SessionId& session_id;
  /// @brief The 16-byte nonce from the session reply.
  std::span<const std::byte> nonce;
  /// @brief The audio channels to ask for, 0 for none.
  std::uint8_t requested_audio_channels{};
};

/// @brief Prepares local streams.
///
/// Stateless; every member is static.
class LocalStreamPreparation {
 public:
  /// @brief Replaces the picture with the smallest one when video is
  /// disabled: H.264 at 640x360, 2000 kbit/s and 0 fps.
  ///
  /// @tparam Configuration Anything with `tuning.disable_video`, `codec` and
  ///     `video` with width, height, bitrate_kbps and max_fps.
  /// @param configuration The configuration.
  /// @return It, changed when video is disabled.
  template <class Configuration>
  static auto apply_disabled_video(Configuration configuration)
      -> Configuration {
    if (configuration.tuning.disable_video) {
      configuration.codec = VideoCodec::h264;
      configuration.video.width = video_preset_360p.width;
      configuration.video.height = video_preset_360p.height;
      configuration.video.bitrate_kbps = video_preset_360p.bitrate_kbps;
      configuration.video.max_fps = 0;
    }
    return configuration;
  }

  /// @brief The Takion wire profile a console's stream uses.
  ///
  /// @param target The console variant.
  /// @param takion_version_override A version to use instead, or 0.
  /// @return The profile of the PS4 or PS5 build version (or the override),
  ///     or nothing when the target or version is unknown.
  [[nodiscard]] static auto console_active_profile(
      RegistrationTarget target, std::uint32_t takion_version_override = 0)
      -> std::optional<takion::WireProfile>;

  /// @brief A calibration to use when Senkusha could not run: the console's
  /// Takion version, a 1 ms round trip and 1454-byte MTUs.
  ///
  /// @param target The console variant.
  /// @param takion_version_override A version to use instead, or 0.
  /// @return The calibration, or nothing when the version is unknown.
  [[nodiscard]] static auto make_reference_fallback_calibration(
      RegistrationTarget target, std::uint32_t takion_version_override = 0)
      -> std::optional<senkusha::Result>;

  /// @brief The device id the control request sends.
  ///
  /// @param supplied The caller's 32-byte id; all zeros asks for a new one.
  /// @param random Randomness for a new id.
  /// @return The supplied id, or a new one of a fixed 10-byte prefix
  ///     (`00 18 00 00 00 07 00 40 00 80`), 16 random bytes and 6 zero bytes;
  ///     nothing when random bytes could not be had.
  [[nodiscard]] static auto resolve_local_did(
      const std::array<std::byte, rp_did_size>& supplied,
      platform::SecureRandomProvider& random)
      -> std::optional<std::array<std::byte, rp_did_size>>;

  /// @brief Prepares the BIG contents.
  ///
  /// Derives the session's AuthMaterial from the nonce and the morning key,
  /// makes a fresh handshake key and ECDH key pair on the Takion version's
  /// curve, and seals the launch specification with the inbound MTU and the
  /// adjusted mean round trip.
  ///
  /// @param inputs What to prepare from.
  /// @return The prepared contents.
  /// @retval LocalPrepareError Why it could not be prepared.
  [[nodiscard]] static auto prepare(const LocalStreamInputs& inputs)
      -> Expected<PreparedLocalStream, LocalPrepareError>;
};

}  // namespace libchiaking::net::protocol::remote_play
