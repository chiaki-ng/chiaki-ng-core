/// @file
/// @brief Declares SOURCE_SETUP, the decoder and key material a Kakusan host
/// gives an admitted guest, and its validation.
#pragma once

#include "libchiaking/detail/secret_array.hpp"
#include "src/core/net/kakusan/result.hpp"
#include "src/core/net/protocol/takion/profile.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace libchiaking::net::kakusan::control {

/// @brief Widest video profile accepted, in pixels.
inline constexpr std::uint32_t max_video_width = 1920;
/// @brief Tallest video profile accepted, in pixels.
inline constexpr std::uint32_t max_video_height = 1080;
/// @brief Highest source frame rate accepted.
inline constexpr std::uint32_t max_video_fps = 60;
/// @brief The only source epoch: the protocol has no source rekeying.
inline constexpr std::uint64_t source_epoch = 1;
/// @brief The wire name of H.264.
inline constexpr std::string_view source_codec_h264 = "h264";
/// @brief The wire name of H.265.
inline constexpr std::string_view source_codec_h265 = "h265";
/// @brief Most video profiles in a setup.
inline constexpr std::size_t max_video_profiles = 8;
/// @brief Largest codec initialisation buffer of a profile, in bytes.
inline constexpr std::size_t max_profile_header_size = 512;
/// @brief Bytes of the source's audio header.
inline constexpr std::size_t audio_header_size = 14;

/// @brief The host's random id for one console session, the same for every
/// guest.
using SourceSessionId = std::array<std::byte, 16>;
/// @brief The source's original 14-byte audio header.
using AudioHeader = std::array<std::byte, audio_header_size>;

/// @brief A 16-byte secret of the source session given to guests: the derived
/// incoming GKCrypt key base or IV.
///
/// Wiped on destruction; move-only, clone() copies. Compared in constant time.
class SourceKey {
 public:
  /// @brief Bytes of the key.
  static constexpr std::size_t extent = 16;

  /// @brief An all-zero key.
  SourceKey() noexcept = default;
  /// @brief Copies a key into wiped storage.
  ///
  /// @param bytes The key.
  SourceKey(const std::array<std::byte, extent>& bytes) noexcept;
  /// @brief Takes a key.
  ///
  /// @param bytes The key.
  explicit SourceKey(SecretArray<extent> bytes) noexcept;
  /// @brief Not copyable; use clone().
  SourceKey(const SourceKey&) = delete;
  /// @brief Takes another key's bytes.
  SourceKey(SourceKey&&) noexcept = default;
  /// @brief Not copyable.
  ///
  /// @return Never returns.
  auto operator=(const SourceKey&) -> SourceKey& = delete;
  /// @brief Takes another key's bytes.
  ///
  /// @return This key.
  auto operator=(SourceKey&&) noexcept -> SourceKey& = default;
  /// @brief Wipes the key.
  ~SourceKey() = default;

  /// @brief Copies the key.
  ///
  /// @return The copy.
  auto clone() const noexcept -> SourceKey { return SourceKey{bytes_.clone()}; }
  /// @brief Hands over the key's storage.
  ///
  /// @return The bytes.
  auto secret() && noexcept -> SecretArray<extent> { return std::move(bytes_); }

  /// @brief The key's bytes.
  ///
  /// @return A pointer to 16 bytes.
  auto data() noexcept -> std::byte* { return bytes_.data(); }
  /// @brief The key's bytes.
  ///
  /// @return A pointer to 16 bytes.
  auto data() const noexcept -> const std::byte* { return bytes_.data(); }
  /// @brief The key's size.
  ///
  /// @return 16.
  constexpr auto size() const noexcept -> std::size_t { return extent; }
  /// @brief Start of the bytes.
  ///
  /// @return A pointer to the first.
  auto begin() noexcept -> std::byte* { return bytes_.begin(); }
  /// @brief End of the bytes.
  ///
  /// @return A pointer past the last.
  auto end() noexcept -> std::byte* { return bytes_.end(); }
  /// @brief Start of the bytes.
  ///
  /// @return A pointer to the first.
  auto begin() const noexcept -> const std::byte* { return bytes_.begin(); }
  /// @brief End of the bytes.
  ///
  /// @return A pointer past the last.
  auto end() const noexcept -> const std::byte* { return bytes_.end(); }
  /// @brief One byte.
  ///
  /// @param index 0 to 15.
  /// @return The byte.
  auto operator[](std::size_t index) noexcept -> std::byte& {
    return bytes_[index];
  }
  /// @brief One byte.
  ///
  /// @param index 0 to 15.
  /// @return The byte.
  auto operator[](std::size_t index) const noexcept -> const std::byte& {
    return bytes_[index];
  }
  /// @brief Compares in constant time.
  ///
  /// @param other The other key.
  /// @return True when equal.
  [[nodiscard]] auto operator==(const SourceKey& other) const noexcept -> bool;

 private:
  /// @brief The key.
  SecretArray<extent> bytes_;
};

/// @brief One adaptive video stream of the source, in stream index order.
struct VideoProfile {
  /// @brief Width in pixels, 1 to 1920.
  std::uint16_t width{};
  /// @brief Height in pixels, 1 to 1080.
  std::uint16_t height{};
  /// @brief The codec initialisation buffer (Annex B parameter sets), 1 to
  /// 512 bytes.
  std::vector<std::byte> header;
};

/// @brief The source's video codec.
enum class SourceCodec : std::uint8_t {
  /// @brief H.264, Baseline or High profile, level up to 4.2.
  h264,
  /// @brief H.265, Main or Main 10 profile, level up to 5.1.
  h265,
};

/// @brief The SOURCE_SETUP message: what a guest needs to decrypt and decode
/// the forwarded stream (RCP WIRE section 9).
///
/// Carries the source session's derived incoming key and IV, so it is sent only
/// inside DTLS after pairing and admission. Move-only through its keys; clone()
/// copies.
struct SourceSetup {
  /// @brief The message type.
  static constexpr std::string_view wire_name = "SOURCE_SETUP";
  /// @brief Highest H.264 level_idc accepted: 4.2.
  static constexpr std::uint8_t max_h264_level_idc = 42;
  /// @brief Highest H.265 general_level_idc accepted: 5.1.
  static constexpr std::uint8_t max_h265_level_idc = 153;

  /// @brief Checks the audio header: channels (1 byte, 1 or 2), bits per
  /// sample
  /// (1 byte, 16), sample rate (u32 big-endian, 48000) and Opus frame size in
  /// samples (u32 big-endian, 120 to 2880); the last 4 bytes are not checked.
  ///
  /// @param header The header.
  /// @return Nothing when valid.
  /// @retval Error::codec It is not.
  static auto validate_audio_header(std::span<const std::byte> header)
      -> Result<void>;
  /// @brief Checks that the first H.264 SPS is Baseline or High at a level
  /// from
  /// 1 to 4.2.
  ///
  /// @param header The Annex B buffer.
  /// @return Nothing when valid.
  /// @retval Error::codec No SPS, or an unsupported profile or level.
  static auto validate_h264_profile_header(std::span<const std::byte> header)
      -> Result<void>;
  /// @brief Checks that the first H.265 SPS is Main or Main 10 at a level
  /// from
  /// 1 to 5.1, reading past emulation prevention bytes.
  ///
  /// @param header The Annex B buffer.
  /// @return Nothing when valid.
  /// @retval Error::codec No SPS, or an unsupported profile or level.
  static auto validate_h265_profile_header(std::span<const std::byte> header)
      -> Result<void>;
  /// @brief Tells the codec from the first SPS in a buffer.
  ///
  /// @param header The Annex B buffer.
  /// @return The codec.
  /// @retval Error::codec No SPS found.
  static auto detect_codec(std::span<const std::byte> header)
      -> Result<SourceCodec>;
  /// @brief Checks a profile header for a codec.
  ///
  /// @param codec The codec.
  /// @param header The Annex B buffer.
  /// @return Nothing when valid.
  /// @retval Error::codec It is not.
  static auto validate_profile_header(SourceCodec codec,
                                      std::span<const std::byte> header)
      -> Result<void>;
  /// @brief Whether a frame rate is accepted.
  ///
  /// @param fps The rate.
  /// @return True for 1 to 60.
  static auto valid_fps(std::uint32_t fps) noexcept -> bool;
  /// @brief Whether a Takion version may be forwarded to Kakusan guests.
  ///
  /// @param version The source session's Takion version.
  /// @return True for a version the library knows that does not use the
  ///     version 7 A/V layout and is not reserved for PS cloud streaming:
  ///     in practice 9 through 20.
  static constexpr auto supported_protocol(std::uint64_t version) noexcept
      -> bool {
    if (version > UINT8_MAX) return false;
    const auto narrowed = static_cast<std::uint32_t>(version);
    const auto profile =
        net::protocol::takion::resolve_takion_wire_profile(narrowed);
    return profile.has_value() &&
           profile->av_layout() != net::protocol::takion::AvLayout::version_7 &&
           !net::protocol::takion::is_cloud_stream_only_takion_version(
               narrowed);
  }
  /// @brief Whether a profile's size and header length are accepted.
  ///
  /// @param width Width in pixels.
  /// @param height Height in pixels.
  /// @param header_size Header bytes.
  /// @return True for 1 to 1920 by 1 to 1080 with 1 to 512 header bytes.
  static auto valid_geometry(std::uint32_t width, std::uint32_t height,
                             std::size_t header_size) noexcept -> bool;

  /// @brief Whether the fields are in range, without parsing headers.
  ///
  /// @return True for a supported source version, a valid frame rate, and 1
  ///     to
  ///     8 valid profiles.
  auto well_formed() const noexcept -> bool;
  /// @brief Checks the fields and the audio header.
  ///
  /// @return Nothing when valid.
  /// @retval Error::codec They are not.
  auto validate() const -> Result<void>;
  /// @brief Checks every profile header against the codec.
  ///
  /// @return Nothing when valid.
  /// @retval Error::codec One is not.
  auto validate_profile_headers() const -> Result<void>;
  /// @brief Copies the setup, keys into new storage.
  ///
  /// @return The copy.
  auto clone() const -> SourceSetup;

  /// @brief The source session's id.
  SourceSessionId source_session_id{};
  /// @brief The source's Takion version.
  std::uint8_t source_protocol{};
  /// @brief The derived remote GKCrypt key base.
  SourceKey receive_key{};
  /// @brief The derived remote GKCrypt IV.
  SourceKey receive_iv{};
  /// @brief The source frame rate, for the guest's pacing.
  std::uint8_t video_fps{};
  /// @brief The video profiles in stream index order.
  std::vector<VideoProfile> video_profiles;
  /// @brief The original audio header.
  AudioHeader audio_header{};
  /// @brief The codec of every profile.
  SourceCodec source_codec{SourceCodec::h264};
};

}  // namespace libchiaking::net::kakusan::control
